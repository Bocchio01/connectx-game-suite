// The whole application window against a real connectx-server: connect page -> lobby -> game,
// driven through the widgets like a user would (mouse, keyboard, buttons, menu, dialogs), with
// scripted opponents on the other side. Also closes the window at every stage of the session.
//
// With CONNECTX_GUI_SCREENSHOT_DIR=<dir> the test saves gui-lobby.png, gui-game.png and
// gui-win.png (the pictures used in the documentation) in that directory.

#include <memory>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTableView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <spdlog/spdlog.h>

#include "gui/board_widget.hpp"
#include "gui/connect_page.hpp"
#include "gui/game_list_model.hpp"
#include "gui/game_logic.hpp"
#include "gui/game_page.hpp"
#include "gui/lobby_page.hpp"
#include "gui/main_window.hpp"
#include "gui/new_game_dialog.hpp"

#include "offscreen_support.hpp"
#include "test_support.hpp"
#include "window_support.hpp"

using namespace WindowSupport;
using TestSupport::ScriptedPlayer;
using TestSupport::ServerProcess;
using TestSupport::SilentServer;

namespace
{
    // A scripted game of 14 plies on a 6 x 7 board, won by the second player with a diagonal.
    // The first player (the opponent) plays the even plies, the user the odd ones; the column of
    // each ply is indexed by the number of discs already on the board.
    constexpr int kScript[14] = {3, 1, 3, 5, 3, 4, 0, 5, 4, 4, 2, 3, 5, 6};
    constexpr int kScreenshotPly = 9; // the picture of the game is taken before this move of the user
}

class FlowTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // CONNECTX_GUI_TEST_PORT=<port>: use that port for the main server (taking the pictures of the README).
        const int preferredPort = qEnvironmentVariableIntValue("CONNECTX_GUI_TEST_PORT");
        QVERIFY2(server_.start((preferredPort > 0 && preferredPort < 65536) ? static_cast<quint16>(preferredPort) : quint16(0)),
                 "cannot start connectx-server");
        screenshotDir_ = qEnvironmentVariable("CONNECTX_GUI_SCREENSHOT_DIR");
        if (!screenshotDir_.isEmpty())
        {
            QDir().mkpath(screenshotDir_);
        }
    }

    void cleanupTestCase()
    {
        server_.stop();
        QTRY_COMPARE_WITH_TIMEOUT(NetworkController::stuckThreadCount(), 0, 12000);
    }

    // =====================================================================================
    // The main scenario: connect, look at the lobby, join, play a whole game, win, go back.
    // =====================================================================================
    void aCompleteSession()
    {
        // ---- other people already in the server ---------------------------------------------
        std::vector<std::unique_ptr<ScriptedPlayer>> others;
        auto person = [&](const char *name) -> ScriptedPlayer *
        {
            others.push_back(std::make_unique<ScriptedPlayer>(QString::fromLatin1(name)));
            return others.back()->connectTo(server_.port()) ? others.back().get() : nullptr;
        };

        ScriptedPlayer *bob = person("Bob");
        QVERIFY(bob != nullptr);
        QVERIFY(bob->createAndJoin(GameConfig(10, 12, 3, 5), QStringLiteral("Big board")));

        ScriptedPlayer *carol = person("Carol");
        ScriptedPlayer *dave = person("Dave");
        QVERIFY(carol != nullptr && dave != nullptr);
        QVERIFY(carol->join(0)); // quick play: an unnamed standard game...
        QVERIFY(dave->join(0));  // ...that starts as soon as Dave arrives

        ScriptedPlayer *erin = person("Erin");
        ScriptedPlayer *frank = person("Frank");
        QVERIFY(erin != nullptr && frank != nullptr);
        QVERIFY(erin->createAndJoin(GameConfig(8, 8, 4, 4), QStringLiteral("Four of us")));
        QVERIFY(frank->join(erin->game->game_id));

        ScriptedPlayer *gina = person("Gina");
        ScriptedPlayer *hal = person("Hal");
        QVERIFY(gina != nullptr && hal != nullptr);
        gina->strategy = [](const GameStateUpdate &, int)
        { return 0; };
        hal->strategy = [](const GameStateUpdate &state, int me)
        { return 1 + TestSupport::discsOf(state.board, me) % 2; };
        QVERIFY(gina->createAndJoin(GameConfig(4, 4, 2, 3), QStringLiteral("Tiny")));
        QVERIFY(hal->join(gina->game->game_id));
        QVERIFY(gina->waitUntilFinished());

        // The opponent of the scripted game plays by the script, whenever it is her turn.
        ScriptedPlayer *alice = person("Alice");
        QVERIFY(alice != nullptr);
        alice->strategy = [](const GameStateUpdate &state, int)
        { return kScript[GameLogic::countDiscs(state.board)]; };

        // ---- the window ---------------------------------------------------------------------
        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
        QCOMPARE(window->statusBar()->findChildren<QLabel *>().last()->text().contains(QStringLiteral("Not connected")), true);
        if (!screenshotDir_.isEmpty())
        {
            // The picture shows the page as a user first sees it (the default server), not the random port of the
            // test server; the values of the test are put back right after.
            window->connectPage()->setValues(QStringLiteral("localhost"), Protocol::DEFAULT_PORT, QStringLiteral("Tommaso"));
            save(*window, QStringLiteral("gui-connect"), true);
            window->connectPage()->setValues(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        }

        // Connect button -> busy -> lobby.
        QPushButton *connectButton = findButton(window->connectPage(), QStringLiteral("Connect"));
        QVERIFY(connectButton && connectButton->isVisibleTo(window->connectPage()));
        QTest::mouseClick(connectButton, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QVERIFY(window->controller()->isConnected());

        // The status bar tells where we are connected.
        const QString connection = window->statusBar()->findChildren<QLabel *>().last()->text();
        QVERIFY2(connection.contains(QStringLiteral("Connected to 127.0.0.1:%1 as Tommaso").arg(server_.port())), qPrintable(connection));

        // ---- the lobby ----------------------------------------------------------------------
        // The list shows the games of the others, refreshed by itself (Alice has not created hers yet).
        QTRY_COMPARE_WITH_TIMEOUT(window->lobbyPage()->model()->rowCount(), 4, 8000);

        // Another game is created while the window is open: it appears without pressing anything.
        QVERIFY(waitForGame(*window, QStringLiteral("Tiny")));
        QVERIFY(waitForGame(*window, QStringLiteral("Big board")));
        const GameInfo tiny = *findGame(*window->lobbyPage()->model(), QStringLiteral("Tiny"));
        QVERIFY(tiny.status == ProtocolGameStatus::FINISHED_WIN);

        // The game of Alice appears in the list, and the selection survives the next refreshes.
        ScriptedPlayer &host = *alice;
        QVERIFY(host.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Coffee break")));
        QVERIFY(waitForGame(*window, QStringLiteral("Coffee break")));
        const quint32 coffeeId = findGame(*window->lobbyPage()->model(), QStringLiteral("Coffee break"))->game_id;
        window->lobbyPage()->selectGame(coffeeId);
        QCOMPARE(window->lobbyPage()->selectedGameId(), coffeeId);
        QTest::qWait(LobbyPage::kRefreshIntervalMs + 600); // at least one refresh in between
        QCOMPARE(window->lobbyPage()->selectedGameId(), coffeeId);
        QVERIFY(findButton(window->lobbyPage(), QStringLiteral("Join selected"))->isEnabled());
        save(*window, QStringLiteral("gui-lobby"), true);

        // ---- the game ------------------------------------------------------------------------
        // The user wants to play second, against the script of Alice (who must start): when the
        // random choice of the first player goes the other way, leave and try again.
        QString gameName = QStringLiteral("Coffee break");
        int attempt = 0;
        for (;; ++attempt)
        {
            QVERIFY2(attempt < 40, "the random starting player never was the opponent");

            selectAndJoin(*window, gameName);
            QTRY_VERIFY2_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game,
                                      qPrintable(QStringLiteral("still in the lobby; message: '%1'; busy: %2")
                                                     .arg(window->lobbyPage()->message())
                                                     .arg(window->lobbyPage()->isBusy())),
                                      10000);
            GamePage *game = window->gamePage();

            // The game starts at once. When the opponent starts she has already dropped her first disc by
            // the time the window shows it (she answers within milliseconds): an empty board on the
            // user's turn is the only way to tell that the user starts.
            QTRY_VERIFY_WITH_TIMEOUT(game->model().phase() == GameModel::Phase::Playing, 5000);
            const bool userStarts = game->model().currentPlayer() == game->model().myPlayerId() &&
                                    GameLogic::countDiscs(game->model().board()) == 0;
            if (!userStarts)
            {
                break; // the opponent starts
            }

            // The user would start: leave (the "Leave game" button) and try with a new game.
            QCOMPARE(game->statusText(), QStringLiteral("Your turn"));
            QTest::mouseClick(findButton(game, QStringLiteral("Leave game")), Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(window->controller()->isConnected(), 10000);

            alice->controller.reconnect();
            QTRY_VERIFY_WITH_TIMEOUT(alice->controller.isConnected() && alice->events.count(QStringLiteral("connected")) >= attempt + 2, 10000);
            alice->game.reset();

            // Both have left the game, so the server dropped it (it disappears from the list) and its name is
            // free again: the next one gets the same name, which keeps the pictures consistent.
            QTRY_VERIFY_WITH_TIMEOUT(!findGame(*window->lobbyPage()->model(), gameName).has_value(), 10000);
            QVERIFY(alice->createAndJoin(GameConfig(6, 7, 2, 4), gameName));
            QVERIFY(waitForGame(*window, gameName));
        }

        GamePage *game = window->gamePage();
        BoardWidget *board = game->board();
        QCOMPARE(board->rows(), 6);
        QCOMPARE(board->cols(), 7);
        QCOMPARE(game->model().myPlayerId(), 2);
        QVERIFY(!game->needsLeaveConfirmation() || game->model().isInProgress());

        // The first state is shown at once: the board starts empty and nothing falls.
        QVERIFY(!board->isAnimating() || GameLogic::countDiscs(board->board()) > 0);

        // Play the script: the user owns the odd plies, using the mouse for some and the keys for others.
        for (int ply = 1; ply < 14; ply += 2)
        {
            // Wait for the turn of the user (the opponent plays by herself).
            QTRY_VERIFY_WITH_TIMEOUT(board->isInputEnabled() && GameLogic::countDiscs(board->board()) == ply, 8000);
            QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 4000);
            QCOMPARE(game->statusText(), QStringLiteral("Your turn"));

            if (ply == kScreenshotPly)
            {
                QTest::mouseMove(board, board->columnPoint(kScript[ply]));
                QTest::qWait(150);
                save(*window, QStringLiteral("gui-game"), true);
            }

            if (ply % 4 == 1)
            {
                QTest::mouseClick(board, Qt::LeftButton, Qt::NoModifier, board->columnPoint(kScript[ply]));
            }
            else
            {
                pressColumnKey(board, kScript[ply]);
            }

            // The board is locked until the server answers; the new disc shows up (the opponent answers
            // within a few milliseconds, so there is no point in looking for the "Waiting for..." text).
            QVERIFY(!board->isInputEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(GameLogic::countDiscs(board->board()) >= ply + 1, 4000);
        }

        // ---- the end ----------------------------------------------------------------------------
        QTRY_COMPARE_WITH_TIMEOUT(game->statusText(), QStringLiteral("You win!"), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 4000);
        QVERIFY(board->hasWinningLine());
        QVERIFY(!board->isInputEnabled());
        QCOMPARE(GameLogic::countDiscs(board->board()), 14);

        // The highlighted line is the diagonal of the script.
        const auto lines = GameLogic::findWinningLines(board->board(), 2, 4);
        QCOMPARE(lines.size(), size_t(1));
        QCOMPARE(lines[0].size(), size_t(4));
        QCOMPARE(lines[0][0].row, 2); // (2,3) (3,4) (4,5) (5,6)
        QCOMPARE(lines[0][0].col, 3);
        QCOMPARE(lines[0][3].row, 5);
        QCOMPARE(lines[0][3].col, 6);

        // The opponent saw the same result.
        QTRY_VERIFY_WITH_TIMEOUT(!alice->states.isEmpty() && alice->states.last().status == ProtocolGameStatus::FINISHED_WIN, 5000);
        QCOMPARE(int(*alice->states.last().winner), 2);

        QTest::qWait(450); // let the banner fade in completely
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));
        QVERIFY(!findButton(game, QStringLiteral("Leave game"))->isVisibleTo(game));
        QVERIFY(!game->needsLeaveConfirmation());
        save(*window, QStringLiteral("gui-win"), true);

        // ---- back to the lobby (a new connection, transparently) --------------------------------------
        QTest::mouseClick(findButton(game, QStringLiteral("Back to lobby")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(window->controller()->isConnected() && !window->lobbyPage()->isBusy(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(findGame(*window->lobbyPage()->model(), gameName).has_value(), 8000);
        const GameInfo over = *findGame(*window->lobbyPage()->model(), gameName);
        QVERIFY(over.status == ProtocolGameStatus::FINISHED_WIN);
        QVERIFY(!GameListModel::isJoinable(over));

        // ---- disconnect from the menu ----------------------------------------------------------------
        QAction *disconnect = findAction(window.get(), QStringLiteral("Disconnect"));
        QVERIFY(disconnect && disconnect->isEnabled());
        disconnect->trigger();
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
        QVERIFY(window->statusBar()->findChildren<QLabel *>().last()->text().contains(QStringLiteral("Not connected")));
        QVERIFY(!disconnect->isEnabled());
        QVERIFY(!window->controller()->isConnected());

        // And the window can connect again.
        QVERIFY(connectWindow(*window));
        window.reset();
        QCOMPARE(NetworkController::stuckThreadCount(), 0);
    }

    // =====================================================================================
    // Failures and unusual situations
    // =====================================================================================
    void theAboutBoxOpens()
    {
        auto window = makeWindow(QStringLiteral("127.0.0.1"), 1, QStringLiteral("Tommaso"));
        QAction *about = findAction(window.get(), QStringLiteral("About ConnectX"));
        QVERIFY(about != nullptr);

        // The box is modal: look at it, and close it, from a timer.
        bool seen = false;
        QTimer::singleShot(300, [&seen]()
                           {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box != nullptr)
            {
                seen = box->text().contains(QStringLiteral("ConnectX")) && box->text().contains(QStringLiteral("Qt"));
                box->accept();
            } });
        about->trigger();
        QVERIFY(seen);
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
    }

    void anUnreachableServerIsExplainedOnTheConnectPage()
    {
        const quint16 port = TestSupport::freePort(); // nobody listens there
        auto window = makeWindow(QStringLiteral("127.0.0.1"), port, QStringLiteral("Tommaso"));

        QTest::mouseClick(findButton(window->connectPage(), QStringLiteral("Connect")), Qt::LeftButton);
        QVERIFY(window->connectPage()->isBusy()); // right away: the form is locked, Cancel is offered
        QVERIFY(findButton(window->connectPage(), QStringLiteral("Cancel"))->isVisibleTo(window->connectPage()));

        QTRY_VERIFY_WITH_TIMEOUT(!window->connectPage()->isBusy(), 20000);
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
        QVERIFY(hasVisibleLabel(window->connectPage(), QStringLiteral("Could not connect to 127.0.0.1:%1").arg(port)));
        QVERIFY(window->statusBar()->currentMessage().contains(QStringLiteral("Could not connect")));
        QVERIFY(findButton(window->connectPage(), QStringLiteral("Connect"))->isVisibleTo(window->connectPage())); // can retry
    }

    void aSilentServerCanBeCancelled()
    {
        SilentServer silent;
        auto window = makeWindow(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Tommaso"));

        QTest::mouseClick(findButton(window->connectPage(), QStringLiteral("Connect")), Qt::LeftButton);
        QVERIFY(window->connectPage()->isBusy());
        QTest::qWait(300);

        QElapsedTimer clock;
        clock.start();
        QTest::mouseClick(findButton(window->connectPage(), QStringLiteral("Cancel")), Qt::LeftButton);
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(!window->connectPage()->isBusy());
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);

        // Nobody complains about the attempt that was given up.
        QTest::qWait(3600);
        QVERIFY(!hasVisibleLabel(window->connectPage(), QStringLiteral("did not answer")));
        QVERIFY(!window->controller()->isConnected());
    }

    void joiningAGameThatBecameFullIsExplainedInTheLobby()
    {
        ScriptedPlayer host(QStringLiteral("Host"));
        ScriptedPlayer sneaky(QStringLiteral("Sneaky"));
        QVERIFY(host.connectTo(server_.port()));
        QVERIFY(sneaky.connectTo(server_.port()));
        QVERIFY(host.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Almost full")));

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        QVERIFY(connectWindow(*window));
        QVERIFY(waitForGame(*window, QStringLiteral("Almost full")));

        // The user selects the game, and the last seat is taken before the button is pressed.
        const quint32 id = findGame(*window->lobbyPage()->model(), QStringLiteral("Almost full"))->game_id;
        window->lobbyPage()->selectGame(id);
        QVERIFY(sneaky.join(id));

        QElapsedTimer clock;
        clock.start();
        QPushButton *join = findButton(window->lobbyPage(), QStringLiteral("Join selected"));
        QVERIFY(join->isEnabled()); // the list was not refreshed yet
        QTest::mouseClick(join, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(window->lobbyPage()->message() == QStringLiteral("That game is already full."), 2500);
        QVERIFY(clock.elapsed() < 1000);                          // the refusal is reported at once
        QCOMPARE(window->currentPage(), MainWindow::Page::Lobby); // no modal dialog, no page change
        QVERIFY(!window->lobbyPage()->isBusy());
        QVERIFY(window->statusBar()->currentMessage().contains(QStringLiteral("already full")));

        // The list is refreshed right away, behind the scenes: the game is no longer offered.
        QTRY_VERIFY_WITH_TIMEOUT(!GameListModel::isJoinable(*findGame(*window->lobbyPage()->model(), QStringLiteral("Almost full"))), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!findButton(window->lobbyPage(), QStringLiteral("Join selected"))->isEnabled(), 5000);
    }

    void creatingAGameFromTheDialog()
    {
        ScriptedPlayer friendly(QStringLiteral("Friend"));
        QVERIFY(friendly.connectTo(server_.port()));

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        QVERIFY(connectWindow(*window));

        // The "New game..." button opens a modal dialog: fill it in from a timer, as a user would.
        QTimer::singleShot(300, [this]()
                           {
            auto *dialog = qobject_cast<NewGameDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->setValues(GameConfig(5, 9, 3, 5), QStringLiteral("From the dialog"));
            dialog->accept(); });
        QTest::mouseClick(findButton(window->lobbyPage(), QStringLiteral("New game…")), Qt::LeftButton);

        // The game is created and joined: the user waits for two more players.
        QTRY_VERIFY2_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game,
                                  qPrintable(QStringLiteral("still in the lobby; message: '%1'; busy: %2")
                                                 .arg(window->lobbyPage()->message())
                                                 .arg(window->lobbyPage()->isBusy())),
                                  10000);
        GamePage *game = window->gamePage();
        QCOMPARE(game->board()->rows(), 5);
        QCOMPARE(game->board()->cols(), 9);
        QCOMPARE(game->model().numPlayers(), 3);
        QCOMPARE(game->model().connectLength(), 5);
        QCOMPARE(game->model().myPlayerId(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(game->statusText(), QStringLiteral("Waiting for players (1/3)…"), 5000);
        QVERIFY(hasVisibleLabel(game, QStringLiteral("From the dialog")));
        QTest::qWait(450); // let the overlay fade in
        save(*window, QStringLiteral("gui-waiting"));

        // Others find it in their lobby and join: the game starts when the last seat is taken.
        const auto listed = [&]()
        {
            for (const GameInfo &info : friendly.list())
            {
                if (QString::fromStdString(info.game_name) == QStringLiteral("From the dialog"))
                {
                    return std::optional<GameInfo>(info);
                }
            }
            return std::optional<GameInfo>();
        }();
        QVERIFY(listed.has_value());
        QVERIFY(friendly.join(listed->game_id));
        QTRY_COMPARE_WITH_TIMEOUT(game->statusText(), QStringLiteral("Waiting for players (2/3)…"), 5000);

        ScriptedPlayer third(QStringLiteral("Third"));
        QVERIFY(third.connectTo(server_.port()));
        QVERIFY(third.join(listed->game_id));
        QTRY_VERIFY_WITH_TIMEOUT(game->model().phase() == GameModel::Phase::Playing, 5000);
        QCOMPARE(game->model().players().size(), 3);
        QVERIFY(game->model().players()[0].isMe);
    }

    void aDrawnGameEndsWithoutWinner()
    {
        ScriptedPlayer opponent(QStringLiteral("Dana"));
        QVERIFY(opponent.connectTo(server_.port()));
        opponent.strategy = [](const GameStateUpdate &state, int)
        { return TestSupport::kDrawColumns[GameLogic::countDiscs(state.board)]; };
        QVERIFY(opponent.createAndJoin(GameConfig(4, 4, 2, 4), QStringLiteral("Tiny board")));

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        QVERIFY(connectWindow(*window));
        QVERIFY(waitForGame(*window, QStringLiteral("Tiny board")));
        selectAndJoin(*window, QStringLiteral("Tiny board"));
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Game, 10000);

        // The user plays the same sequence, with the keyboard: 16 discs fill the board.
        GamePage *game = window->gamePage();
        BoardWidget *board = game->board();
        QCOMPARE(board->rows(), 4);
        QCOMPARE(board->cols(), 4);
        for (int guard = 0; guard < 16 && !game->model().isFinished(); ++guard)
        {
            QTRY_VERIFY_WITH_TIMEOUT(game->model().isFinished() || board->isInputEnabled(), 8000);
            if (game->model().isFinished())
            {
                break;
            }
            QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 4000);
            if (GameLogic::countDiscs(board->board()) == 11)
            {
                QTest::mouseMove(board, board->columnPoint(TestSupport::kDrawColumns[11]));
            }
            pressColumnKey(board, TestSupport::kDrawColumns[GameLogic::countDiscs(board->board())]);
            QTRY_VERIFY_WITH_TIMEOUT(game->model().isFinished() || !board->isInputEnabled(), 5000);
        }

        QTRY_VERIFY_WITH_TIMEOUT(game->model().isFinished(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(game->statusText(), QStringLiteral("It's a draw"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 4000);
        QCOMPARE(GameLogic::countDiscs(board->board()), 16);
        QVERIFY(!board->hasWinningLine());
        QVERIFY(!board->isInputEnabled());
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));
        QTest::qWait(450);
        save(*window, QStringLiteral("gui-draw"));

        QTRY_VERIFY_WITH_TIMEOUT(!opponent.states.isEmpty() && opponent.states.last().status == ProtocolGameStatus::FINISHED_DRAW, 5000);
    }

    void leavingAGameInProgressAsksForConfirmation()
    {
        ScriptedPlayer opponent(QStringLiteral("Opponent"));
        QVERIFY(opponent.connectTo(server_.port()));
        QVERIFY(opponent.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Stay or go")));

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->setConfirmationsEnabled(true);
        QVERIFY(connectWindow(*window));
        QVERIFY(waitForGame(*window, QStringLiteral("Stay or go")));
        selectAndJoin(*window, QStringLiteral("Stay or go"));
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game && window->gamePage()->model().phase() == GameModel::Phase::Playing, 10000);

        QPushButton *leave = findButton(window->gamePage(), QStringLiteral("Leave game"));
        QVERIFY(leave && window->gamePage()->needsLeaveConfirmation());

        // "No": stays in the game.
        QTimer::singleShot(300, []()
                           {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            QTest::keyClick(box, Qt::Key_Escape); });
        QTest::mouseClick(leave, Qt::LeftButton);
        QCOMPARE(window->currentPage(), MainWindow::Page::Game);
        QVERIFY(window->controller()->isConnected());

        // "Yes": back to the lobby, the opponent is left alone.
        QTimer::singleShot(300, []()
                           {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            QAbstractButton *yes = box->button(QMessageBox::Yes);
            QVERIFY(yes != nullptr);
            QTest::mouseClick(yes, Qt::LeftButton); });
        QTest::mouseClick(leave, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(window->controller()->isConnected(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!opponent.states.isEmpty() && opponent.states.last().players.size() == 1, 5000);
    }

    void theOpponentLeavingEndsTheGame()
    {
        ScriptedPlayer opponent(QStringLiteral("Quitter"));
        QVERIFY(opponent.connectTo(server_.port()));
        QVERIFY(opponent.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Abandoned")));

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        QVERIFY(connectWindow(*window));
        QVERIFY(waitForGame(*window, QStringLiteral("Abandoned")));
        selectAndJoin(*window, QStringLiteral("Abandoned"));
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game && window->gamePage()->model().phase() == GameModel::Phase::Playing, 10000);

        opponent.controller.disconnectFromServer(); // the opponent closes the program

        GamePage *game = window->gamePage();
        QTRY_COMPARE_WITH_TIMEOUT(game->statusText(), QStringLiteral("All other players left"), 5000);
        QVERIFY(!game->board()->isInputEnabled()); // nobody to play against
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));
        QVERIFY(!game->needsLeaveConfirmation());
        QVERIFY(window->controller()->isConnected()); // the connection itself is fine
    }

    void lostConnectionInTheMiddleOfAGame()
    {
        ServerProcess doomed;
        QVERIFY(doomed.start());

        ScriptedPlayer opponent(QStringLiteral("Opponent"));
        QVERIFY(opponent.connectTo(doomed.port()));
        QVERIFY(opponent.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Doomed")));

        auto window = makeWindow(QStringLiteral("127.0.0.1"), doomed.port(), QStringLiteral("Tommaso"));
        QVERIFY(connectWindow(*window));
        QVERIFY(waitForGame(*window, QStringLiteral("Doomed")));
        selectAndJoin(*window, QStringLiteral("Doomed"));
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game && window->gamePage()->model().phase() == GameModel::Phase::Playing, 10000);

        doomed.stop(); // the server dies

        // The board stays on screen, with the explanation and the way out.
        GamePage *game = window->gamePage();
        QTRY_COMPARE_WITH_TIMEOUT(game->statusText(), QStringLiteral("Connection lost"), 8000);
        QCOMPARE(window->currentPage(), MainWindow::Page::Game);
        QVERIFY(!game->board()->isInputEnabled());
        QVERIFY(!window->controller()->isConnected());
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));

        // "Back to lobby" tries to reconnect; the server is gone, so the connect page explains why.
        QTest::mouseClick(findButton(game, QStringLiteral("Back to lobby")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Connect, 20000);
        QVERIFY(hasVisibleLabel(window->connectPage(), QStringLiteral("Could not connect")));
    }

    void lostConnectionInTheLobby()
    {
        ServerProcess doomed;
        QVERIFY(doomed.start());
        auto window = makeWindow(QStringLiteral("127.0.0.1"), doomed.port(), QStringLiteral("Tommaso"));
        QVERIFY(connectWindow(*window));

        doomed.stop();
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Connect, 8000);
        QVERIFY(hasVisibleLabel(window->connectPage(), QStringLiteral("connection to the server was lost")));
        QVERIFY(!window->connectPage()->isBusy());
    }

    // =====================================================================================
    // Closing the window at every moment
    // =====================================================================================
    void closingTheWindowNeverHangs()
    {
        // What is measured is the destruction of the window, which stops the network threads. The close()
        // before it also writes the settings: with an .ini file on Windows that write occasionally takes
        // seconds (file locking, virus scanners), which says nothing about the shutdown of the network.
        int stage = 0;
        auto closeTimed = [&stage](std::unique_ptr<MainWindow> &window)
        {
            QElapsedTimer clock;
            clock.start();
            window->close();
            const qint64 closed = clock.elapsed();
            clock.restart();
            window.reset();
            const qint64 destroyed = clock.elapsed();
            qInfo("closing the window at stage %d: close() %lld ms, destruction %lld ms", ++stage,
                  static_cast<long long>(closed), static_cast<long long>(destroyed));
            if (closed > 20000)
            {
                qWarning("close() took %lld ms", static_cast<long long>(closed));
            }
            return destroyed;
        };

        // 1. while connecting to a server that never answers (the pending handshake is released)
        {
            SilentServer silent;
            auto window = makeWindow(QStringLiteral("127.0.0.1"), silent.port(), QStringLiteral("Tommaso"));
            window->connectNow();
            QTest::qWait(300);
            const qint64 ms = closeTimed(window);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }
        QTRY_COMPARE_WITH_TIMEOUT(NetworkController::stuckThreadCount(), 0, 8000);

        // 2. in the lobby
        {
            auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
            QVERIFY(connectWindow(*window));
            QTest::qWait(300);
            const qint64 ms = closeTimed(window);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }

        // 3. waiting for the other players
        {
            auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
            QVERIFY(connectWindow(*window));
            emit window->lobbyPage()->newGameRequested(GameConfig(6, 7, 3, 4), QStringLiteral("Closed while waiting"), OpponentChoice());
            QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Game, 10000);
            const qint64 ms = closeTimed(window);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }

        // 4. in the middle of a game, with the opponent watching
        {
            ScriptedPlayer opponent(QStringLiteral("Watcher"));
            QVERIFY(opponent.connectTo(server_.port()));
            QVERIFY(opponent.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Closed mid-game")));

            auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
            QVERIFY(connectWindow(*window));
            QVERIFY(waitForGame(*window, QStringLiteral("Closed mid-game")));
            selectAndJoin(*window, QStringLiteral("Closed mid-game"));
            QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game && window->gamePage()->model().phase() == GameModel::Phase::Playing, 10000);
            const qint64 ms = closeTimed(window);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
            QTRY_VERIFY_WITH_TIMEOUT(!opponent.states.isEmpty() && opponent.states.last().players.size() == 1, 5000);
        }

        // 5. after the game is over
        {
            ScriptedPlayer opponent(QStringLiteral("Winner"));
            QVERIFY(opponent.connectTo(server_.port()));
            opponent.strategy = [](const GameStateUpdate &, int)
            { return 0; };
            QVERIFY(opponent.createAndJoin(GameConfig(6, 7, 2, 4), QStringLiteral("Closed after the end")));

            auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
            QVERIFY(connectWindow(*window));
            QVERIFY(waitForGame(*window, QStringLiteral("Closed after the end")));
            selectAndJoin(*window, QStringLiteral("Closed after the end"));
            QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Game, 10000);

            // The user plays columns 1 and 2 alternately, the opponent stacks column 0 and wins.
            GamePage *game = window->gamePage();
            for (int turn = 0; turn < 8 && !game->model().isFinished(); ++turn)
            {
                QTRY_VERIFY_WITH_TIMEOUT(game->model().isFinished() || game->board()->isInputEnabled(), 8000);
                if (game->model().isFinished())
                {
                    break;
                }
                pressColumnKey(game->board(), 1 + turn % 2);
                QTRY_VERIFY_WITH_TIMEOUT(game->model().isFinished() || !game->board()->isInputEnabled(), 5000);
            }
            QTRY_VERIFY_WITH_TIMEOUT(game->model().isFinished(), 8000);
            QCOMPARE(game->statusText(), QStringLiteral("%1 wins").arg(QStringLiteral("Winner")));

            const qint64 ms = closeTimed(window);
            QVERIFY2(ms < 1500, qPrintable(QString::number(ms)));
        }

        // Nothing is left running.
        QCOMPARE(NetworkController::stuckThreadCount(), 0);
    }

private:
    /**
     * Saves a picture of the window when CONNECTX_GUI_SCREENSHOT_DIR is set, and the same picture with a
     * dark palette (name-dark.png) when `alsoDark` is true: the interface follows the palette, so
     * switching it on the live window is all it takes.
     */
    void save(QWidget &window, const QString &name, bool alsoDark = false)
    {
        if (screenshotDir_.isEmpty())
        {
            return;
        }

        auto grab = [&](const QString &fileName)
        {
            QTest::qWait(120); // let the pending paints happen
            const QPixmap picture = window.grab();
            const QString path = QDir(screenshotDir_).filePath(fileName + QStringLiteral(".png"));
            QVERIFY2(picture.save(path), qPrintable(path));
        };

        grab(name);
        if (alsoDark)
        {
            const QPalette original = QApplication::palette();
            QApplication::setPalette(OffscreenSupport::darkPalette());
            grab(name + QStringLiteral("-dark"));
            QApplication::setPalette(original);
            QTest::qWait(60);
        }
    }

    ServerProcess server_;
    QString screenshotDir_;
};

int main(int argc, char *argv[])
{
    OffscreenSupport::prepare();
    QApplication app(argc, argv);
    OffscreenSupport::chooseFont();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    spdlog::set_level(spdlog::level::err);

    QTemporaryDir settingsDir;
    OffscreenSupport::useIsolatedSettings(settingsDir.path());

    FlowTest test;
    const int result = OffscreenSupport::runTest(test, argc, argv);
    OffscreenSupport::removeIsolatedSettings();
    return result;
}

#include "test_flow.moc"
