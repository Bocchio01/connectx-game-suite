#pragma once

#include <QString>
#include <QTest>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h> // (WIN32_LEAN_AND_MEAN is defined by the build: no winsock.h here)
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

/** Helpers of the tests that run (or stand in for) helper programs. */
namespace ProcessSupport
{
    /**
     * Whether a process with this id is alive. Always by id, never by name: other sessions run programs
     * with the same names, and nothing here may touch them.
     */
    inline bool isAlive(qint64 processId)
    {
        if (processId <= 0)
        {
            return false;
        }
#ifdef Q_OS_WIN
        HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
        if (handle == nullptr)
        {
            return false; // no such process
        }
        const DWORD state = WaitForSingleObject(handle, 0);
        CloseHandle(handle);
        return state == WAIT_TIMEOUT; // (a process that has ended is signalled, even while somebody holds a handle)
#else
        return kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
#endif
    }

    /** Waits until the process is gone; false if it is still there after `timeoutMs`. */
    inline bool waitUntilGone(qint64 processId, int timeoutMs = 4000)
    {
        return QTest::qWaitFor([processId]()
                               { return !isAlive(processId); },
                               timeoutMs);
    }

    /** The stand-in for the helper programs (tests/fake_helper.cpp), built together with the tests. */
    inline QString fakeHelper()
    {
#ifdef CONNECTX_FAKE_HELPER
        return QString::fromUtf8(CONNECTX_FAKE_HELPER);
#else
        return QString();
#endif
    }

    /** Sets an environment variable for the duration of a scope (the programs started meanwhile inherit it). */
    class EnvironmentGuard
    {
    public:
        EnvironmentGuard(const char *name, const QByteArray &value) : name_(name) { qputenv(name, value); }
        ~EnvironmentGuard() { qunsetenv(name_); }
        EnvironmentGuard(const EnvironmentGuard &) = delete;
        EnvironmentGuard &operator=(const EnvironmentGuard &) = delete;

    private:
        const char *name_;
    };
}
