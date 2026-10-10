#include "sqlite_database.h"
#include "sqlite_query.h"
#include "sql_mm.h"
#include "tier0/dbg.h"

#include "operations/sqlite_connectop.h"
#include "operations/sqlite_queryop.h"
#include "operations/sqlite_transactop.h"

extern std::vector<SqConnection *> g_vecSqliteConnections;
extern bool g_bInFrame;

SqConnection::SqConnection(const SQLiteConnectionInfo info)
{
    char *database = new char[strlen(info.database) + 1];
    strncopy(database, info.database, strlen(info.database) + 1);
    this->info.database = database;
}

SqConnection::~SqConnection()
{
    ConMsg("Destroying SQLite connection %s\n", info.database);
    if (m_thread)
    {
        {
            std::lock_guard<std::mutex> lock(m_Lock);
            m_Terminate = true;
            m_QueueEvent.notify_all();
        }

        m_thread->join();
        m_thread.reset();
        m_Terminate = false;
    }

    while (!m_ThinkQueue.empty())
    {
        ThreadOperation *op = m_ThinkQueue.front();
        m_ThinkQueue.pop();

        op->CancelThinkPart();
        delete op;
    }

    if (m_pDatabase)
    {
        sqlite3_close_v2(m_pDatabase);
    }
    delete[] this->info.database;
}

void SqConnection::Connect(ConnectCallbackFunc callback)
{
    TSQLiteConnectOp *op = new TSQLiteConnectOp(this, callback);

    AddToThreadQueue(op);
}

void SqConnection::Query(char *query, QueryCallbackFunc callback)
{
    TSQLiteQueryOp *op = new TSQLiteQueryOp(this, std::string(query), callback);

    if (!m_pDatabase)
    {
        op->SetError("Database is not connected");
        AddToThinkQueue(op);
        return;
    }

    AddToThreadQueue(op);
}

void SqConnection::Query(const char *query, QueryCallbackFunc callback, ...)
{
    va_list args;
    va_start(args, callback);

    va_list vaCopy;
    va_copy(vaCopy, args);
    const int iLen = std::vsnprintf(NULL, 0, query, vaCopy);
    va_end(vaCopy);

    std::vector<char> zc(iLen + 1);
    std::vsnprintf(zc.data(), zc.size(), query, args);
    va_end(args);

    TSQLiteQueryOp *op = new TSQLiteQueryOp(this, std::string(zc.data(), zc.size()), callback);

    if (!m_pDatabase)
    {
        op->SetError("Database is not connected");
        AddToThinkQueue(op);
        return;
    }

    AddToThreadQueue(op);
}

void SqConnection::ExecuteTransaction(Transaction txn, TransactionSuccessCallbackFunc success, TransactionFailureCallbackFunc failure)
{
    TSQLiteTransactOp *op = new TSQLiteTransactOp(this, txn, success, failure);

    if (!m_pDatabase)
    {
        op->SetError("Database is not connected");
        AddToThinkQueue(op);
        return;
    }

    AddToThreadQueue(op);
}

void SqConnection::Destroy()
{
    if (g_bInFrame)
    {
        m_bDestroyed = true;
        return;
    }

    g_vecSqliteConnections.erase(std::remove(g_vecSqliteConnections.begin(), g_vecSqliteConnections.end(), this), g_vecSqliteConnections.end());
    delete this;
}

void SqConnection::RunFrame()
{
    size_t count;
    {
        std::lock_guard<std::mutex> lock(m_ThinkLock);
        count = m_ThinkQueue.size();
    }

    // Only the ops finished before this frame, so a callback chain of fast queries can't stall the frame.
    // A callback may Destroy() the connection; its remaining ops are cancelled by the destructor.
    for (; count > 0 && !m_bDestroyed; count--)
    {
        ThreadOperation *op;
        {
            std::lock_guard<std::mutex> lock(m_ThinkLock);
            op = m_ThinkQueue.front();
            m_ThinkQueue.pop();
        }

        op->RunThinkPart();
        delete op;
    }
}

std::string SqConnection::Escape(char *string)
{
    std::string result(strlen(string) * 2 + 1, ' ');
    sqlite3_snprintf(result.size(), result.data(), "%q", string);
    result.resize(strlen(result.c_str()));
    return result;
}

std::string SqConnection::Escape(const char *string)
{
    return Escape(const_cast<char *>(string));
}

SqQuery *SqConnection::PrepareQuery(const char *query, char *error, size_t maxlength, int *errCode)
{
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    if ((m_LastErrorCode = sqlite3_prepare_v2(m_pDatabase, query, -1, &stmt, &tail)) != SQLITE_OK || !stmt)
    {
        const char *msg;
        if (m_LastErrorCode != SQLITE_OK)
        {
            msg = sqlite3_errmsg(m_pDatabase);
        }
        else
        {
            msg = "Invalid query string";
            m_LastErrorCode = SQLITE_ERROR;
        }
        if (error)
        {
            strncopy(error, msg, maxlength);
        }
        m_LastError.assign(msg);
        return NULL;
    }

    // Only the first statement gets compiled, so reject the query instead of silently skipping the rest.
    // Preparing a tail of only whitespace, semicolons or comments succeeds with no statement.
    sqlite3_stmt *next = NULL;
    if (sqlite3_prepare_v2(m_pDatabase, tail, -1, &next, NULL) != SQLITE_OK || next)
    {
        sqlite3_finalize(next);
        sqlite3_finalize(stmt);
        const char *msg = "Only one statement per query is supported";
        if (error)
        {
            strncopy(error, msg, maxlength);
        }
        m_LastErrorCode = SQLITE_ERROR;
        m_LastError.assign(msg);
        return NULL;
    }
    return new SqQuery(this, stmt);
}

void SqConnection::ThreadRun()
{
    std::unique_lock<std::mutex> lock(m_Lock);

    while (true)
    {
        if (m_threadQueue.empty())
        {
            if (m_Terminate)
            {
                return;
            }

            m_QueueEvent.wait(lock);
            continue;
        }

        ThreadOperation *op = m_threadQueue.front();
        m_threadQueue.pop();

        lock.unlock();
        op->RunThreadPart();

        {
            std::lock_guard<std::mutex> think_lock(m_ThinkLock);
            m_ThinkQueue.push(op);
        }

        lock.lock();
    }
}

void SqConnection::AddToThreadQueue(ThreadOperation *threadOperation)
{
    if (!m_thread)
    {
        m_thread = std::unique_ptr<std::thread>(new std::thread(&SqConnection::ThreadRun, this));
    }

    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_threadQueue.push(threadOperation);
        m_QueueEvent.notify_one();
    }
}

// Hands an op that never needs the worker thread straight to RunFrame, so its callback still fires on a later frame.
void SqConnection::AddToThinkQueue(ThreadOperation *threadOperation)
{
    std::lock_guard<std::mutex> lock(m_ThinkLock);
    m_ThinkQueue.push(threadOperation);
}
