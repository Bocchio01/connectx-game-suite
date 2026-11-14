// The computer opponent and the offline mode, end to end: the real window, a real connectx-server and the
// real connectx-ai (for the offline mode a private connectx-server of the window's own). Nothing is faked
// except where a failure is the point (a stand-in program that cannot join a game or never listens).
//
// The tests need connectx-ai and connectx-server next to the test binaries (a build with BUILD_AI=ON puts
// them there); without them every test is skipped, with a message that says so.
//
// What is checked about processes is checked by process id, never by name: other sessions run programs
// with the same names.

#include <functional>
#include <memory>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <spdlog/spdlog.h>

#include "gui/board_widget.hpp"
#include "gui/computer_levels.hpp"
#include "gui/computer_player.hpp"
#include "gui/connect_page.hpp"
#include "gui/game_logic.hpp"
#include "gui/game_page.hpp"
#include "gui/lobby_page.hpp"
#include "gui/main_window.hpp"
#include "gui/new_game_dialog.hpp"
#include "gui/private_server.hpp"

#include "offscreen_support.hpp"
#include "process_support.hpp"
#include "test_support.hpp"
#include "window_support.hpp"

using namespace WindowSupport;
using ProcessSupport::EnvironmentGuard;
using ProcessSupport::isAlive;
using ProcessSupport::waitUntilGone;
using TestSupport::ServerProcess;

namespace
{
    constexpr int kJoinTimeoutMs = 20000;

    const QString kOfflineButton = QStringLiteral("Play offline against the computer");

    /** The standard game. */
    GameConfig standardGame()
    {
        return GameConfig(6, 7, 2, 4);
    }

    /**
     * Presses "New game..." and answers the dialog like a user would: chooses the computer at `level` for the
     * game `config` and accepts. The dialog is modal (exec() returns only when it is closed), so the answer
     * comes from a timer. Both timers die with this function: nothing is left to fire during a later test.
     * Whatever happens, a dialog that is still open after a while is closed, so that a failure does not hang.
     */
    void createComputerGame(MainWindow &window, const GameConfig &config, int level)
    {
        QTimer answer;
        answer.setSingleShot(true);
        QObject::connect(&answer, &QTimer::timeout, &answer, [config, level]()
                         {
            auto *dialog = qobject_cast<NewGameDialog *>(QApplication::activeModalWidget());
            if (dialog == nullptr)
            {
                qWarning("the New game dialog is not open");
                return;
            }
            dialog->setValues(config, QString());
            auto *computer = dialog->findChild<QRadioButton *>(QStringLiteral("computerRadio"));
            auto *levels = dialog->findChild<QComboBox *>(QStringLiteral("levelCombo"));
            if (computer != nullptr && computer->isEnabled())
            {
                QTest::mouseClick(computer, Qt::LeftButton);
            }
            if (levels != nullptr)
            {
                levels->setCurrentIndex(level - 1);
            }
            dialog->accept(); });

        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &watchdog, []()
                         {
            if (QWidget *stuck = QApplication::activeModalWidget())
            {
                stuck->close();
            } });

