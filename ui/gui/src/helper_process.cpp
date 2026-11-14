#include "gui/helper_process.hpp"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
    /** A line of output that never ends is cut here (the programs we run print short lines). */
    constexpr int kMaxLineBytes = 64 * 1024;

#ifdef Q_OS_WIN
    /**
     * Puts a child in a job object that the system closes when this program ends, whatever the way (a crash,
     * the task manager): "kill on close" ends the children with us, so that a private server can never be
     * left behind. Best effort, a failure only means that the cleanup relies on the normal paths.
     */
    void endWithThisProgram(qint64 processId)
    {
        static const HANDLE job = []() -> HANDLE
        {
            HANDLE handle = CreateJobObjectW(nullptr, nullptr);
            if (handle == nullptr)
            {
                return nullptr;
            }

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
            ZeroMemory(&limits, sizeof(limits));
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            {
                CloseHandle(handle);
                return nullptr;
            }
            return handle; // (never closed: the system closes it when the program ends, which is the point)
        }();

        if (job == nullptr || processId <= 0)
        {
            return;
        }

        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(processId));
        if (process != nullptr)
        {
            AssignProcessToJobObject(job, process);
            CloseHandle(process);
        }
    }
#endif
}

HelperProcess::HelperProcess(QObject *parent)
    : QObject(parent),
      process_(this),
      graceTimer_(this)
{
    process_.setProcessChannelMode(QProcess::MergedChannels);

#ifdef Q_OS_WIN
    // Qt creates the process without a console window when the parent has no console (the GUI), but
    // the guarantee is cheap to make explicit, and it also holds for a GUI started from a terminal.
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments)
                                               { arguments->flags |= CREATE_NO_WINDOW; });
#endif

    graceTimer_.setSingleShot(true);

    connect(&process_, &QProcess::started, this, &HelperProcess::onStarted);
    connect(&process_, &QProcess::errorOccurred, this, &HelperProcess::onError);
    connect(&process_, &QProcess::finished, this, &HelperProcess::onFinished);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &HelperProcess::onReadyRead);
    connect(&graceTimer_, &QTimer::timeout, this, &HelperProcess::onGraceElapsed);
}

HelperProcess::~HelperProcess()
{
    // Nothing may reach the owner while it is being destroyed, but the program must be gone before we are.
    disconnect();
    graceTimer_.stop();
    stopAndWait();
}

void HelperProcess::start(const QString &program, const QStringList &arguments, const QString &workingDirectory)
{
    stopAndWait();

    output_.clear();
    partialLine_.clear();
    stopRequested_ = false;
    running_ = true;

    process_.setWorkingDirectory(workingDirectory);
    process_.start(program, arguments);
}

bool HelperProcess::isRunning() const
{
    return running_;
}

qint64 HelperProcess::processId() const
{
    return process_.state() == QProcess::NotRunning ? 0 : process_.processId();
}

void HelperProcess::stop()
{
    if (process_.state() == QProcess::NotRunning)
    {
        return;
    }

    stopRequested_ = true;
    if (process_.state() == QProcess::Starting)
    {
        process_.waitForStarted(2000); // (a process cannot be stopped before it exists)
    }

#ifdef Q_OS_WIN
    // terminate() only posts WM_CLOSE to windows, and these console programs have none: asking would just
    // waste the grace period. They keep no state, so the end is the same.
    process_.kill();
#else
    process_.terminate(); // SIGTERM: both helpers stop cleanly on it
    graceTimer_.start(kGracePeriodMs);
#endif
}

void HelperProcess::stopAndWait()
{
    graceTimer_.stop();
    if (process_.state() == QProcess::NotRunning)
    {
        return;
    }

    stopRequested_ = true;
    if (process_.state() == QProcess::Starting)
    {
        process_.waitForStarted(2000);
    }

#ifdef Q_OS_WIN
    process_.kill();
#else
    process_.terminate();
    if (!process_.waitForFinished(kGracePeriodMs))
    {
        process_.kill();
    }
#endif
    process_.waitForFinished(3000);
}

void HelperProcess::onStarted()
{
#ifdef Q_OS_WIN
    endWithThisProgram(process_.processId());
#endif
    emit started();
}

void HelperProcess::onError(QProcess::ProcessError error)
{
    // A crash is followed by finished(); the other errors (reading, writing) say nothing we could act on.
    if (error == QProcess::FailedToStart)
    {
        running_ = false;
        emit startFailed(process_.errorString());
    }
}

void HelperProcess::onFinished(int exitCode, QProcess::ExitStatus status)
{
    graceTimer_.stop();
    collectOutput(true);
    running_ = false;
    emit finished(exitCode, status == QProcess::CrashExit, stopRequested_);
}

void HelperProcess::onReadyRead()
{
    collectOutput(false);
}

void HelperProcess::onGraceElapsed()
{
    if (process_.state() != QProcess::NotRunning)
    {
        process_.kill(); // it ignored the polite request
    }
}

void HelperProcess::collectOutput(bool flushPartialLine)
{
    partialLine_ += process_.readAllStandardOutput();

    auto take = [this](QByteArray line)
    {
        while (line.endsWith('\r'))
        {
            line.chop(1);
        }
        const QString text = QString::fromUtf8(line);
        if (text.trimmed().isEmpty())
        {
            return;
        }

        output_.append(text);
        while (output_.size() > kMaxOutputLines)
        {
            output_.removeFirst();
        }
        emit outputLine(text);
    };

    for (;;)
    {
        const qsizetype newline = partialLine_.indexOf('\n');
        if (newline < 0)
        {
            break;
        }
        take(partialLine_.left(newline));
        partialLine_.remove(0, newline + 1);
    }

    if (partialLine_.size() > kMaxLineBytes || (flushPartialLine && !partialLine_.isEmpty()))
    {
        take(partialLine_);
        partialLine_.clear();
    }
}
