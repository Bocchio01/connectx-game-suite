#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

/**
 * A helper program run by the GUI as a child process (the computer player, the private server).
 *
 * What every helper needs, once: its output (stdout and stderr merged, the last lines are kept to explain
 * a failure), no console window on Windows, and above all a guarantee that it never outlives its use:
 * stop() asks it to end (and kills it after kGracePeriodMs), the destructor stops it synchronously, and
 * on Windows the operating system itself kills it if the GUI dies without cleaning up.
 *
 * Not a place for decisions: the owners (ComputerPlayer, PrivateServer) decide what an exit means.
 */
class HelperProcess : public QObject
{
    Q_OBJECT

public:
    /** Time between the polite request to end and the kill. */
    static constexpr int kGracePeriodMs = 1000;

    /** Number of output lines kept. */
    static constexpr int kMaxOutputLines = 40;

    explicit HelperProcess(QObject *parent = nullptr);
    ~HelperProcess() override;

    /** Starts `program` (a previous run is stopped first). startFailed() or started() follow. */
    void start(const QString &program, const QStringList &arguments, const QString &workingDirectory = QString());

    /** Asks the program to end, and kills it after kGracePeriodMs. Returns at once; finished() tells when it is gone. */
    void stop();

    /** Same, but returns only when the program is gone. */
    void stopAndWait();

    /** True from start() until the program has ended (also while it is starting). */
    bool isRunning() const;

    /** Operating system id of the running program (0 when none). */
    qint64 processId() const;

    /** Whether the end of the last run was asked for with stop(). */
    bool wasStopped() const { return stopRequested_; }

    /** The last lines of output. */
    QStringList output() const { return output_; }
    QString outputText() const { return output_.join(QLatin1Char('\n')); }

signals:
    void started();

    /** The program could not be started (not found, not executable...). */
    void startFailed(const QString &message);

    /** The program ended. `stopped` is true when stop() asked for it. */
    void finished(int exitCode, bool crashed, bool stopped);

    void outputLine(const QString &line);

private slots:
    void onStarted();
    void onError(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onReadyRead();
    void onGraceElapsed();

private:
    void collectOutput(bool flushPartialLine);

    QProcess process_;
    QTimer graceTimer_;
    QStringList output_;
    QByteArray partialLine_;
    bool stopRequested_ = false;
    bool running_ = false;
};