        answer.start(300);
        watchdog.start(10000);
        QTest::mouseClick(findButton(window.lobbyPage(), QStringLiteral("New game…")), Qt::LeftButton);
    }

    /**
     * Waits for the New game dialog that the window opens by itself (offline games), lets `inspect` look at it
     * and then accepts (or rejects) it. The dialog is modal and opened from the event loop, so the test
     * function cannot look at it while it is open: the answer comes from a timer that polls. The timers die with
     * this object, so nothing is left to fire during a later test; a dialog still open after a long while is
     * closed, so that a failure does not hang the test.
     */
    class DialogWatcher
    {
    public:
        explicit DialogWatcher(std::function<void(NewGameDialog &)> inspect, bool accept = true)
        {
            poll_.setInterval(50);
            QObject::connect(&poll_, &QTimer::timeout, &poll_, [this, inspect, accept]()
                             {
                auto *dialog = qobject_cast<NewGameDialog *>(QApplication::activeModalWidget());
                if (dialog == nullptr)
                {
                    return;
                }
                poll_.stop();
                watchdog_.stop();
                inspect(*dialog); // (a failed check only leaves this function: the dialog is closed below anyway)
                seen_ = true;
                if (accept)
                {
                    dialog->accept();
                }
                else
                {
                    dialog->reject();
                } });

            watchdog_.setSingleShot(true);
            QObject::connect(&watchdog_, &QTimer::timeout, &watchdog_, []()
                             {
                if (QWidget *stuck = QApplication::activeModalWidget())
                {
                    stuck->close();
                } });

            poll_.start();
            watchdog_.start(30000);
        }

        /** True once the dialog has been seen and answered. */
        bool seen() const { return seen_; }

    private:
        QTimer poll_;
        QTimer watchdog_;
        bool seen_ = false;
    };

    /** Saves a picture of the window when CONNECTX_GUI_SCREENSHOT_DIR is set: to look at the real thing. */
    void shoot(QWidget &window, const QString &name)
    {
        const QString directory = qEnvironmentVariable("CONNECTX_GUI_SCREENSHOT_DIR");
        if (directory.isEmpty())
        {
            return;
        }
        QDir().mkpath(directory);
        QTest::qWait(450); // (let the discs land and the banners settle)
        QString fileName = name;
        fileName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("-"));
        window.grab().save(QDir(directory).filePath(QStringLiteral("computer-") + fileName + QStringLiteral(".png")));
    }

    /** The first column that is not full: the "human" plays like a robot, which is all these tests need. */
    int firstOpenColumn(const GameLogic::Board &board)
    {
        const int columns = board.empty() ? 0 : static_cast<int>(board.front().size());
        for (int column = 0; column < columns; ++column)
        {
            if (GameLogic::landingRow(board, column) >= 0)
            {
                return column;
            }
        }
        return -1;
    }

    /** Makes one move for the user if it is their turn. Returns true if a disc was dropped. */
    bool moveIfMyTurn(MainWindow &window)
    {
        GamePage *page = window.gamePage();
        BoardWidget *board = page->board();
        if (!board->isInputEnabled() || board->isAnimating())
        {
            return false;
        }
        const int column = firstOpenColumn(page->model().board());
        if (column < 0)
        {
            return false;
        }
        QTest::mouseClick(board, Qt::LeftButton, Qt::NoModifier, board->columnPoint(column));
        return true;
    }

    /** Plays the user's side until the game is over; false if it takes more than `timeoutMs`. */
    bool playUntilOver(MainWindow &window, int timeoutMs = 90000)
    {
        QElapsedTimer clock;
        clock.start();
        while (!window.gamePage()->model().isFinished() && clock.elapsed() < timeoutMs)
        {
            if (!moveIfMyTurn(window))
            {
                QTest::qWait(15);
            }
        }
        return window.gamePage()->model().isFinished();
    }

    /** Plays until `discs` are on the board (the user's moves and the answers of the computer). */
    bool playUntilDiscs(MainWindow &window, int discs, int timeoutMs = 60000)
    {
        QElapsedTimer clock;
        clock.start();
        while (GameLogic::countDiscs(window.gamePage()->model().board()) < discs && !window.gamePage()->model().isFinished() &&
               clock.elapsed() < timeoutMs)
        {
            if (!moveIfMyTurn(window))
            {
                QTest::qWait(15);
            }
        }
        return GameLogic::countDiscs(window.gamePage()->model().board()) >= discs || window.gamePage()->model().isFinished();
    }

    /** The name of the player that is not the user, as the game page lists it. */
    QString opponentName(const GamePage &page)
    {
        for (const PlayerEntry &entry : page.model().players())
        {
            if (!entry.isMe)
            {
                return entry.name;
            }
        }
        return QString();
    }
}

class ComputerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        helpers_ = HelperPrograms::locate();
        if (!helpers_.hasComputerPlayer() || !helpers_.hasServer())
        {
            QSKIP("connectx-ai and connectx-server are not next to the test binaries (configure with BUILD_AI=ON and "
                  "build everything): the end-to-end tests with the computer are skipped");
        }
        QVERIFY2(server_.start(), "cannot start connectx-server");
    }

    void cleanupTestCase()
    {
        server_.stop();
        QTRY_COMPARE_WITH_TIMEOUT(NetworkController::stuckThreadCount(), 0, 12000);
    }

    // =====================================================================================
    // The table of levels against the real program
    // =====================================================================================
    void everyLevelIsAcceptedByTheHelper()
    {
        // The helper reads its agent spec before it does anything else: a spec it does not understand ends
        // with "Error: ..." at once, a good one gets as far as trying to join the game (which does not exist).
        auto outputOf = [this](const QString &spec)
        {
            QProcess helper;
            helper.setProcessChannelMode(QProcess::MergedChannels);
            helper.start(helpers_.computerPlayer, {QStringLiteral("--agent"), spec, QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
                                                   QStringLiteral("--port"), QString::number(server_.port()), QStringLiteral("-g"),
                                                   QStringLiteral("999999"), QStringLiteral("--games"), QStringLiteral("1"),
                                                   QStringLiteral("--delay"), QStringLiteral("0")});
            if (!helper.waitForFinished(30000))
            {
                helper.kill();
                helper.waitForFinished(3000);
                return QStringLiteral("(timeout)");
            }
            return QString::fromUtf8(helper.readAll());
        };

        // (the check discriminates: a spec that is wrong is refused)
        QVERIFY(outputOf(QStringLiteral("bogus:depth=3")).contains(QLatin1String("Error:")));

        const GameConfig other(5, 9, 2, 4);
        for (int level = 1; level <= ComputerLevels::kCount; ++level)
        {
            const QString standard = ComputerLevels::agentSpec(level, standardGame(), helpers_.network);
            const QString elsewhere = ComputerLevels::agentSpec(level, other, helpers_.network);
            for (const QString &spec : {standard, elsewhere})
            {
                const QString output = outputOf(spec);
                QVERIFY2(!output.contains(QLatin1String("Error:")), qPrintable(QStringLiteral("%1: %2").arg(spec, output)));
                QVERIFY2(output.contains(QLatin1String("Could not join the game")), qPrintable(QStringLiteral("%1: %2").arg(spec, output)));
            }
        }
        if (!helpers_.network)
        {
            QWARN("the pretrained network was not found next to connectx-ai: the strongest levels were checked with their fallback only");
        }
    }

    // =====================================================================================
    // A game against the computer, from the New game dialog to "Play again"
    // =====================================================================================
    void aGameAgainstTheComputerIsPlayedToTheEnd()
    {
        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->setComputerDelayMs(0);
        QVERIFY(connectWindow(*window));

        ComputerPlayer *computer = window->computerPlayer();
        QSignalSpy started(computer, &ComputerPlayer::started);
        QSignalSpy finished(computer, &ComputerPlayer::finished);
        QSignalSpy failed(computer, &ComputerPlayer::failed);

        createComputerGame(*window, standardGame(), 1);

        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        GamePage *game = window->gamePage();
        QVERIFY(game->model().hasComputerOpponent());
        QCOMPARE(game->model().computerName(), QStringLiteral("Computer (Beginner)"));
        QCOMPARE(game->model().myPlayerId(), 1);

        // The helper runs, joins the game, and the game starts.
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 firstProcess = computer->processId();
        QVERIFY(firstProcess > 0);
        QTRY_VERIFY_WITH_TIMEOUT(game->model().phase() != GameModel::Phase::Waiting, kJoinTimeoutMs);
        QCOMPARE(opponentName(*game), QStringLiteral("Computer (Beginner)"));
        QCOMPARE(failed.count(), 0);

        QVERIFY(playUntilOver(*window));

        // The game is over: the helper ends by itself, successfully, and the user is offered a rematch.
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(computer->exitCode(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(waitUntilGone(firstProcess));

        QVERIFY(findButton(game, QStringLiteral("Play again"))->isVisibleTo(game));
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));
        QVERIFY(!findButton(game, QStringLiteral("Leave game"))->isVisibleTo(game));
        const GameConfig firstConfig = game->model().info().config;

        // "Play again": a new game with the same settings, against a new helper.
        QTest::mouseClick(findButton(game, QStringLiteral("Play again")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game && !game->model().isFinished() &&
                                     game->model().hasComputerOpponent(),
                                 10000);
        const qint64 secondProcess = computer->processId();
        QVERIFY(secondProcess > 0 && secondProcess != firstProcess);
        QCOMPARE(game->model().info().config.rows, firstConfig.rows);
        QCOMPARE(game->model().info().config.cols, firstConfig.cols);
        QCOMPARE(game->model().info().config.connect_length, firstConfig.connect_length);
        QCOMPARE(game->model().computerName(), QStringLiteral("Computer (Beginner)"));
        QTRY_VERIFY_WITH_TIMEOUT(game->model().phase() == GameModel::Phase::Playing, kJoinTimeoutMs);

        QVERIFY(playUntilOver(*window));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
        QCOMPARE(computer->exitCode(), 0);
        QVERIFY(waitUntilGone(secondProcess));
        QTRY_VERIFY_WITH_TIMEOUT(!computer->isRunning(), 3000);
    }

    void leavingTheGameStopsTheComputer()
    {
        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->setComputerDelayMs(0);
        QVERIFY(connectWindow(*window));

        QSignalSpy started(window->computerPlayer(), &ComputerPlayer::started);
        createComputerGame(*window, standardGame(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 process = window->computerPlayer()->processId();
        QTRY_VERIFY_WITH_TIMEOUT(window->gamePage()->model().phase() == GameModel::Phase::Playing, kJoinTimeoutMs);

        QTest::mouseClick(findButton(window->gamePage(), QStringLiteral("Leave game")), Qt::LeftButton);

        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QVERIFY(waitUntilGone(process, 4000));
        QTRY_VERIFY_WITH_TIMEOUT(!window->computerPlayer()->isRunning(), 3000); // (its end is reported a moment later)
    }

    void disconnectingStopsTheComputer()
    {
        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->setComputerDelayMs(0);
        QVERIFY(connectWindow(*window));

        QSignalSpy started(window->computerPlayer(), &ComputerPlayer::started);
        createComputerGame(*window, standardGame(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 process = window->computerPlayer()->processId();

        findAction(window.get(), QStringLiteral("Disconnect"))->trigger();

        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
        QVERIFY(waitUntilGone(process, 4000));
    }

    void closingTheWindowStopsTheComputer()
    {
        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->setComputerDelayMs(0);
        QVERIFY(connectWindow(*window));

        QSignalSpy started(window->computerPlayer(), &ComputerPlayer::started);
        createComputerGame(*window, standardGame(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(window->gamePage()->model().phase() == GameModel::Phase::Playing, kJoinTimeoutMs);
        const qint64 process = window->computerPlayer()->processId();
        QVERIFY(isAlive(process));

        QElapsedTimer clock;
        clock.start();
        window->close();
        window.reset();

        QVERIFY(!isAlive(process)); // gone when the window is gone, not a moment later
        QVERIFY2(clock.elapsed() < 3000, qPrintable(QString::number(clock.elapsed())));
    }

    // =====================================================================================
    // Every kind of computer answers, on the standard board and on others
    // =====================================================================================
    void eachKindOfComputerAnswers_data()
    {
        QTest::addColumn<int>("level");
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("cols");
        QTest::addColumn<int>("connect");

        QTest::newRow("Beginner, standard board") << 1 << 6 << 7 << 4;
        QTest::newRow("Easy, 5 x 9 board") << 2 << 5 << 9 << 4;
        QTest::newRow("Medium, standard board") << 3 << 6 << 7 << 4;
        QTest::newRow("Hard, standard board") << 4 << 6 << 7 << 4;
        QTest::newRow("Expert, standard board") << 5 << 6 << 7 << 4;
        QTest::newRow("Hard, 5 x 9 board") << 4 << 5 << 9 << 4;
        QTest::newRow("Expert, 7 x 8 board, connect 5") << 5 << 7 << 8 << 5;
    }

    void eachKindOfComputerAnswers()
    {
        QFETCH(int, level);
        QFETCH(int, rows);
        QFETCH(int, cols);
        QFETCH(int, connect);

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->setComputerDelayMs(0);
        QVERIFY(connectWindow(*window));

        QSignalSpy failed(window->computerPlayer(), &ComputerPlayer::failed);
        createComputerGame(*window, GameConfig(static_cast<uint8_t>(rows), static_cast<uint8_t>(cols), 2, static_cast<uint8_t>(connect)), level);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        QCOMPARE(int(window->gamePage()->model().info().config.rows), rows);
        QCOMPARE(int(window->gamePage()->model().info().config.cols), cols);

        // It joins and plays: after three moves of the user and three answers there are (at least) six discs.
        QTRY_VERIFY_WITH_TIMEOUT(window->gamePage()->model().phase() == GameModel::Phase::Playing, kJoinTimeoutMs);
        QVERIFY2(playUntilDiscs(*window, 6), "the computer did not answer");
        shoot(*window, QString::fromUtf8(QTest::currentDataTag()));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(opponentName(*window->gamePage()), ComputerLevels::playerName(level));

        const qint64 process = window->computerPlayer()->processId();
        QTest::mouseClick(findButton(window->gamePage(), QStringLiteral("Leave game")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QVERIFY(waitUntilGone(process, 4000));
    }

    // =====================================================================================
    // When the computer cannot join: the reason, Retry, Back to lobby
    // =====================================================================================
    void aProblemWithTheComputerIsShownAndCanBeRetried()
    {
        // A stand-in that stops with an error, like the real program does when it cannot join a game.
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");
        EnvironmentGuard code("FAKE_HELPER_EXIT", "1");
        EnvironmentGuard text("FAKE_HELPER_TEXT", "Joining game 1...|Could not join the game: Game is full");

        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        HelperPrograms fake = helpers_;
        fake.computerPlayer = ProcessSupport::fakeHelper();
        window->setHelperPrograms(fake);
        QVERIFY(connectWindow(*window));

        QSignalSpy started(window->computerPlayer(), &ComputerPlayer::started);
        createComputerGame(*window, standardGame(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        GamePage *game = window->gamePage();

        // The game waits for the computer, and says so...
        QCOMPARE(game->model().phase(), GameModel::Phase::Waiting);

        // ...until the helper has failed: then the page explains, with the last lines of its output.
        QTRY_VERIFY_WITH_TIMEOUT(game->hasComputerProblem(), 10000);
        QCOMPARE(game->statusText(), QStringLiteral("The computer could not join"));
        auto *output = game->findChild<QPlainTextEdit *>(QStringLiteral("computerOutput"));
        QVERIFY(output != nullptr && output->isVisibleTo(game));
        QVERIFY2(output->toPlainText().contains(QLatin1String("Could not join the game: Game is full")), qPrintable(output->toPlainText()));
        QVERIFY(hasVisibleLabel(game, QStringLiteral("exit code 1")));
        QVERIFY(findButton(game, QStringLiteral("Retry"))->isVisibleTo(game));
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));
        QVERIFY(!findButton(game, QStringLiteral("Leave game"))->isVisibleTo(game));
        QCOMPARE(started.count(), 1);

        // Retry starts it again (it fails again, with the same explanation).
        QTest::mouseClick(findButton(game, QStringLiteral("Retry")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(game->hasComputerProblem() && findButton(game, QStringLiteral("Retry"))->isVisibleTo(game), 10000);

        // Once the problem is gone (a helper that keeps running), Retry gets the game waiting again.
        qputenv("FAKE_HELPER_MODE", "run");
        QTest::mouseClick(findButton(game, QStringLiteral("Retry")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 3, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!game->hasComputerProblem(), 5000);
        QCOMPARE(game->statusText(), QStringLiteral("Waiting for the computer…"));
        QVERIFY(!findButton(game, QStringLiteral("Retry"))->isVisibleTo(game));
        const qint64 process = window->computerPlayer()->processId();
        QVERIFY(isAlive(process));

        // The way out: Back to the lobby is offered while the game cannot start; "Leave game" ends the helper.
        QTest::mouseClick(findButton(game, QStringLiteral("Leave game")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QVERIFY(waitUntilGone(process, 4000));
    }

    void aComputerThatCannotBeStartedIsExplained()
    {
        auto window = makeWindow(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        HelperPrograms broken = helpers_;
        broken.computerPlayer = QDir::temp().filePath(QStringLiteral("gui-surely-not-here/connectx-ai"));
        window->setHelperPrograms(broken);
        QVERIFY(connectWindow(*window));

        createComputerGame(*window, standardGame(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);

        GamePage *game = window->gamePage();
        QTRY_VERIFY_WITH_TIMEOUT(game->hasComputerProblem(), 10000);
        QVERIFY(hasVisibleLabel(game, QStringLiteral("Could not start the computer player")));
        QVERIFY(findButton(game, QStringLiteral("Back to lobby"))->isVisibleTo(game));

        QTest::mouseClick(findButton(game, QStringLiteral("Back to lobby")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window->lobbyPage()->isBusy(), 10000);
    }

    // =====================================================================================
    // Offline games: a private server and the computer
    // =====================================================================================
    void anOfflineGameIsPlayedOnAPrivateServer()
    {
        auto window = std::make_unique<MainWindow>();
        window->setConfirmationsEnabled(false);
        window->setComputerDelayMs(0);
        window->resize(1100, 760);
        // (the server of the form is not the one that will be used)
        window->connectPage()->setValues(QStringLiteral("no-such-server.invalid"), 1, QStringLiteral("Tommaso"));
        window->show();
        QCoreApplication::processEvents();

        QPushButton *offline = findButton(window->connectPage(), kOfflineButton);
        QVERIFY(offline != nullptr && offline->isEnabled());
        QSignalSpy started(window->computerPlayer(), &ComputerPlayer::started);
        QSignalSpy finished(window->computerPlayer(), &ComputerPlayer::finished);

        // The private server comes up, the window connects to it and opens the New game dialog by itself:
        // what is there to see at that moment is looked at from the timer that answers the dialog.
        quint16 port = 0;
        qint64 serverProcess = 0;
        DialogWatcher dialogOfTheFirstGame([&](NewGameDialog &dialog)
                                           {
            QVERIFY(window->isOffline());
            QCOMPARE(window->currentPage(), MainWindow::Page::Lobby);
            PrivateServer *server = window->privateServer();
            QVERIFY(server->isReady());
            port = server->port();
            serverProcess = server->processId();
            QVERIFY(isAlive(serverProcess));

            // The page behind the dialog says that this is a private server, not a remote one.
            QVERIFY(hasVisibleLabel(window->lobbyPage(), QStringLiteral("Offline: playing on a private server of this computer (port %1) as Tommaso").arg(port)));
            const QString status = window->statusBar()->findChildren<QLabel *>().last()->text();
            QVERIFY2(status.contains(QLatin1String("Offline")) && status.contains(QString::number(port)), qPrintable(status));

            // The dialog has the computer chosen (at the remembered level: Beginner is wanted here).
            QVERIFY(dialog.opponent().computer);
            dialog.findChild<QComboBox *>(QStringLiteral("levelCombo"))->setCurrentIndex(0); });

        QTest::mouseClick(offline, Qt::LeftButton);
        QVERIFY(window->connectPage()->isBusy()); // starting the server: only Cancel is offered
        QVERIFY(window->statusBar()->findChildren<QLabel *>().last()->text().contains(QLatin1String("Offline")));
        QTRY_VERIFY_WITH_TIMEOUT(dialogOfTheFirstGame.seen(), 30000);
        QVERIFY(port != 0 && serverProcess != 0);

        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 firstProcess = window->computerPlayer()->processId();
        QTRY_VERIFY_WITH_TIMEOUT(window->gamePage()->model().phase() != GameModel::Phase::Waiting, kJoinTimeoutMs);

        // Only this computer can reach that server.
        for (const QHostAddress &address : QNetworkInterface::allAddresses())
        {
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback() || address.isLinkLocal())
            {
                continue;
            }
            QTcpSocket outsider;
            outsider.connectToHost(address, port);
            QVERIFY2(!outsider.waitForConnected(2000), qPrintable(QStringLiteral("the private server accepts connections on %1").arg(address.toString())));
            break;
        }

        QVERIFY(playUntilOver(*window));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(window->computerPlayer()->exitCode(), 0);
        QVERIFY(waitUntilGone(firstProcess));

        // "Play again" uses the same private server (a new connection, not a new server).
        QTest::mouseClick(findButton(window->gamePage(), QStringLiteral("Play again")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game && !window->gamePage()->model().isFinished(), 10000);
        QCOMPARE(window->privateServer()->processId(), serverProcess);
        QCOMPARE(window->privateServer()->port(), port);
        QVERIFY(window->isOffline());
        const qint64 secondProcess = window->computerPlayer()->processId();
        QTRY_VERIFY_WITH_TIMEOUT(window->gamePage()->model().phase() == GameModel::Phase::Playing, kJoinTimeoutMs);

        // Ending the session (Game > Disconnect) takes the private server and the computer with it.
        findAction(window.get(), QStringLiteral("Disconnect"))->trigger();
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
        QVERIFY(!window->isOffline());
        QVERIFY(waitUntilGone(secondProcess, 4000));
        QVERIFY(waitUntilGone(serverProcess, 4000));
        QTRY_VERIFY_WITH_TIMEOUT(!window->privateServer()->isRunning(), 3000); // (its end is reported a moment after it is gone)
        QVERIFY(window->statusBar()->findChildren<QLabel *>().last()->text().contains(QLatin1String("Not connected")));

        // And the window can do it again (with a server of its own: the first one is gone).
        qint64 secondServer = 0;
        DialogWatcher dialogOfTheSecondSession([&](NewGameDialog &)
                                               {
            secondServer = window->privateServer()->processId();
            QVERIFY(secondServer != serverProcess);
            QVERIFY(isAlive(secondServer)); },
                                               false);
        QTest::mouseClick(findButton(window->connectPage(), kOfflineButton), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dialogOfTheSecondSession.seen(), 30000);
        QVERIFY(isAlive(secondServer));

        // Closing the window ends it.
        window.reset();
        QVERIFY(!isAlive(secondServer));
    }

    void quickPlayOfflineIsAGameAgainstTheComputer()
    {
        QSettings().setValue(QStringLiteral("newGame/level"), 1); // (what the quick game uses: Beginner is quick)

        auto window = std::make_unique<MainWindow>();
        window->setConfirmationsEnabled(false);
        window->setComputerDelayMs(0);
        window->resize(1100, 760);
        window->connectPage()->setValues(QStringLiteral("no-such-server.invalid"), 1, QStringLiteral("Tommaso"));
        window->show();
        QCoreApplication::processEvents();

        // The dialog that opens by itself is cancelled: the user is left in the lobby of the private server.
        DialogWatcher cancelled([](NewGameDialog &) {}, false);
        QTest::mouseClick(findButton(window->connectPage(), kOfflineButton), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(cancelled.seen(), 30000);
        QCOMPARE(window->currentPage(), MainWindow::Page::Lobby);
        QVERIFY(window->isOffline());
        const qint64 serverProcess = window->privateServer()->processId();

        // Nobody else is there: "Quick play" is a standard game against the computer.
        QPushButton *quick = findButton(window->lobbyPage(), QStringLiteral("Quick play"));
        QVERIFY2(quick->toolTip().contains(QLatin1String("against the computer")), qPrintable(quick->toolTip()));
        QSignalSpy started(window->computerPlayer(), &ComputerPlayer::started);
        QTest::mouseClick(quick, Qt::LeftButton);

        QTRY_VERIFY_WITH_TIMEOUT(window->currentPage() == MainWindow::Page::Game, 10000);
        GamePage *game = window->gamePage();
        QVERIFY(game->model().hasComputerOpponent());
        QCOMPARE(game->model().computerName(), QStringLiteral("Computer (Beginner)"));
        QCOMPARE(int(game->model().info().config.rows), 6);
        QCOMPARE(int(game->model().info().config.cols), 7);
        QCOMPARE(int(game->model().info().config.connect_length), 4);
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 process = window->computerPlayer()->processId();
        QTRY_VERIFY_WITH_TIMEOUT(game->model().phase() == GameModel::Phase::Playing, kJoinTimeoutMs);
        QVERIFY(playUntilDiscs(*window, 4));

        QTest::mouseClick(findButton(game, QStringLiteral("Leave game")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window->currentPage(), MainWindow::Page::Lobby, 10000);
        QVERIFY(waitUntilGone(process, 4000));
        QVERIFY(isAlive(serverProcess)); // (the private server lives as long as the session)

        findAction(window.get(), QStringLiteral("Disconnect"))->trigger();
        QVERIFY(waitUntilGone(serverProcess, 4000));
    }

    void anOfflineGameCanBeCancelledWhileTheServerStarts()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");
        EnvironmentGuard delay("FAKE_HELPER_DELAY_MS", "4000");

        auto window = std::make_unique<MainWindow>();
        window->setConfirmationsEnabled(false);
        window->resize(1100, 760);
        window->connectPage()->setValues(QStringLiteral("127.0.0.1"), 1, QStringLiteral("Tommaso"));
        HelperPrograms fake = helpers_;
        fake.server = ProcessSupport::fakeHelper();
        window->setHelperPrograms(fake);
        window->show();
        QCoreApplication::processEvents();

        QTest::mouseClick(findButton(window->connectPage(), kOfflineButton), Qt::LeftButton);
        QVERIFY(window->connectPage()->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(window->privateServer()->processId() != 0, 10000);
        const qint64 serverProcess = window->privateServer()->processId();

        QTest::mouseClick(findButton(window->connectPage(), QStringLiteral("Cancel")), Qt::LeftButton);

        QVERIFY(!window->connectPage()->isBusy());
        QVERIFY(!window->isOffline());
        QVERIFY(waitUntilGone(serverProcess, 4000));
        QTest::qWait(500);
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect); // the late "ready" of a cancelled start changes nothing
        QVERIFY(findButton(window->connectPage(), kOfflineButton)->isEnabled());
    }

    void aPrivateServerThatCannotStartIsExplained()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");
        EnvironmentGuard code("FAKE_HELPER_EXIT", "1");
        EnvironmentGuard text("FAKE_HELPER_TEXT", "Error creating acceptor: address already in use");

        auto window = std::make_unique<MainWindow>();
        window->setConfirmationsEnabled(false);
        window->resize(1100, 760);
        HelperPrograms fake = helpers_;
        fake.server = ProcessSupport::fakeHelper();
        window->setHelperPrograms(fake);
        window->show();
        QCoreApplication::processEvents();

        QTest::mouseClick(findButton(window->connectPage(), kOfflineButton), Qt::LeftButton);

        QTRY_VERIFY_WITH_TIMEOUT(!window->connectPage()->isBusy(), 10000);
        QCOMPARE(window->currentPage(), MainWindow::Page::Connect);
        QVERIFY(!window->isOffline());
        QVERIFY(hasVisibleLabel(window->connectPage(), QStringLiteral("could not be started")));
        QVERIFY(hasVisibleLabel(window->connectPage(), QStringLiteral("address already in use")));
        QVERIFY(findButton(window->connectPage(), kOfflineButton)->isEnabled()); // it can be tried again
    }

    void withoutTheHelpersTheComputerOptionsAreDisabled()
    {
        auto window = std::make_unique<MainWindow>();
        window->setConfirmationsEnabled(false);
        window->resize(1100, 760);
        window->setHelperPrograms(HelperPrograms()); // nothing found
        window->connectPage()->setValues(QStringLiteral("127.0.0.1"), server_.port(), QStringLiteral("Tommaso"));
        window->show();
        QCoreApplication::processEvents();

        QPushButton *offline = findButton(window->connectPage(), kOfflineButton);
        QVERIFY(offline != nullptr);
        QVERIFY(!offline->isEnabled());
        QVERIFY2(offline->toolTip().contains(QLatin1String("connectx-server")) && offline->toolTip().contains(QLatin1String("connectx-ai")),
                 qPrintable(offline->toolTip()));

        // The rest of the program works as before.
        QVERIFY(connectWindow(*window));

        QTimer look;
        look.setSingleShot(true);
        bool looked = false;
        QObject::connect(&look, &QTimer::timeout, &look, [&looked]()
                         {
            auto *dialog = qobject_cast<NewGameDialog *>(QApplication::activeModalWidget());
            if (dialog == nullptr)
            {
                qWarning("the New game dialog is not open");
                return;
            }
            auto *computer = dialog->findChild<QRadioButton *>(QStringLiteral("computerRadio"));
            QVERIFY(computer != nullptr);
            QVERIFY(!computer->isEnabled());
            QVERIFY2(computer->toolTip().contains(QLatin1String("connectx-ai")), qPrintable(computer->toolTip()));
            looked = true;
            dialog->reject(); });
        look.start(300);
        QTest::mouseClick(findButton(window->lobbyPage(), QStringLiteral("New game…")), Qt::LeftButton);
        QVERIFY(looked);
        QCOMPARE(window->currentPage(), MainWindow::Page::Lobby);
    }

private:
    HelperPrograms helpers_;
    ServerProcess server_;
};

int main(int argc, char *argv[])
{
    OffscreenSupport::prepare();
    QApplication app(argc, argv);
    OffscreenSupport::chooseFont();
    spdlog::set_level(spdlog::level::err);

    QTemporaryDir settingsDir;
    OffscreenSupport::useIsolatedSettings(settingsDir.path());

    ComputerTest test;
    const int result = OffscreenSupport::runTest(test, argc, argv);
    OffscreenSupport::removeIsolatedSettings();
    return result;
}

#include "test_computer.moc"
