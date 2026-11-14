#include "gui/computer_player.hpp"

QStringList ComputerPlayer::arguments(const Options &options)
{
    // --games 1: one game, then the program ends by itself (exit code 0), which is how we know it is over.
    return {QStringLiteral("--host"), options.host,
            QStringLiteral("--port"), QString::number(options.port),
            QStringLiteral("-g"), QString::number(options.gameId),
            QStringLiteral("--agent"), options.agent,
            QStringLiteral("-n"), options.playerName,
            QStringLiteral("--games"), QStringLiteral("1"),
            QStringLiteral("--delay"), QString::number(options.delayMs)};
}

ComputerPlayer::ComputerPlayer(QObject *parent)
    : QObject(parent),
      process_(this)
{
    connect(&process_, &HelperProcess::started, this, &ComputerPlayer::started);
    connect(&process_, &HelperProcess::startFailed, this, &ComputerPlayer::onStartFailed);
    connect(&process_, &HelperProcess::finished, this, &ComputerPlayer::onFinished);
    connect(&process_, &HelperProcess::outputLine, this, &ComputerPlayer::outputLine);
}

ComputerPlayer::~ComputerPlayer()
{
    // (the HelperProcess member kills the program synchronously when it is destroyed)
    disconnect();
}

void ComputerPlayer::start(const Options &options)
{
    program_ = options.program;
    exitCode_ = -1;

    if (options.program.isEmpty())
    {
        // Nothing to run: report it the same way as a program that cannot be started.
        emit failed(tr("The computer player (connectx-ai) was not found."), QString());
        return;
    }

    process_.start(options.program, arguments(options));
}

void ComputerPlayer::stop()
{
    process_.stop();
}

void ComputerPlayer::stopAndWait()
{
    process_.stopAndWait();
}

void ComputerPlayer::onStartFailed(const QString &error)
{
    emit failed(tr("Could not start the computer player (%1): %2").arg(program_, error), QString());
}

void ComputerPlayer::onFinished(int exitCode, bool crashed, bool stopRequested)
{
    if (stopRequested)
    {
        emit stopped();
        return;
    }

    exitCode_ = crashed ? -1 : exitCode;

    if (crashed)
    {
        emit failed(tr("The computer player stopped unexpectedly."), process_.outputText());
    }
    else if (exitCode != 0)
    {
        emit failed(tr("The computer player stopped with an error (exit code %1).").arg(exitCode), process_.outputText());
    }
    else
    {
        emit finished();
    }
}
