#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QList>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include "gui/game_logic.hpp"
#include "gui/network_controller.hpp"

/** Helpers of the tests that talk to a real connectx-server. */
namespace TestSupport
{
    /** A TCP port nobody is listening on (best effort: it is free at the time of the call). */
    inline quint16 freePort()
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

    /** Path of the server executable built together with the tests. */
    inline QString serverExecutable()
    {
#ifdef CONNECTX_SERVER_EXE
        return QString::fromUtf8(CONNECTX_SERVER_EXE);
#else
#ifdef Q_OS_WIN
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("connectx-server.exe"));
#else
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("connectx-server"));
#endif
#endif
    }

    /** A connectx-server child process on a free port, started in a scratch directory (it writes server.log). */
    class ServerProcess
    {
    public:
        ServerProcess() = default;
        ServerProcess(const ServerProcess &) = delete;
        ServerProcess &operator=(const ServerProcess &) = delete;
        ~ServerProcess() { stop(); }

        /**
         * `preferredPort` (0: none) is tried at the first attempt; when it is taken a free one is used.
         * Only for the pictures of the README, which look better with the default port than with a random
         * one. (Never for several servers at a time: on Windows two servers can share a port.)
         */
        bool start(quint16 preferredPort = 0)
        {
            stop();

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const quint16 candidate = (attempt == 0 && preferredPort != 0) ? preferredPort : freePort();
                if (candidate == 0)
                {
                    continue;
                }

                process_ = std::make_unique<QProcess>();
                process_->setWorkingDirectory(workDir_.path());
                process_->setProcessChannelMode(QProcess::MergedChannels);
                process_->start(serverExecutable(), {QStringLiteral("--port"), QString::number(candidate)});
                // (the first launch of a freshly built executable can be slow: virus scanners look at it)
                if (!process_->waitForStarted(30000))
                {
                    continue;
                }

                // Wait for the banner: the server is listening from then on.
                output_.clear();
                QElapsedTimer clock;
                clock.start();
                while (clock.elapsed() < 30000)
                {
                    process_->waitForReadyRead(100);
                    output_ += QString::fromLocal8Bit(process_->readAll());
                    if (output_.contains(QStringLiteral("Server started successfully")))
                    {
                        port_ = candidate;
                        return true;
                    }
                    if (process_->state() == QProcess::NotRunning)
                    {
                        break; // e.g. the port was taken in the meantime: try another one
                    }
                }
                stop();
            }
            return false;
        }

        void stop()
        {
            if (process_ && process_->state() != QProcess::NotRunning)
            {
                process_->kill();
                process_->waitForFinished(3000);
            }
            process_.reset();
            port_ = 0;
        }

        bool isRunning() const { return process_ && process_->state() == QProcess::Running; }
        quint16 port() const { return port_; }
        QString output() const { return output_; }

    private:
        QTemporaryDir workDir_;
        std::unique_ptr<QProcess> process_;
        quint16 port_ = 0;
        QString output_;
    };

    /**
     * A player made of a NetworkController and a few lambdas: records everything it is told, and can
     * play by itself with a strategy (called with the new state when it is its turn).
     */
    class ScriptedPlayer
    {
    public:
        using Strategy = std::function<int(const GameStateUpdate &state, int myPlayerId)>;

        explicit ScriptedPlayer(const QString &playerName)
            : name(playerName)
        {
            NetworkController *c = &controller;
            QObject::connect(c, &NetworkController::connected, c, [this](const ConnectionInfo &)
                             { events << QStringLiteral("connected"); });
            QObject::connect(c, &NetworkController::connectionFailed, c, [this](const QString &message)
                             {
                failures << message;
                events << QStringLiteral("connectionFailed"); });
            QObject::connect(c, &NetworkController::disconnected, c, [this](const QString &reason)
                             {
                disconnectReason = reason;
                events << QStringLiteral("disconnected"); });
            QObject::connect(c, &NetworkController::gamesListed, c, [this](const QList<GameInfo> &games)
                             {
                lastList = games;
                ++listCount; });
            QObject::connect(c, &NetworkController::refreshFailed, c, [this](const QString &)
                             { ++refreshFailures; });
            QObject::connect(c, &NetworkController::joined, c, [this](const GameInfo &info, int id)
                             {
                game = info;
                playerId = id;
                events << QStringLiteral("joined"); });
            QObject::connect(c, &NetworkController::joinFailed, c, [this](const QString &message)
                             {
                failures << message;
                events << QStringLiteral("joinFailed"); });
            QObject::connect(c, &NetworkController::createFailed, c, [this](const QString &message)
                             {
                failures << message;
                events << QStringLiteral("createFailed"); });
            QObject::connect(c, &NetworkController::stateUpdated, c, [this](const GameStateUpdate &state)
                             {
                states.push_back(state);
                events << QStringLiteral("state:%1").arg(static_cast<int>(state.status));
                playIfMyTurn(state); });
            QObject::connect(c, &NetworkController::moveRejected, c, [this](const QString &message)
                             { moveErrors << message; });
            QObject::connect(c, &NetworkController::serverError, c, [this](quint16 code, const QString &message)
                             {
                serverErrors << message;
                serverErrorCodes << code; });
        }

        // ---- blocking helpers (they run the event loop) -----------------------------------------
        bool connectTo(quint16 port, int timeoutMs = 10000)
        {
            const int before = events.count(QStringLiteral("connected"));
            const int failuresBefore = static_cast<int>(failures.size());
            controller.connectToServer(QStringLiteral("127.0.0.1"), port, name);
            return QTest::qWaitFor([&]()
                                   { return events.count(QStringLiteral("connected")) > before || failures.size() > failuresBefore; },
                                   timeoutMs) &&
                   controller.isConnected();
        }

        bool createAndJoin(const GameConfig &config, const QString &gameName, int timeoutMs = 10000)
        {
            const int before = events.count(QStringLiteral("joined"));
            const int failuresBefore = static_cast<int>(failures.size());
            controller.createGame(config, gameName);
            return QTest::qWaitFor([&]()
                                   { return events.count(QStringLiteral("joined")) > before || failures.size() > failuresBefore; },
                                   timeoutMs) &&
                   events.count(QStringLiteral("joined")) > before;
        }

        bool join(quint32 gameId, int timeoutMs = 10000)
        {
            const int before = events.count(QStringLiteral("joined"));
            const int failuresBefore = static_cast<int>(failures.size());
            controller.joinGame(gameId);
            return QTest::qWaitFor([&]()
                                   { return events.count(QStringLiteral("joined")) > before || failures.size() > failuresBefore; },
                                   timeoutMs) &&
                   events.count(QStringLiteral("joined")) > before;
        }

        QList<GameInfo> list(int timeoutMs = 10000)
        {
            const int before = listCount;
            controller.refreshGames();
            (void)QTest::qWaitFor([&]()
                                  { return listCount > before; },
                                  timeoutMs);
            return lastList;
        }

        bool waitForStates(int count, int timeoutMs = 10000)
        {
            return QTest::qWaitFor([&]()
                                   { return states.size() >= count; },
                                   timeoutMs);
        }

        bool waitUntilFinished(int timeoutMs = 15000)
        {
            return QTest::qWaitFor([&]()
                                   { return !states.isEmpty() && isFinished(states.last()); },
                                   timeoutMs);
        }

        static bool isFinished(const GameStateUpdate &state)
        {
            return state.status == ProtocolGameStatus::FINISHED_WIN || state.status == ProtocolGameStatus::FINISHED_DRAW;
        }

        /** Index in `events` of the first entry starting with `prefix`, or -1. */
        int firstEvent(const QString &prefix) const
        {
            for (int i = 0; i < events.size(); ++i)
            {
                if (events[i].startsWith(prefix))
                {
                    return i;
                }
            }
            return -1;
        }

        NetworkController controller;
        const QString name;
        int playerId = 0;
        std::optional<GameInfo> game;
        QList<GameStateUpdate> states;
        QStringList events; // order of the notifications: connected, joined, state:<status>...
        QStringList failures;
        QStringList moveErrors;
        QStringList serverErrors;
        QList<int> serverErrorCodes;
        QString disconnectReason;
        QList<GameInfo> lastList;
        int listCount = 0;
        int refreshFailures = 0;
        Strategy strategy; // empty: never plays

    private:
        void playIfMyTurn(const GameStateUpdate &state)
        {
            if (!strategy || state.status != ProtocolGameStatus::IN_PROGRESS || state.current_player != playerId)
            {
                return;
            }
            const int column = strategy(state, playerId);
            if (column >= 0)
            {
                controller.sendMove(column);
            }
        }
    };

    /**
     * Accepts connections and never answers: a server that is up but stuck.
     * dropClients() makes it hang up on everybody. Needs the event loop (QTest::qWait...) to accept.
     */
    class SilentServer
    {
    public:
        SilentServer()
        {
            server_.listen(QHostAddress::LocalHost, 0);
            QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this]()
                             {
                while (QTcpSocket *socket = server_.nextPendingConnection())
                {
                    socket->setParent(&server_); // keep it open, say nothing
                    ++accepted_;
                } });
        }

        quint16 port() const { return server_.serverPort(); }
        int accepted() const { return accepted_; }

        void dropClients()
        {
            for (QTcpSocket *socket : server_.findChildren<QTcpSocket *>())
            {
                socket->disconnectFromHost();
            }
        }

    private:
        QTcpServer server_;
        int accepted_ = 0;
    };

    /**
     * A complete drawn game on a 4x4 board with connect 4 (found by exhaustive search): the column to
     * play for the n-th disc, whoever plays it (a draw does not depend on who starts).
     */
    constexpr int kDrawColumns[16] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 2, 3};

    /** How many discs of a player are on the board. */
    inline int discsOf(const GameLogic::Board &board, int playerId)
    {
        int count = 0;
        for (const auto &row : board)
        {
            for (uint8_t cell : row)
            {
                count += (cell == playerId) ? 1 : 0;
            }
        }
        return count;
    }
}
