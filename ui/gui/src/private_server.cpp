#include <QDir>
#include <QHostAddress>
#include <QTcpServer>

#include "gui/private_server.hpp"

namespace
{
    constexpr int kProbeIntervalMs = 60;

    /** What connectx-server prints once it listens (network/server/src/game_server.cpp). */
    const char *const kReadyBanner = "Server started successfully";

    /** Asks the operating system for a free port of this computer (listen on 0, read it, close). */
    quint16 askForFreePort()
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0))
        {
            return 0;
        }
        const quint16 port = probe.serverPort();
        probe.close();
        return port;
    }
}

PrivateServer::PrivateServer(QObject *parent)
    : QObject(parent),
      process_(this),
      probeTimer_(this),
      probeSocket_(this)
{
    connect(&process_, &HelperProcess::startFailed, this, &PrivateServer::onStartFailed);
    connect(&process_, &HelperProcess::finished, this, &PrivateServer::onFinished);
    connect(&process_, &HelperProcess::outputLine, this, &PrivateServer::onOutputLine);

    connect(&probeTimer_, &QTimer::timeout, this, &PrivateServer::probe);
    connect(&probeSocket_, &QTcpSocket::connected, this, [this]()
            {
        probeSocket_.abort();
        markReady(); });
    // (a refused connection just means "not yet": the next tick tries again)
}

PrivateServer::~PrivateServer()
{
    disconnect();
    stopAndWait(); // before the working directory goes: the server has its log open in it
}

void PrivateServer::start(const QString &program)
{
    stopAndWait();

    program_ = program;
    if (program.isEmpty())
    {
        emit failed(tr("The server program (connectx-server) was not found."));
        return;
    }

    port_ = askForFreePort();
    if (port_ == 0)
    {
        emit failed(tr("Could not find a free port on this computer."));
        return;
    }

    workDir_ = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("connectx-private-server-XXXXXX")));
    starting_ = true;
    ready_ = false;
    sinceStart_.start();

    // --bind: only this computer can reach the server (no firewall prompt, nobody else on the network can join)
    process_.start(program,
                   {QStringLiteral("--port"), QString::number(port_), QStringLiteral("--bind"), QStringLiteral("127.0.0.1")},
                   workDir_->isValid() ? workDir_->path() : QString());
    if (starting_) // (a program that cannot be started has already reported it)
    {
        probeTimer_.start(kProbeIntervalMs);
    }
}

void PrivateServer::stop()
{
    stopProbing();
    starting_ = false;
    ready_ = false;
    process_.stop();
}

void PrivateServer::stopAndWait()
{
    stopProbing();
    starting_ = false;
    ready_ = false;
    process_.stopAndWait();
    workDir_.reset();
}

void PrivateServer::stopProbing()
{
    probeTimer_.stop();
    probeSocket_.abort();
}

void PrivateServer::probe()
{
    if (!starting_)
    {
        probeTimer_.stop();
        return;
    }

    if (sinceStart_.elapsed() > startTimeoutMs_)
    {
        giveUp(tr("The private server did not start listening in time."));
        return;
    }

    if (probeSocket_.state() == QAbstractSocket::UnconnectedState)
    {
        probeSocket_.connectToHost(QHostAddress::LocalHost, port_);
    }
}

void PrivateServer::onOutputLine(const QString &line)
{
    // The banner says the port is open, which saves a probe connection (the server has to account for each).
    if (starting_ && line.contains(QLatin1String(kReadyBanner)))
    {
        markReady();
    }
}

void PrivateServer::markReady()
{
    if (!starting_)
    {
        return;
    }
    starting_ = false;
    ready_ = true;
    stopProbing();
    emit ready(port_);
}

void PrivateServer::giveUp(const QString &message)
{
    starting_ = false;
    ready_ = false;
    stopProbing();
    process_.stop();

    const QString output = process_.outputText();
    emit failed(output.isEmpty() ? message : message + QLatin1Char('\n') + output);
}

void PrivateServer::onStartFailed(const QString &error)
{
    giveUp(tr("Could not start the private server (%1): %2").arg(program_, error));
}

void PrivateServer::onFinished(int exitCode, bool crashed, bool stopRequested)
{
    const bool wasStarting = starting_;
    const bool wasReady = ready_;

    stopProbing();
    starting_ = false;
    ready_ = false;
    workDir_.reset(); // the server is gone: its folder can go too

    if (stopRequested)
    {
        return;
    }

    const QString what = crashed ? tr("The private server stopped unexpectedly.")
                                 : tr("The private server stopped (exit code %1).").arg(exitCode);
    const QString output = process_.outputText();
    const QString text = output.isEmpty() ? what : what + QLatin1Char('\n') + output;

    if (wasStarting)
    {
        emit failed(text);
    }
    else if (wasReady)
    {
        emit died(text);
    }
}
