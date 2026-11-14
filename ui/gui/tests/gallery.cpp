// Renders the widgets of the GUI to PNG files, without a server: a quick way to look at the
// board in many configurations (sizes, player counts, states, light/dark palette) after a change.
//
//   gui-gallery [output-dir] [--dark] [--style <name>]
//
// Runs on the "offscreen" Qt platform by default, so it works on machines without a display.
// The default style is Fusion (the same on every machine); --style windowsvista (or windows11, macOS...) shows
// the native look. Try QT_SCALE_FACTOR=2 to check the HiDPI rendering.

#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QPalette>
#include <QPixmap>
#include <QSettings>
#include <QStringList>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>

#include "gui/board_widget.hpp"
#include "gui/computer_levels.hpp"
#include "gui/connect_page.hpp"
#include "gui/game_logic.hpp"
#include "gui/game_page.hpp"
#include "gui/lobby_page.hpp"
#include "gui/new_game_dialog.hpp"

#include "offscreen_support.hpp"
#include "test_builders.hpp"

using TestBuilders::boardFromArt;
using TestBuilders::makeInfo;
using TestBuilders::makeState;

namespace
{
    QString g_outputDir;
    QString g_suffix;

    /** The helper programs as if they had been found (nothing is run: the gallery only draws the widgets). */
    HelperPrograms helpersFound()
    {
        HelperPrograms helpers;
        helpers.computerPlayer = QStringLiteral("connectx-ai");
        helpers.server = QStringLiteral("connectx-server");
        helpers.network = true;
        return helpers;
    }

    void settle(int ms = 60)
    {
        QTest::qWait(ms);
    }

    void save(QWidget &widget, const QString &name)
    {
        settle();
        const QPixmap pixmap = widget.grab();
        const QString path = QDir(g_outputDir).filePath(name + g_suffix + QStringLiteral(".png"));
        if (!pixmap.save(path))
        {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
        }
        else
        {
            std::printf("%s (%dx%d, dpr %.1f)\n", qPrintable(path), pixmap.width(), pixmap.height(), pixmap.devicePixelRatio());
        }
    }

    /** A board widget in a window-coloured parent, so that the "holes" show the page background. */
    void renderBoard(const QString &name, const GameLogic::Board &board, const QSize &size, int players, int localPlayer,
                     int hoverColumn = -1, bool animateLast = false)
    {
        (void)players;
        QWidget page;
        page.setAutoFillBackground(true);
        page.resize(size);
        auto *widget = new BoardWidget(&page);
        widget->setGeometry(0, 0, size.width(), size.height());
        widget->setLocalPlayer(localPlayer);
        widget->reset(static_cast<int>(board.size()), static_cast<int>(board.front().size()));
        widget->setBoard(board, animateLast);
        page.show();

        if (hoverColumn >= 0)
        {
            widget->setInputEnabled(true);
            QTest::mouseMove(widget, widget->columnPoint(hoverColumn));
        }
        save(page, name);
    }
}

