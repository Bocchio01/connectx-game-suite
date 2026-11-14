// NetworkController against a real connectx-server (started by the test on a free port):
// connecting, listing, creating and joining games, playing complete games, errors, server loss,
// and shutting down at every stage without hanging.

#include <algorithm>
#include <memory>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <spdlog/spdlog.h>

#include "gui/game_logic.hpp"
#include "gui/network_controller.hpp"

#include "offscreen_support.hpp"
#include "test_support.hpp"

using TestSupport::ScriptedPlayer;
using TestSupport::ServerProcess;
using TestSupport::SilentServer;

namespace
{
    std::optional<GameInfo> findGame(const QList<GameInfo> &games, const QString &name)
    {
        for (const GameInfo &game : games)
        {
            if (QString::fromStdString(game.game_name) == name)
            {
                return game;
            }
        }
        return std::nullopt;
    }
}

class NetworkTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(server_.start(), "cannot start connectx-server");
    }

    void cleanupTestCase()
    {
        server_.stop();
        // Every worker thread that had to be abandoned has finished by itself.
        QTRY_COMPARE_WITH_TIMEOUT(NetworkController::stuckThreadCount(), 0, 12000);
    }

    // ---------------------------------------------------------------- connecting
    void connectsAndHandshakes()
    {
        ScriptedPlayer alice(QStringLiteral("Alice"));
        QVERIFY(alice.connectTo(server_.port()));
        QCOMPARE(alice.controller.state(), NetworkController::State::Connected);
        QCOMPARE(alice.controller.connectionInfo().playerName, QStringLiteral("Alice"));
        QCOMPARE(int(alice.controller.connectionInfo().port), int(server_.port()));
        QCOMPARE(alice.events, QStringList{QStringLiteral("connected")});

        // A disconnection asked by the user is silent.
        alice.controller.disconnectFromServer();
        QCOMPARE(alice.controller.state(), NetworkController::State::Idle);
        QTest::qWait(300);
        QCOMPARE(alice.events, QStringList{QStringLiteral("connected")});
        QVERIFY(alice.failures.isEmpty());
    }

    void failsWhenNobodyListens()
    {
        const quint16 port = TestSupport::freePort();
        ScriptedPlayer nobody(QStringLiteral("Nobody"));
        nobody.controller.connectToServer(QStringLiteral("127.0.0.1"), port, QStringLiteral("Nobody"));
        QCOMPARE(nobody.controller.state(), NetworkController::State::Connecting);

        QTRY_VERIFY_WITH_TIMEOUT(!nobody.failures.isEmpty(), 20000);
        QVERIFY2(nobody.failures.first().contains(QStringLiteral("127.0.0.1:%1").arg(port)), qPrintable(nobody.failures.first()));
        QCOMPARE(nobody.controller.state(), NetworkController::State::Idle);
        QVERIFY(!nobody.events.contains(QStringLiteral("connected")));
        QCOMPARE(nobody.failures.size(), 1);
    }

    void explainsAnUnknownHost()
    {
        // sockpp throws when the name cannot be resolved (the Client does not catch it): it must end up as a
        // message for the user, not as a crash of the worker thread. (A name that no resolver can know.)
        ScriptedPlayer player(QStringLiteral("Lost"));
        player.controller.connectToServer(QStringLiteral("bad host name!"), 8080, QStringLiteral("Lost"));
        QTRY_VERIFY_WITH_TIMEOUT(!player.failures.isEmpty(), 15000);
        QVERIFY2(player.failures.first().contains(QStringLiteral("Could not resolve the host name")), qPrintable(player.failures.first()));
        QVERIFY2(player.failures.first().contains(QStringLiteral("bad host name!")), qPrintable(player.failures.first()));
        QCOMPARE(player.controller.state(), NetworkController::State::Idle);
        QCOMPARE(player.failures.size(), 1);
    }

    void refusesIncompleteParameters()
    {
        ScriptedPlayer player(QStringLiteral("x"));
        player.controller.connectToServer(QString(), 1234, QStringLiteral("name"));
        player.controller.connectToServer(QStringLiteral("localhost"), 0, QStringLiteral("name"));
        player.controller.connectToServer(QStringLiteral("localhost"), 1234, QStringLiteral("   "));
        QCOMPARE(player.failures.size(), 3); // reported at once, nothing was started
        QCOMPARE(player.controller.state(), NetworkController::State::Idle);
        QCOMPARE(NetworkController::stuckThreadCount(), 0);
    }

    void reportsASilentServer()
    {
        // The TCP connection works but nobody answers the handshake: a timeout, not a hang.
        SilentServer silent;
        ScriptedPlayer player(QStringLiteral("Patient"));
        QElapsedTimer clock;
        clock.start();
        player.controller.connectToServer(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Patient"));
        QTRY_VERIFY_WITH_TIMEOUT(!player.failures.isEmpty(), 8000);
        QVERIFY2(clock.elapsed() > 2000 && clock.elapsed() < 6000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY2(player.failures.first().contains(QStringLiteral("did not answer")), qPrintable(player.failures.first()));
        QCOMPARE(player.controller.state(), NetworkController::State::Idle);
    }

    // ---------------------------------------------------------------- lobby
    void listsGamesAndCoalescesRequests()
    {
        ScriptedPlayer alice(QStringLiteral("Alice"));
        ScriptedPlayer bob(QStringLiteral("Bob"));
        QVERIFY(alice.connectTo(server_.port()));
        QVERIFY(bob.connectTo(server_.port()));
        QVERIFY(alice.createAndJoin(GameConfig(5, 6, 3, 4), QStringLiteral("list-me")));

        const auto game = findGame(bob.list(), QStringLiteral("list-me"));
        QVERIFY(game.has_value());
        QCOMPARE(game->game_id, alice.game->game_id);
        QCOMPARE(int(game->config.rows), 5);
        QCOMPARE(int(game->config.cols), 6);
        QCOMPARE(int(game->config.num_players), 3);
        QCOMPARE(int(game->config.connect_length), 4);
        QCOMPARE(int(game->current_players), 1);
        QVERIFY(game->status == ProtocolGameStatus::NOT_STARTED);

        // Asking again and again while an answer is on its way produces one answer.
        const int before = bob.listCount;
        for (int i = 0; i < 6; ++i)
        {
            bob.controller.refreshGames();
        }
        QTRY_VERIFY_WITH_TIMEOUT(bob.listCount > before, 5000);
        QTest::qWait(300);
        QCOMPARE(bob.listCount, before + 1);
    }

    void quickPlayMeetsAnotherPlayer()
    {
        ServerProcess own; // a server of its own: quick play joins ANY open unnamed game
        QVERIFY(own.start());

        ScriptedPlayer alice(QStringLiteral("Alice"));
        ScriptedPlayer bob(QStringLiteral("Bob"));
        QVERIFY(alice.connectTo(own.port()));
        QVERIFY(bob.connectTo(own.port()));

        QVERIFY(alice.join(0));
        QVERIFY(alice.game->game_name.empty());
        QCOMPARE(int(alice.game->config.rows), 6);
        QCOMPARE(int(alice.game->config.cols), 7);
        QCOMPARE(int(alice.game->config.num_players), 2);
        QCOMPARE(int(alice.game->config.connect_length), 4);

        QVERIFY(bob.join(0));
        QCOMPARE(bob.game->game_id, alice.game->game_id); // the same game
        QVERIFY(alice.playerId != bob.playerId);

        // The game starts when the last seat is taken.
        QTRY_VERIFY_WITH_TIMEOUT(!alice.states.isEmpty() && alice.states.last().status == ProtocolGameStatus::IN_PROGRESS, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!bob.states.isEmpty() && bob.states.last().status == ProtocolGameStatus::IN_PROGRESS, 5000);
        QCOMPARE(alice.states.last().players.size(), size_t(2));
        QCOMPARE(QString::fromStdString(alice.states.last().player_names[0]), QStringLiteral("Alice"));
        QCOMPARE(QString::fromStdString(alice.states.last().player_names[1]), QStringLiteral("Bob"));
    }

    void quickPlayMayJoinAGameCreatedOnPurpose()
    {
        // A quirk of the server that the GUI shows as it is: "quick play" (joining game 0) takes ANY open game
        // without a name, also one that somebody made with "New game..." on purpose. A named game is safe from it.
        ServerProcess own;
        QVERIFY(own.start());

        ScriptedPlayer creator(QStringLiteral("Creator"));
        ScriptedPlayer stranger(QStringLiteral("Stranger"));
        QVERIFY(creator.connectTo(own.port()));
        QVERIFY(stranger.connectTo(own.port()));
        QVERIFY(creator.createAndJoin(GameConfig(5, 9, 2, 5), QString()));
        QVERIFY(stranger.join(0));
        QCOMPARE(stranger.game->game_id, creator.game->game_id);
        QCOMPARE(int(stranger.game->config.cols), 9);

        ScriptedPlayer host(QStringLiteral("Host"));
        ScriptedPlayer passer(QStringLiteral("Passer"));
        QVERIFY(host.connectTo(own.port()));
        QVERIFY(passer.connectTo(own.port()));
        QVERIFY(host.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Closed door")));
        QVERIFY(passer.join(0));
        QVERIFY(passer.game->game_id != host.game->game_id); // it started a game of its own
        QVERIFY(passer.game->game_name.empty());
    }

    // ---------------------------------------------------------------- whole games
    void playsACompleteGameToAWin()
    {
        ScriptedPlayer alice(QStringLiteral("Alice"));
        ScriptedPlayer bob(QStringLiteral("Bob"));
        QVERIFY(alice.connectTo(server_.port()));
        QVERIFY(bob.connectTo(server_.port()));

        // Alice only plays column 0 and wins vertically; Bob alternates two columns and never gets four.
        alice.strategy = [](const GameStateUpdate &, int)
        { return 0; };
        bob.strategy = [](const GameStateUpdate &state, int me)
        { return 1 + TestSupport::discsOf(state.board, me) % 2; };

        QVERIFY(alice.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("win-game")));
        QCOMPARE(QString::fromStdString(alice.game->game_name), QStringLiteral("win-game"));
        QCOMPARE(alice.playerId, 1); // creating does not join: she joined right after, as the first player

        const auto listed = findGame(bob.list(), QStringLiteral("win-game"));
        QVERIFY(listed.has_value());
        QVERIFY(bob.join(listed->game_id));
        QCOMPARE(bob.playerId, 2);

        QVERIFY(alice.waitUntilFinished());
        QVERIFY(bob.waitUntilFinished());

        // The result.
        const GameStateUpdate &last = alice.states.last();
        QVERIFY(last.status == ProtocolGameStatus::FINISHED_WIN);
        QVERIFY(last.winner.has_value());
        QCOMPARE(int(*last.winner), alice.playerId);
        const auto lines = GameLogic::findWinningLines(last.board, static_cast<uint8_t>(alice.playerId), 4);
        QCOMPARE(lines.size(), size_t(1));
        QCOMPARE(lines[0].size(), size_t(4));
        for (const GameLogic::Cell &cell : lines[0])
        {
            QCOMPARE(cell.col, 0);
        }
        QVERIFY(GameLogic::findWinningLines(last.board, static_cast<uint8_t>(bob.playerId), 4).empty());
        QCOMPARE(TestSupport::discsOf(last.board, alice.playerId), 4);

        // Both saw the very same game. Alice also saw the lobby state before Bob came.
        QVERIFY(alice.states.first().status == ProtocolGameStatus::NOT_STARTED);
        QCOMPARE(alice.states.first().players.size(), size_t(1));
        QCOMPARE(alice.states.size() - 1, bob.states.size());
        for (int i = 0; i < bob.states.size(); ++i)
        {
            const GameStateUpdate &a = alice.states[i + 1];
            const GameStateUpdate &b = bob.states[i];
            QVERIFY(a.board == b.board);
            QVERIFY(a.status == b.status);
            QCOMPARE(int(a.current_player), int(b.current_player));
        }

        // Every update after the start adds exactly one disc, dropped by the player whose turn it was.
        for (int i = 1; i < bob.states.size(); ++i)
        {
            const GameStateUpdate &before = bob.states[i - 1];
            const GameStateUpdate &after = bob.states[i];
            const auto changes = GameLogic::findNewDiscs(before.board, after.board);
            QVERIFY(changes.has_value());
            QCOMPARE(changes->size(), size_t(1));
            QCOMPARE(int((*changes)[0].player), int(before.current_player));
            QCOMPARE((*changes)[0].cell.row, GameLogic::landingRow(before.board, (*changes)[0].cell.col));
        }
        QVERIFY(bob.states.first().status == ProtocolGameStatus::IN_PROGRESS);
        QCOMPARE(GameLogic::countDiscs(bob.states.first().board), 0);

        // The join notification always comes before the first state of the game.
        QVERIFY(alice.firstEvent(QStringLiteral("joined")) < alice.firstEvent(QStringLiteral("state:")));
        QVERIFY(bob.firstEvent(QStringLiteral("joined")) < bob.firstEvent(QStringLiteral("state:")));
        QVERIFY(alice.moveErrors.isEmpty() && bob.moveErrors.isEmpty());
        QVERIFY(alice.serverErrors.isEmpty() && bob.serverErrors.isEmpty());

        // "Back to the lobby": a fresh connection with the same parameters, the finished game is still listed.
        alice.controller.reconnect();
        QTRY_COMPARE_WITH_TIMEOUT(alice.events.count(QStringLiteral("connected")), 2, 10000);
        QVERIFY(alice.controller.isConnected());
        QVERIFY(!alice.events.contains(QStringLiteral("disconnected"))); // leaving on purpose is not an error
        const auto stillListed = findGame(alice.list(), QStringLiteral("win-game"));
        QVERIFY(stillListed.has_value());
        QVERIFY(stillListed->status == ProtocolGameStatus::FINISHED_WIN);
    }

    void movesTravelQuicklyThroughTheWholeStack()
    {
        // From the moment a move is sent until the new state of the game comes back: the controller, its
        // worker thread, the Client, the server, and back through the event thread to the GUI thread.
        ScriptedPlayer alice(QStringLiteral("Alice"));
        ScriptedPlayer bob(QStringLiteral("Bob"));
        QVERIFY(alice.connectTo(server_.port()));
        QVERIFY(bob.connectTo(server_.port()));

        QElapsedTimer clock;
        clock.start();
        qint64 sentAt = -1;
        QList<qint64> latencies;

        alice.strategy = [&](const GameStateUpdate &, int)
        {
            sentAt = clock.elapsed();
            return 0;
        };
        bob.strategy = [](const GameStateUpdate &state, int me)
        { return 1 + TestSupport::discsOf(state.board, me) % 2; };
        QObject::connect(&alice.controller, &NetworkController::stateUpdated, &alice.controller, [&](const GameStateUpdate &state)
                         {
            if (sentAt >= 0 && state.status == ProtocolGameStatus::IN_PROGRESS && state.current_player != alice.playerId)
            {
                latencies << clock.elapsed() - sentAt;
                sentAt = -1;
            } });

        QVERIFY(alice.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("latency-game")));
        QVERIFY(bob.join(alice.game->game_id));
        QVERIFY(alice.waitUntilFinished());
        QVERIFY(latencies.size() >= 3);

        std::sort(latencies.begin(), latencies.end());
        const qint64 median = latencies[latencies.size() / 2];
        qInfo("move round trip: median %lld ms, max %lld ms (%d moves)", static_cast<long long>(median),
              static_cast<long long>(latencies.last()), static_cast<int>(latencies.size()));
        // A few milliseconds in practice; this only catches something being badly wrong (a stuck queue,
        // a 200 ms delay of the network stack...).
        QVERIFY2(median < 150, qPrintable(QString::number(median)));
    }

    void playsACompleteGameToADraw()
    {
        ScriptedPlayer alice(QStringLiteral("Alice"));
        ScriptedPlayer bob(QStringLiteral("Bob"));
        QVERIFY(alice.connectTo(server_.port()));
        QVERIFY(bob.connectTo(server_.port()));

        const auto strategy = [](const GameStateUpdate &state, int)
        { return TestSupport::kDrawColumns[GameLogic::countDiscs(state.board)]; };
        alice.strategy = strategy;
        bob.strategy = strategy;

        QVERIFY(alice.createAndJoin(GameConfig(4, 4, 2, 4), QStringLiteral("draw-game")));
        QVERIFY(bob.join(alice.game->game_id));

        QVERIFY(alice.waitUntilFinished());
        QVERIFY(bob.waitUntilFinished());

        const GameStateUpdate &last = bob.states.last();
        QVERIFY(last.status == ProtocolGameStatus::FINISHED_DRAW);
        QVERIFY(!last.winner.has_value());
        QCOMPARE(GameLogic::countDiscs(last.board), 16);
        QVERIFY(GameLogic::findWinningLines(last.board, 1, 4).empty());
        QVERIFY(GameLogic::findWinningLines(last.board, 2, 4).empty());
        QVERIFY(alice.states.last().status == ProtocolGameStatus::FINISHED_DRAW);
        QVERIFY(alice.moveErrors.isEmpty() && bob.moveErrors.isEmpty());
    }

    void playsAThreePlayerGame()
    {
        ScriptedPlayer a(QStringLiteral("A"));
        ScriptedPlayer b(QStringLiteral("B"));
        ScriptedPlayer c(QStringLiteral("C"));
        QVERIFY(a.connectTo(server_.port()));
        QVERIFY(b.connectTo(server_.port()));
        QVERIFY(c.connectTo(server_.port()));

        // Everybody plays their own column: the first to stack three (connect 3) wins.
        const auto strategy = [](const GameStateUpdate &, int me)
        { return me - 1; };
        a.strategy = strategy;
        b.strategy = strategy;
        c.strategy = strategy;

        QVERIFY(a.createAndJoin(GameConfig(5, 6, 3, 3), QStringLiteral("three-players")));
        QVERIFY(b.join(a.game->game_id));
        QVERIFY(c.join(a.game->game_id));
        QCOMPARE(a.playerId, 1);
        QCOMPARE(b.playerId, 2);
        QCOMPARE(c.playerId, 3);

        QVERIFY(a.waitUntilFinished());
        QVERIFY(b.waitUntilFinished());
        QVERIFY(c.waitUntilFinished());
        const GameStateUpdate &last = c.states.last();
        QVERIFY(last.status == ProtocolGameStatus::FINISHED_WIN);
        QCOMPARE(last.players.size(), size_t(3));
        QCOMPARE(GameLogic::findWinningLines(last.board, *last.winner, 3).size(), size_t(1));
    }

    // ---------------------------------------------------------------- errors
    void reportsARefusedJoinAtOnce()
    {
        ScriptedPlayer bob(QStringLiteral("Bob"));
        QVERIFY(bob.connectTo(server_.port()));

        QElapsedTimer clock;
        clock.start();
        bob.controller.joinGame(987654);
        QTRY_COMPARE_WITH_TIMEOUT(bob.failures.size(), 1, 2500);
        // The refusal of the server is reported at once, not after the 3 s timeout of the request.
        QVERIFY2(clock.elapsed() < 1000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(bob.failures.first(), QStringLiteral("That game does not exist any more."));
        QCOMPARE(bob.serverErrors.last(), QStringLiteral("That game does not exist any more."));
        QCOMPARE(int(bob.serverErrorCodes.last()), 200);

        // Reported once, and the connection is still fine (and free: the next request goes through at once).
        QTest::qWait(500);
        QCOMPARE(bob.failures.size(), 1);
        QCOMPARE(bob.events.count(QStringLiteral("joinFailed")), 1);
        QVERIFY(bob.controller.isConnected());
        clock.restart();
        const int listings = bob.listCount;
        bob.list();
        QVERIFY(bob.listCount > listings);
        QVERIFY2(clock.elapsed() < 1000, qPrintable(QString::number(clock.elapsed())));
    }

    void reportsAFullGame()
    {
        ScriptedPlayer a(QStringLiteral("A"));
        ScriptedPlayer b(QStringLiteral("B"));
        ScriptedPlayer c(QStringLiteral("C"));
        QVERIFY(a.connectTo(server_.port()));
        QVERIFY(b.connectTo(server_.port()));
        QVERIFY(c.connectTo(server_.port()));
        QVERIFY(a.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("full-game")));
        QVERIFY(b.join(a.game->game_id));

        c.controller.joinGame(a.game->game_id);
        QTRY_COMPARE_WITH_TIMEOUT(c.failures.size(), 1, 2500);
        QCOMPARE(c.failures.first(), QStringLiteral("That game is already full."));
        QVERIFY(!c.game.has_value());
    }

    void reportsADuplicateName()
    {
        ScriptedPlayer a(QStringLiteral("A"));
        ScriptedPlayer b(QStringLiteral("B"));
        QVERIFY(a.connectTo(server_.port()));
        QVERIFY(b.connectTo(server_.port()));
        QVERIFY(a.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("same-name")));

        b.controller.createGame(GameConfig(6, 7, 2, 4), QStringLiteral("same-name"));
        QTRY_COMPARE_WITH_TIMEOUT(b.failures.size(), 1, 2500);
        QCOMPARE(b.failures.first(), QStringLiteral("A game with this name already exists."));
        QCOMPARE(b.events.last(), QStringLiteral("createFailed"));
        QVERIFY(!b.game.has_value()); // and it did not join anything
    }

    void reportsRefusedSettings()
    {
        ScriptedPlayer a(QStringLiteral("A"));
        QVERIFY(a.connectTo(server_.port()));
        a.controller.createGame(GameConfig(3, 7, 2, 4), QStringLiteral("too-small")); // the server wants 4..20
        QTRY_COMPARE_WITH_TIMEOUT(a.failures.size(), 1, 2500);
        QCOMPARE(a.failures.first(), QStringLiteral("The server rejected the game settings."));
    }

    void reportsMovesOutOfTurnAndInvalidMoves()
    {
        ScriptedPlayer a(QStringLiteral("A"));
        ScriptedPlayer b(QStringLiteral("B"));
        QVERIFY(a.connectTo(server_.port()));
        QVERIFY(b.connectTo(server_.port()));
        QVERIFY(a.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("rules-game")));
        QVERIFY(b.join(a.game->game_id));
        QTRY_VERIFY_WITH_TIMEOUT(!b.states.isEmpty() && b.states.last().status == ProtocolGameStatus::IN_PROGRESS, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(a.states.size() >= 2, 5000);

        const bool aToMove = (b.states.last().current_player == a.playerId);
        ScriptedPlayer &mover = aToMove ? a : b;
        ScriptedPlayer &other = aToMove ? b : a;

        other.controller.sendMove(0);
        QTRY_VERIFY_WITH_TIMEOUT(!other.serverErrors.isEmpty(), 3000);
        QCOMPARE(other.serverErrors.last(), QStringLiteral("It is not your turn."));
        QCOMPARE(int(other.serverErrorCodes.last()), 301);

        mover.controller.sendMove(99); // there is no such column
        QTRY_VERIFY_WITH_TIMEOUT(!mover.moveErrors.isEmpty(), 3000);
        QCOMPARE(mover.moveErrors.last(), QStringLiteral("Invalid move (column full or out of range)"));

        // The game goes on normally afterwards.
        const int discsBefore = GameLogic::countDiscs(mover.states.last().board);
        mover.controller.sendMove(3);
        QTRY_VERIFY_WITH_TIMEOUT(GameLogic::countDiscs(other.states.last().board) == discsBefore + 1, 3000);
    }

    // ---------------------------------------------------------------- leaving and losing connections
    void leavingByReconnectingFreesTheSeat()
    {
        ScriptedPlayer a(QStringLiteral("A"));
        ScriptedPlayer b(QStringLiteral("B"));
        QVERIFY(a.connectTo(server_.port()));
        QVERIFY(b.connectTo(server_.port()));
        QVERIFY(a.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("leave-game")));
        QVERIFY(b.join(a.game->game_id));
        QTRY_VERIFY_WITH_TIMEOUT(a.states.size() >= 2, 5000);

        // B leaves in the middle of the game, the way the "Leave game" button does it.
        b.controller.reconnect();
        QTRY_COMPARE_WITH_TIMEOUT(b.events.count(QStringLiteral("connected")), 2, 10000);

        // A is told that B is gone.
        QTRY_VERIFY_WITH_TIMEOUT(a.states.last().players.size() == 1, 5000);
        QCOMPARE(int(a.states.last().players[0]), a.playerId);
        QVERIFY(!b.events.contains(QStringLiteral("disconnected")));

        // The new connection of B is a clean slate: no leftover game events for the old game.
        const int statesOfB = b.states.size();
        QTest::qWait(300);
        QCOMPARE(b.states.size(), statesOfB);
        QVERIFY(b.controller.isConnected());
    }

    void notifiesWhenTheServerGoesAway()
    {
        ServerProcess doomed;
        QVERIFY(doomed.start());

        ScriptedPlayer a(QStringLiteral("A"));
        QVERIFY(a.connectTo(doomed.port()));
        QVERIFY(a.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("doomed-game")));

        doomed.stop();
        QTRY_VERIFY_WITH_TIMEOUT(a.events.contains(QStringLiteral("disconnected")), 8000);
        QCOMPARE(a.events.count(QStringLiteral("disconnected")), 1);
        QVERIFY(!a.disconnectReason.isEmpty());
        QCOMPARE(a.controller.state(), NetworkController::State::Idle);

        // Nothing works any more, and says so without hanging.
        a.controller.joinGame(0);
        QCOMPARE(a.failures.size(), 1);

        // The same controller connects to another server afterwards.
        QVERIFY(a.connectTo(server_.port()));
        QVERIFY(a.controller.isConnected());
    }

    // ---------------------------------------------------------------- ordering
    void joinNotificationAlwaysPrecedesTheFirstState()
    {
        // The state broadcast races with the join response: do it many times.
        for (int i = 0; i < 25; ++i)
        {
            ScriptedPlayer a(QStringLiteral("A"));
            ScriptedPlayer b(QStringLiteral("B"));
            QVERIFY(a.connectTo(server_.port()));
            QVERIFY(b.connectTo(server_.port()));
            QVERIFY(a.createAndJoin(GameConfig(4, 4, 2, 4), QStringLiteral("race-%1").arg(i)));
            QVERIFY(b.join(a.game->game_id));
            QTRY_VERIFY_WITH_TIMEOUT(!b.states.isEmpty() && a.states.size() >= 2, 5000);

            const int joinedA = a.firstEvent(QStringLiteral("joined"));
            const int stateA = a.firstEvent(QStringLiteral("state:"));
            const int joinedB = b.firstEvent(QStringLiteral("joined"));
            const int stateB = b.firstEvent(QStringLiteral("state:"));
            QVERIFY2(joinedA >= 0 && stateA > joinedA, qPrintable(a.events.join(QLatin1Char(' '))));
            QVERIFY2(joinedB >= 0 && stateB > joinedB, qPrintable(b.events.join(QLatin1Char(' '))));
        }
    }

    void survivesRapidConnectAndDisconnectCycles()
    {
        ScriptedPlayer player(QStringLiteral("Cycler"));
        for (int i = 0; i < 24; ++i)
        {
            QVERIFY(player.connectTo(server_.port()));
            if (i % 3 == 0)
            {
                player.controller.refreshGames();
            }
            if (i % 4 == 0)
            {
                player.controller.joinGame(0); // takes a seat in a quick game, then leaves it
                QTest::qWait(10);
            }
            if (i % 2 == 0)
            {
                player.controller.disconnectFromServer();
            }
            else
            {
                player.controller.reconnect(); // replaces the connection straight away
                QTRY_VERIFY_WITH_TIMEOUT(player.controller.isConnected(), 10000);
            }
        }
        player.controller.disconnectFromServer();
        QVERIFY(!player.events.contains(QStringLiteral("disconnected")));
        QVERIFY(player.failures.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(NetworkController::stuckThreadCount(), 0, 6000);
    }

    // ---------------------------------------------------------------- shutting down
    void shutsDownPromptlyAtEveryStage()
    {
        auto timedReset = [](std::unique_ptr<ScriptedPlayer> &player)
        {
            QElapsedTimer clock;
            clock.start();
            player.reset();
            return clock.elapsed();
        };

        // 1. while connecting
        {
            auto p = std::make_unique<ScriptedPlayer>(QStringLiteral("S1"));
            p->controller.connectToServer(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("S1"));
            const qint64 ms = timedReset(p);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }
        // 2. connected, in the lobby
        {
            auto p = std::make_unique<ScriptedPlayer>(QStringLiteral("S2"));
            QVERIFY(p->connectTo(server_.port()));
            p->controller.refreshGames();
            const qint64 ms = timedReset(p);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }
        // 3. waiting for the other players: the seat (and the empty game) disappear from the server
        {
            ScriptedPlayer watcher(QStringLiteral("Watcher"));
            QVERIFY(watcher.connectTo(server_.port()));

            auto p = std::make_unique<ScriptedPlayer>(QStringLiteral("S3"));
            QVERIFY(p->connectTo(server_.port()));
            QVERIFY(p->createAndJoin(GameConfig(6, 7, 3, 4), QStringLiteral("abandoned-lobby")));
            QVERIFY(findGame(watcher.list(), QStringLiteral("abandoned-lobby")).has_value());

            const qint64 ms = timedReset(p);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
            QTRY_VERIFY_WITH_TIMEOUT(!findGame(watcher.list(), QStringLiteral("abandoned-lobby")).has_value(), 5000);
        }
        // 4. in the middle of a game: the opponent is told
        {
            ScriptedPlayer survivor(QStringLiteral("Survivor"));
            QVERIFY(survivor.connectTo(server_.port()));
            auto p = std::make_unique<ScriptedPlayer>(QStringLiteral("S4"));
            QVERIFY(p->connectTo(server_.port()));
            QVERIFY(survivor.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("abandoned-game")));
            QVERIFY(p->join(survivor.game->game_id));
            QTRY_VERIFY_WITH_TIMEOUT(survivor.states.size() >= 2, 5000);

            const qint64 ms = timedReset(p);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
            QTRY_VERIFY_WITH_TIMEOUT(survivor.states.last().players.size() == 1, 5000);
        }
        // 5. after the end of a game
        {
            auto p = std::make_unique<ScriptedPlayer>(QStringLiteral("S5"));
            ScriptedPlayer q(QStringLiteral("S5b"));
            QVERIFY(p->connectTo(server_.port()));
            QVERIFY(q.connectTo(server_.port()));
            p->strategy = [](const GameStateUpdate &, int)
            { return 0; };
            q.strategy = [](const GameStateUpdate &state, int me)
            { return 1 + TestSupport::discsOf(state.board, me) % 2; };
            QVERIFY(p->createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("finished-then-closed")));
            QVERIFY(q.join(p->game->game_id));
            QVERIFY(p->waitUntilFinished());

            const qint64 ms = timedReset(p);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }

        QCOMPARE(NetworkController::stuckThreadCount(), 0); // nothing had to be abandoned
    }

    void shutsDownPromptlyWhileTheServerIsSilent()
    {
        // The worker is waiting for the answer of the handshake: closing the connection releases it.
        SilentServer silent;
        auto player = std::make_unique<ScriptedPlayer>(QStringLiteral("Mute"));
        player->controller.connectToServer(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Mute"));
        QTest::qWait(300);

        QElapsedTimer clock;
        clock.start();
        player.reset();
        QVERIFY2(clock.elapsed() < 1000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(NetworkController::stuckThreadCount(), 0); // nothing had to be abandoned
    }

    void cancellingAConnectionAttemptIsInstantAndSilent()
    {
        SilentServer silent;
        ScriptedPlayer player(QStringLiteral("Mute"));
        player.controller.connectToServer(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Mute"));
        QTest::qWait(300);

        QElapsedTimer clock;
        clock.start();
        player.controller.disconnectFromServer();
        QVERIFY2(clock.elapsed() < 1000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(player.controller.state(), NetworkController::State::Idle);
        QCOMPARE(NetworkController::stuckThreadCount(), 0);

        // The attempt that was given up does not tell anybody anything, not even when its 3 s are over.
        QTest::qWait(3500);
        QVERIFY(player.events.isEmpty());
        QVERIFY(player.failures.isEmpty());
    }

    void aNewConnectionReplacesAPendingOne()
    {
        SilentServer silent;
        ScriptedPlayer player(QStringLiteral("Impatient"));
        player.controller.connectToServer(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Impatient"));
        QTest::qWait(300);

        // Change of mind: connect somewhere that works, while the first handshake is still pending.
        QElapsedTimer clock;
        clock.start();
        QVERIFY(player.connectTo(server_.port(), 5000));
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(player.events.count(QStringLiteral("connected")), 1);

        QTest::qWait(3500); // the first attempt would have timed out by now: it must not report anything
        QVERIFY(player.failures.isEmpty());
        QVERIFY(player.controller.isConnected());
        QCOMPARE(NetworkController::stuckThreadCount(), 0);
    }

    void reportsAServerThatHangsUpDuringTheHandshake()
    {
        // The TCP connection is made, then the server closes it while the handshake is pending: the
        // request is released at once (no waiting for the 3 s timeout), and the failure is reported once.
        SilentServer silent;
        ScriptedPlayer player(QStringLiteral("Dropped"));
        player.controller.connectToServer(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Dropped"));
        QTRY_VERIFY_WITH_TIMEOUT(silent.accepted() == 1, 3000);

        QElapsedTimer clock;
        clock.start();
        silent.dropClients();
        QTRY_VERIFY_WITH_TIMEOUT(!player.failures.isEmpty(), 3000);
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(player.controller.state(), NetworkController::State::Idle);

        QTest::qWait(500);
        QCOMPARE(player.failures.size(), 1);
        QVERIFY(!player.events.contains(QStringLiteral("connected")));
    }

    void abandonsAConnectAttemptThatNeverEnds()
    {
        // The one call that cannot be interrupted is the TCP connect to a host that does not answer (it gives
        // up after the connect timeout, 8 s): its worker is abandoned, the user is not kept waiting.
        // Opt-in, because it needs an address that silently drops the packets and takes about 8 s:
        //   CONNECTX_GUI_TEST_BLACKHOLE=192.0.2.1 gui_test_network abandonsAConnectAttemptThatNeverEnds
        const QString address = qEnvironmentVariable("CONNECTX_GUI_TEST_BLACKHOLE");
        if (address.isEmpty())
        {
            QSKIP("set CONNECTX_GUI_TEST_BLACKHOLE to an address that silently drops the connections (e.g. 192.0.2.1)");
        }

        auto player = std::make_unique<ScriptedPlayer>(QStringLiteral("Lost"));
        player->controller.connectToServer(address, 9, QStringLiteral("Lost"));
        QTest::qWait(1000);
        if (player->controller.state() != NetworkController::State::Connecting)
        {
            QSKIP("the network answered at once (unreachable): no black hole available here");
        }

        QElapsedTimer clock;
        clock.start();
        player.reset();
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(NetworkController::stuckThreadCount(), 1);

        // It ends by itself when the connect gives up, without telling anybody.
        QTRY_COMPARE_WITH_TIMEOUT(NetworkController::stuckThreadCount(), 0, 20000);
    }

private:
    ServerProcess server_;
};

int main(int argc, char *argv[])
{
    OffscreenSupport::prepare();
    QCoreApplication app(argc, argv);
    spdlog::set_level(spdlog::level::err);

    NetworkTest test;
    return OffscreenSupport::runTest(test, argc, argv);
}

#include "test_network.moc"
