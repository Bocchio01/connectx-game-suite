#pragma once

#include <memory>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include "gui/helper_process.hpp"

/**
 * A server of our own for offline games: `connectx-server` run as a child process on a free port of this
 * computer (`--bind 127.0.0.1`: nobody else can reach it), which lives as long as the offline session
 * and never longer.
 *
 * start() asks the operating system for a free port, starts the program and waits until the port accepts
 * connections (the program announces itself on its output, and the port is probed anyway); ready() or
 * failed() follow. Not a place for UI: the window decides what to tell the user.
 */
class PrivateServer : public QObject
{
    Q_OBJECT

public:
    /** How long the server may take to start listening. */
    static constexpr int kDefaultStartTimeoutMs = 5000;

    explicit PrivateServer(QObject *parent = nullptr);
    ~PrivateServer() override;

    /** For the tests: the time allowed to start listening. */
    void setStartTimeout(int milliseconds) { startTimeoutMs_ = milliseconds; }

    /** Starts the server (a previous one is stopped first). ready() or failed() follow. */
    void start(const QString &program);

    /** Ends the server: asks it to end and kills it after a second. Returns at once. */
    void stop();

    /** Ends the server and returns when it is gone. */
    void stopAndWait();

    /** True from start() until the server has ended. */
    bool isRunning() const { return process_.isRunning(); }

    /** True once the port accepts connections, until the server ends. */
    bool isReady() const { return ready_; }

    quint16 port() const { return port_; }
    qint64 processId() const { return process_.processId(); }
    QString output() const { return process_.outputText(); }

signals:
    /** The server accepts connections on `port` (of this computer). */
    void ready(quint16 port);

    /** The server did not come up: could not be started, died, or never listened. */
    void failed(const QString &message);

    /** The server ended by itself after it had been ready. */
    void died(const QString &message);

private slots:
    void onStartFailed(const QString &error);
    void onFinished(int exitCode, bool crashed, bool stopRequested);
    void onOutputLine(const QString &line);
    void probe();

private:
    void markReady();
    void giveUp(const QString &message);
    void stopProbing();

    HelperProcess process_;
    QTimer probeTimer_;
    QTcpSocket probeSocket_;
    QElapsedTimer sinceStart_;
    std::unique_ptr<QTemporaryDir> workDir_; // the server writes its log in its working directory
    QString program_;
    quint16 port_ = 0;
    int startTimeoutMs_ = kDefaultStartTimeoutMs;
    bool starting_ = false;
    bool ready_ = false;
};