int main(int argc, char *argv[])
{
    OffscreenSupport::prepare();

    QApplication app(argc, argv);
    OffscreenSupport::chooseFont();

    // Never touch the real settings of the user.
    QTemporaryDir settingsDir;
    OffscreenSupport::useIsolatedSettings(settingsDir.path());

    g_outputDir = QDir::currentPath();
    bool dark = false;
    QString style = QStringLiteral("Fusion");
    const QStringList arguments = app.arguments().mid(1);
    for (int i = 0; i < arguments.size(); ++i)
    {
        if (arguments[i] == QLatin1String("--dark"))
        {
            dark = true;
        }
        else if (arguments[i] == QLatin1String("--style") && i + 1 < arguments.size())
        {
            style = arguments[++i]; // e.g. windowsvista, windows11, macOS: what the user gets by default
        }
        else
        {
            g_outputDir = arguments[i];
        }
    }
    QDir().mkpath(g_outputDir);

    if (QStyle *created = QStyleFactory::create(style))
    {
        QApplication::setStyle(created);
    }
    else
    {
        std::fprintf(stderr, "unknown style '%s', available: %s\n", qPrintable(style), qPrintable(QStyleFactory::keys().join(QStringLiteral(", "))));
    }

    if (dark)
    {
        QApplication::setPalette(OffscreenSupport::darkPalette());
        g_suffix = QStringLiteral("-dark");
    }
    if (style.compare(QStringLiteral("Fusion"), Qt::CaseInsensitive) != 0)
    {
        g_suffix += QStringLiteral("-") + style.toLower();
    }

    // ---- the board --------------------------------------------------------------
    renderBoard(QStringLiteral("board-6x7-play"),
                boardFromArt({".......",
                              ".......",
                              ".......",
                              "...2...",
                              "..12...",
                              "1.212.1"}),
                QSize(760, 640), 2, 1, 3);

    renderBoard(QStringLiteral("board-8x10-3-players"),
                boardFromArt({"..........",
                              "..........",
                              "..........",
                              "..........",
                              "....3.....",
                              "...13.....",
                              "..1232.2..",
                              ".21123311."}),
                QSize(900, 700), 3, 2, 6);

    renderBoard(QStringLiteral("board-4x4-connect3"),
                boardFromArt({"....",
                              "....",
                              ".12.",
                              "2121"}),
                QSize(420, 440), 2, 1, 0);

    renderBoard(QStringLiteral("board-4-players"),
                boardFromArt({"........",
                              "........",
                              "........",
                              "...4....",
                              "..341...",
                              ".2143...",
                              "12321.4.",
                              "43214123"}),
                QSize(780, 780), 4, 4, 5);

    {
        GameLogic::Board big = GameLogic::makeEmptyBoard(20, 20);
        for (int r = 8; r < 20; ++r)
        {
            for (int c = 0; c < 20; ++c)
            {
                big[static_cast<size_t>(r)][static_cast<size_t>(c)] = static_cast<uint8_t>(((r * 7 + c * 3) % 3) + 1);
            }
        }
        renderBoard(QStringLiteral("board-20x20"), big, QSize(920, 820), 3, 1, 9);
    }

    // Aspect ratio: the board must stay undistorted in very tall and very wide widgets.
    const GameLogic::Board mid = boardFromArt({".......", ".......", ".......", "...2...", "..12...", "1.212.1"});
    renderBoard(QStringLiteral("board-tall"), mid, QSize(320, 720), 2, 1, 2);
    renderBoard(QStringLiteral("board-wide"), mid, QSize(1000, 360), 2, 1, 4);

    // The drop animation, caught while the disc is falling.
    {
        QWidget page;
        page.setAutoFillBackground(true);
        page.resize(640, 560);
        auto *widget = new BoardWidget(&page);
        widget->setGeometry(0, 0, 640, 560);
        widget->reset(6, 7);
        page.show();
        GameLogic::Board board = GameLogic::makeEmptyBoard(6, 7);
        board[5][2] = 1;
        widget->setBoard(board, false);
        board[4][2] = 2;
        widget->setBoard(board, true);
        QTest::qWait(190);
        save(page, QStringLiteral("board-falling"));
        QTest::qWait(900);
        save(page, QStringLiteral("board-landed"));
    }

    // Win: highlighted line and banner.
    {
        QWidget page;
        page.setAutoFillBackground(true);
        page.resize(760, 640);
        auto *widget = new BoardWidget(&page);
        widget->setGeometry(0, 0, 760, 640);
        widget->reset(6, 7);
        page.show();
        const GameLogic::Board board = boardFromArt({".......",
                                                      ".......",
                                                      "..1....",
                                                      "..12...",
                                                      "..1221.",
                                                      "1.1212."});
        widget->setBoard(board, false);
        widget->setWinningLines(GameLogic::findWinningLines(board, 1, 4));
        widget->setOverlay(QStringLiteral("You win!"), QStringLiteral("4 in a row"), BoardWidget::OverlayStyle::Banner);
        QTest::qWait(400);
        save(page, QStringLiteral("board-win"));
    }

    // Waiting for players.
    {
        QWidget page;
        page.setAutoFillBackground(true);
        page.resize(760, 640);
        auto *widget = new BoardWidget(&page);
        widget->setGeometry(0, 0, 760, 640);
        widget->reset(6, 7);
        widget->setOverlay(QStringLiteral("Waiting for players"), QStringLiteral("1 of 2 players joined"), BoardWidget::OverlayStyle::Centered);
        page.show();
        QTest::qWait(400);
        save(page, QStringLiteral("board-waiting"));
    }

    // ---- the pages --------------------------------------------------------------
    {
        ConnectPage page;
        page.setValues(QStringLiteral("localhost"), 8080, QStringLiteral("tommaso"));
        page.resize(1040, 700);
        page.show();
        save(page, QStringLiteral("page-connect-no-helpers")); // (the offline button is off: nothing to run it with)

        page.setOfflineAvailable(true);
        save(page, QStringLiteral("page-connect"));

        page.setBusy(true, QStringLiteral("Connecting to localhost:8080…"));
        save(page, QStringLiteral("page-connect-busy"));

        page.setBusy(true, QStringLiteral("Starting a private server on this computer…"));
        save(page, QStringLiteral("page-connect-offline-busy"));

        page.setBusy(false);
        page.setError(QStringLiteral("Could not connect to localhost:8080. Is the server running, and are the address and the port correct?"));
        save(page, QStringLiteral("page-connect-error"));
    }

    {
        LobbyPage page;
        page.setConnection(QStringLiteral("localhost"), 8080, QStringLiteral("tommaso"));
        page.resize(1040, 700);
        page.show();
        save(page, QStringLiteral("page-lobby-empty"));

        page.setGames({makeInfo(1, "Coffee break", 6, 7, 2, 4, 1),
                       makeInfo(2, "Big board", 10, 12, 3, 5, 2),
                       makeInfo(3, "", 6, 7, 2, 4, 2, ProtocolGameStatus::IN_PROGRESS),
                       makeInfo(4, "Tiny", 4, 4, 2, 3, 2, ProtocolGameStatus::FINISHED_WIN),
                       makeInfo(5, "Four of us", 8, 8, 4, 4, 3)});
        page.selectGame(2);
        save(page, QStringLiteral("page-lobby"));

        page.showError(QStringLiteral("That game is already full."));
        save(page, QStringLiteral("page-lobby-error"));
    }

    {
        // The lobby of an offline game: nothing in it, and it says that the server is a private one.
        LobbyPage page;
        page.setHelperPrograms(helpersFound());
        page.setConnection(QStringLiteral("127.0.0.1"), 50321, QStringLiteral("tommaso"), true);
        page.resize(1040, 700);
        page.show();
        save(page, QStringLiteral("page-lobby-offline"));
    }

    {
        NewGameDialog dialog{helpersFound()};
        dialog.setValues(NewGameDialog::defaultConfig(), QString());
        dialog.setOpponent(OpponentChoice{false, 0});
        dialog.adjustSize();
        dialog.show();
        save(dialog, QStringLiteral("dialog-new-game"));

        // The computer, at the default level: one line about the level, the details in a tooltip.
        dialog.setOpponent(OpponentChoice{true, ComputerLevels::kDefaultLevel});
        dialog.adjustSize();
        save(dialog, QStringLiteral("dialog-new-game-computer"));

        // The strongest level on a board the pretrained network does not know.
        dialog.setValues(GameConfig(5, 9, 2, 4), QStringLiteral("Wide one"));
        dialog.setOpponent(OpponentChoice{true, 4});
        dialog.adjustSize();
        save(dialog, QStringLiteral("dialog-new-game-computer-wide"));

        // Not for this game: three players.
        dialog.setValues(GameConfig(6, 7, 3, 4), QString());
        dialog.adjustSize();
        save(dialog, QStringLiteral("dialog-new-game-computer-unavailable"));
    }

    {
        // Without the helper program the choice is off, and the dialog says which program is missing.
        NewGameDialog dialog{HelperPrograms()};
        dialog.setValues(NewGameDialog::defaultConfig(), QString());
        dialog.adjustSize();
        dialog.show();
        save(dialog, QStringLiteral("dialog-new-game-no-computer"));
    }

    {
        GamePage page;
        page.resize(1040, 700);
        page.show();

        const GameInfo info = makeInfo(7, "Coffee break", 6, 7, 2, 4, 1);
        page.startGame(info, 1, QStringLiteral("tommaso"));
        save(page, QStringLiteral("page-game-waiting"));

        const GameLogic::Board playing = boardFromArt({".......",
                                                        ".......",
                                                        ".......",
                                                        "...2...",
                                                        "..12...",
                                                        "1.212.1"});
        page.applyState(makeState(info, GameLogic::makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"tommaso", "Alice"}));
        page.applyState(makeState(info, playing, ProtocolGameStatus::IN_PROGRESS, 1, {"tommaso", "Alice"}));
        QTest::mouseMove(page.board(), page.board()->columnPoint(3));
        QTest::qWait(900);
        save(page, QStringLiteral("page-game-playing"));

        page.applyState(makeState(info, playing, ProtocolGameStatus::IN_PROGRESS, 2, {"tommaso", "Alice"}));
        save(page, QStringLiteral("page-game-opponent-turn"));

        const GameLogic::Board won = boardFromArt({".......",
                                                    ".......",
                                                    "..1....",
                                                    "..12...",
                                                    "..1221.",
                                                    "1.1212."});
        page.applyState(makeState(info, won, ProtocolGameStatus::FINISHED_WIN, 1, {"tommaso", "Alice"}, 1));
        QTest::qWait(1200);
        save(page, QStringLiteral("page-game-win"));
    }

    {
        // Against the computer: waiting for it to join, then playing, a problem, and the end.
        GamePage page;
        page.resize(1040, 700);
        page.show();

        const GameInfo info = makeInfo(7, "", 6, 7, 2, 4, 1);
        page.startGame(info, 1, QStringLiteral("tommaso"), QStringLiteral("Computer (Medium)"));
        QTest::qWait(450);
        save(page, QStringLiteral("page-game-computer-waiting"));

        page.showComputerProblem(QStringLiteral("The computer player stopped with an error (exit code 1)."),
                                 QStringLiteral("Joining game 7...\nCould not join the game: Game is full"));
        QTest::qWait(450);
        save(page, QStringLiteral("page-game-computer-problem"));
        page.clearComputerProblem();

        const GameLogic::Board playing = boardFromArt({".......",
                                                        ".......",
                                                        ".......",
                                                        "...2...",
                                                        "..12...",
                                                        "1.212.1"});
        page.applyState(makeState(info, GameLogic::makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"tommaso", "Computer (Medium)"}));
        page.applyState(makeState(info, playing, ProtocolGameStatus::IN_PROGRESS, 1, {"tommaso", "Computer (Medium)"}));
        QTest::mouseMove(page.board(), page.board()->columnPoint(3));
        QTest::qWait(900);
        save(page, QStringLiteral("page-game-computer-playing"));

        page.applyState(makeState(info, playing, ProtocolGameStatus::IN_PROGRESS, 2, {"tommaso", "Computer (Medium)"}));
        save(page, QStringLiteral("page-game-computer-thinking"));

        const GameLogic::Board lost = boardFromArt({".......",
                                                     ".......",
                                                     "..2....",
                                                     "..21...",
                                                     "..2111.",
                                                     "1.2212."});
        page.applyState(makeState(info, lost, ProtocolGameStatus::FINISHED_WIN, 2, {"tommaso", "Computer (Medium)"}, 2));
        QTest::qWait(1200);
        save(page, QStringLiteral("page-game-computer-over"));
    }

    {
        // Four players, someone left.
        GamePage page;
        page.resize(1040, 700);
        page.show();
        const GameInfo info = makeInfo(9, "", 8, 8, 4, 4, 4);
        page.startGame(info, 3, QStringLiteral("Carol"));
        const GameLogic::Board board = boardFromArt({"........",
                                                      "........",
                                                      "........",
                                                      "........",
                                                      "........",
                                                      "...4....",
                                                      "..31....",
                                                      ".2143..."});
        GameStateUpdate state = makeState(info, GameLogic::makeEmptyBoard(8, 8), ProtocolGameStatus::IN_PROGRESS, 3, {"Alice", "Bob", "Carol", "Dave"});
        page.applyState(state);
        state = makeState(info, board, ProtocolGameStatus::IN_PROGRESS, 3, {"Alice", "Bob", "Carol", "Dave"});
        state.players = {1, 3, 4};
        state.player_names = {"Alice", "Carol", "Dave"};
        page.applyState(state);
        QTest::qWait(900);
        save(page, QStringLiteral("page-game-4-players"));
    }

    OffscreenSupport::removeIsolatedSettings();
    return 0;
}
