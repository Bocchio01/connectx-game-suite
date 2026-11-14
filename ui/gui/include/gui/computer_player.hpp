#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "gui/helper_process.hpp"

/**
 * The computer opponent: the `connectx-ai` helper run as a child process, which connects to the server like
 * any other client, joins a game that already exists and plays it. Nothing here knows about widgets or about
 * the network protocol: the owner builds the Options and reacts to the signals; the game itself is watched
 * through the normal game states coming from the server.
 *
 * It never outlives its use: stop() ends it, destroying the object ends it at once, and the program is
 * killed if it ignores the request to end.
 */
class ComputerPlayer : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QString program;    // full path of connectx-ai
        QString host;       // server of the game
        quint16 port = 0;
        quint32 gameId = 0; // the game to join (it must have been created and joined by the human already)
        QString agent;      // `--agent` spec, see ComputerLevels::agentSpec()
        QString playerName; // name shown to the other player
        int delayMs = 500;  // never answers faster than this, so that the human can follow
    };

    /** The command line of connectx-ai for these options (no shell is involved: nothing needs quoting). */
    static QStringList arguments(const Options &options);

    explicit ComputerPlayer(QObject *parent = nullptr);
    ~ComputerPlayer() override;

    /** Starts the program (stopping a previous one). started() or failed() follow. */
    void start(const Options &options);

    /** Ends the program, if it runs: asks it to end, kills it after a second. stopped() follows. */
    void stop();

    /** Same, but returns only when the program is gone. */
    void stopAndWait();

    /** True from start() until the program has ended. */
    bool isRunning() const { return process_.isRunning(); }

    /** Operating system id of the program (0 when it is not running); used by the tests to check for leftovers. */
    qint64 processId() const { return process_.processId(); }

    /** Exit code of the last run, -1 if it has not ended by itself. */
    int exitCode() const { return exitCode_; }

    /** The last lines the program printed. */
    QString output() const { return process_.outputText(); }

signals:
    /** The program is running (it has not joined the game yet: that shows in the game states). */
    void started();

    /** The program played its game and ended with exit code 0. */
    void finished();

    /**
     * The program could not be started, or ended by itself with an error: `message` says what happened
     * ("... exit code 1"), `output` has the last lines it printed (usually the reason).
     */
    void failed(const QString &message, const QString &output);

    /** The program ended because stop() asked for it. */
    void stopped();

    void outputLine(const QString &line);

private slots:
    void onStartFailed(const QString &error);
    void onFinished(int exitCode, bool crashed, bool stopRequested);

private:
    HelperProcess process_;
    QString program_;
    int exitCode_ = -1;
};
