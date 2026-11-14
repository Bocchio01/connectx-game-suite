// Pure logic of the GUI: winning lines, state diff, player styles and the game model.
// No window, no network.

#include <random>
#include <set>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include "core/board.hpp"
#include "core/game/rules.hpp"

#include "gui/computer_levels.hpp"
#include "gui/computer_player.hpp"
#include "gui/game_logic.hpp"
#include "gui/game_model.hpp"
#include "gui/helper_programs.hpp"
#include "gui/player_style.hpp"

#include "offscreen_support.hpp"
#include "test_builders.hpp"

using namespace GameLogic;
using Grid = GameLogic::Board; // "Board" alone would clash with the class of the core library
using TestBuilders::boardFromArt;
using TestBuilders::makeInfo;
using TestBuilders::makeState;

namespace
{
    bool contains(const std::vector<Cell> &cells, int row, int col)
    {
        for (const Cell &cell : cells)
        {
            if (cell.row == row && cell.col == col)
            {
                return true;
            }
        }
        return false;
    }
}

class LogicTest : public QObject
{
    Q_OBJECT

private slots:
    // ---- board helpers ----------------------------------------------------------
    void emptyBoardHasTheRequestedShape()
    {
        const Grid board = makeEmptyBoard(6, 7);
        QVERIFY(hasShape(board, 6, 7));
        QVERIFY(!hasShape(board, 7, 6));
        QCOMPARE(countDiscs(board), 0);
        QVERIFY(makeEmptyBoard(0, 5).empty());
        QVERIFY(makeEmptyBoard(5, -1).empty());

        Grid ragged = board;
        ragged[2].pop_back();
        QVERIFY(!hasShape(ragged, 6, 7));
    }

    void landingRowStacksFromTheBottom()
    {
        Grid board = makeEmptyBoard(4, 4);
        QCOMPARE(landingRow(board, 2), 3);
        board[3][2] = 1;
        QCOMPARE(landingRow(board, 2), 2);
        board[2][2] = 2;
        board[1][2] = 1;
        QCOMPARE(landingRow(board, 2), 0);
        board[0][2] = 2;
        QCOMPARE(landingRow(board, 2), -1); // full
        QCOMPARE(landingRow(board, 0), 3);  // other columns unaffected
        QCOMPARE(landingRow(board, -1), -1);
        QCOMPARE(landingRow(board, 4), -1);
        QCOMPARE(countDiscs(board), 4);
    }

    // ---- winning lines ------------------------------------------------------------
    void horizontalLine()
    {
        const Grid board = boardFromArt({".......",
                                          ".......",
                                          ".......",
                                          ".......",
                                          "2......",
                                          "2111122"});
        const auto lines = findWinningLines(board, 1, 4);
        QCOMPARE(lines.size(), size_t(1));
        const std::vector<Cell> expected = {{5, 1}, {5, 2}, {5, 3}, {5, 4}};
        QVERIFY(lines[0] == expected);
        QVERIFY(findWinningLines(board, 2, 4).empty());
    }

    void verticalLine()
    {
        const Grid board = boardFromArt({".......",
                                          ".......",
                                          "..1....",
                                          "..1....",
                                          "2.1.2..",
                                          "2.12.1."});
        const auto lines = findWinningLines(board, 1, 4);
        QCOMPARE(lines.size(), size_t(1));
        const std::vector<Cell> expected = {{2, 2}, {3, 2}, {4, 2}, {5, 2}}; // top to bottom
        QVERIFY(lines[0] == expected);
    }

    void diagonalDownRightLine()
    {
        const Grid board = boardFromArt({".......",
                                          ".......",
                                          "2......",
                                          "12.....",
                                          "112....",
                                          "1212..."});
        const auto lines = findWinningLines(board, 2, 4);
        QCOMPARE(lines.size(), size_t(1));
        const std::vector<Cell> expected = {{2, 0}, {3, 1}, {4, 2}, {5, 3}};
        QVERIFY(lines[0] == expected);
    }

    void diagonalUpRightLine()
    {
        const Grid board = boardFromArt({".......",
                                          ".......",
                                          "...1...",
                                          "..12...",
                                          ".122...",
                                          "1212..."});
        const auto lines = findWinningLines(board, 1, 4);
        QCOMPARE(lines.size(), size_t(1));
        const std::vector<Cell> expected = {{5, 0}, {4, 1}, {3, 2}, {2, 3}}; // bottom-left to top-right
        QVERIFY(lines[0] == expected);
    }

    void runLongerThanRequiredIsReportedWhole()
    {
        const Grid board = boardFromArt({"......",
                                          "......",
                                          "......",
                                          "......",
                                          "......",
                                          "11111."});
        const auto lines = findWinningLines(board, 1, 4);
        QCOMPARE(lines.size(), size_t(1));
        QCOMPARE(lines[0].size(), size_t(5));
        QCOMPARE(findWinningCells(board, 1, 4).size(), size_t(5));
        // ...and with a longer connect length it is not a win yet
        QVERIFY(findWinningLines(board, 1, 6).empty());
    }

    void crossingLinesAreBothReported()
    {
        // A horizontal and a vertical run sharing the disc at (3,1).
        const Grid board = boardFromArt({".1.....",
                                          ".1.....",
                                          ".1.....",
                                          "1111...",
                                          "2222...",
                                          "2222..."});
        const auto lines = findWinningLines(board, 1, 4);
        QCOMPARE(lines.size(), size_t(2));
        const auto cells = findWinningCells(board, 1, 4);
        QCOMPARE(cells.size(), size_t(7)); // 4 + 4 - the shared disc, counted once
        QVERIFY(contains(cells, 3, 1));
        QVERIFY(contains(cells, 0, 1));
        QVERIFY(contains(cells, 3, 3));
    }

    void noLineWhenTheRunIsTooShortOrBroken()
    {
        const Grid board = boardFromArt({".......",
                                          ".......",
                                          ".......",
                                          ".......",
                                          "111.111",
                                          "2212221"});
        QVERIFY(findWinningLines(board, 1, 4).empty());
        QVERIFY(findWinningCells(board, 1, 4).empty());
        QCOMPARE(findWinningLines(board, 1, 3).size(), size_t(2)); // both triples are lines for connect 3
    }

    void connectThreeOnTinyBoard()
    {
        const Grid board = boardFromArt({"....",
                                          "....",
                                          "..1.",
                                          "2111"});
        QCOMPARE(findWinningLines(board, 1, 3).size(), size_t(1)); // row 3, cols 1..3
        QVERIFY(findWinningLines(board, 1, 4).empty());
    }

    void otherPlayersDoNotCount()
    {
        const Grid board = boardFromArt({"....",
                                          "....",
                                          "....",
                                          "1212"});
        QVERIFY(findWinningLines(board, 1, 3).empty());
        QVERIFY(findWinningLines(board, 2, 3).empty());
    }

    void badInputIsHarmless()
    {
        const Grid board = boardFromArt({"1111", "....", "....", "...."});
        QVERIFY(findWinningLines(board, 0, 4).empty());
        QVERIFY(findWinningLines(board, 1, 0).empty());
        QVERIFY(findWinningLines(board, 1, -3).empty());
        QVERIFY(findWinningLines(Grid(), 1, 4).empty());

        Grid ragged = board;
        ragged[1].resize(2);
        ragged[3].resize(7);
        QVERIFY(!findWinningLines(ragged, 1, 4).empty()); // no crash, the run is still found
    }

    void agreesWithTheRulesOfTheCore()
    {
        // The server decides the winner with GameRules::checkWin: the highlight must exist exactly when it says so.
        std::mt19937 rng(20240607);
        int wins = 0;
        int noWins = 0;

        for (int iteration = 0; iteration < 4000; ++iteration)
        {
            const int rows = 4 + static_cast<int>(rng() % 7);
            const int cols = 4 + static_cast<int>(rng() % 7);
            const int connect = 3 + static_cast<int>(rng() % 3);
            const int density = 35 + static_cast<int>(rng() % 50); // percent of occupied cells

            Grid board = makeEmptyBoard(rows, cols);
            ::Board coreBoard(static_cast<uint8_t>(rows), static_cast<uint8_t>(cols), 4);
            for (int r = 0; r < rows; ++r)
            {
                for (int c = 0; c < cols; ++c)
                {
                    if (static_cast<int>(rng() % 100) < density)
                    {
                        const uint8_t player = static_cast<uint8_t>(1 + rng() % 3);
                        board[static_cast<size_t>(r)][static_cast<size_t>(c)] = player;
                        coreBoard.set(static_cast<uint8_t>(r), static_cast<uint8_t>(c), player);
                    }
                }
            }

            const GameRules rules(static_cast<uint8_t>(connect));
            for (uint8_t player = 1; player <= 3; ++player)
            {
                const bool expected = rules.checkWin(coreBoard, player);
                const auto lines = findWinningLines(board, player, connect);
                if (expected != !lines.empty())
                {
                    QFAIL(qPrintable(QStringLiteral("mismatch: %1x%2, connect %3, player %4, core says %5")
                                         .arg(rows)
                                         .arg(cols)
                                         .arg(connect)
                                         .arg(player)
                                         .arg(expected)));
                }

                // Every reported cell really belongs to the player, and every run is long enough and straight.
                for (const auto &line : lines)
                {
                    QVERIFY(static_cast<int>(line.size()) >= connect);
                    for (const Cell &cell : line)
                    {
                        QCOMPARE(board[static_cast<size_t>(cell.row)][static_cast<size_t>(cell.col)], player);
                    }
                    const int dr = line[1].row - line[0].row;
                    const int dc = line[1].col - line[0].col;
                    for (size_t i = 1; i < line.size(); ++i)
                    {
                        QCOMPARE(line[i].row - line[i - 1].row, dr);
                        QCOMPARE(line[i].col - line[i - 1].col, dc);
                    }
                }

                expected ? ++wins : ++noWins;
            }
        }

        // Make sure the random boards exercised both outcomes.
        QVERIFY2(wins > 300, qPrintable(QString::number(wins)));
        QVERIFY2(noWins > 300, qPrintable(QString::number(noWins)));
    }

    // ---- state diff -----------------------------------------------------------------
    void identicalBoardsHaveNoNewDiscs()
    {
        const Grid board = boardFromArt({"..", "..", "..", "12"});
        const auto changes = findNewDiscs(board, board);
        QVERIFY(changes.has_value());
        QVERIFY(changes->empty());
    }

    void oneNewDisc()
    {
        const Grid before = boardFromArt({"....", "....", "....", "12.."});
        const Grid after = boardFromArt({"....", "....", "....", "121."});
        const auto changes = findNewDiscs(before, after);
        QVERIFY(changes.has_value());
        QCOMPARE(changes->size(), size_t(1));
        QCOMPARE((*changes)[0].cell.row, 3);
        QCOMPARE((*changes)[0].cell.col, 2);
        QCOMPARE(int((*changes)[0].player), 1);
    }

    void severalNewDiscs()
    {
        const Grid before = makeEmptyBoard(4, 4);
        const Grid after = boardFromArt({"....", "....", ".2..", "12.."});
        const auto changes = findNewDiscs(before, after);
        QVERIFY(changes.has_value());
        QCOMPARE(changes->size(), size_t(3));
    }

    void removedOrReplacedDiscsAreNotIncremental()
    {
        const Grid before = boardFromArt({"....", "....", "....", "12.."});
        QVERIFY(!findNewDiscs(before, makeEmptyBoard(4, 4)).has_value());                              // disc removed
        QVERIFY(!findNewDiscs(before, boardFromArt({"....", "....", "....", "22.."})).has_value());    // disc replaced
        QVERIFY(!findNewDiscs(before, makeEmptyBoard(5, 4)).has_value());                              // shape changed
        QVERIFY(!findNewDiscs(before, makeEmptyBoard(4, 5)).has_value());
    }

    // ---- players --------------------------------------------------------------------
    void playerStylesAreDistinctInColourAndSymbol()
    {
        std::set<QRgb> colours;
        std::set<int> symbols;
        for (int id = 1; id <= PlayerStyles::kCount; ++id)
        {
            const PlayerStyle &style = PlayerStyles::forPlayer(id);
            colours.insert(style.color.rgb());
            symbols.insert(static_cast<int>(style.symbol));
            QVERIFY(!PlayerStyles::describe(id).isEmpty());
            QVERIFY(!PlayerStyles::symbolPath(style.symbol, QPointF(0, 0), 10.0).isEmpty());
        }
        QCOMPARE(colours.size(), size_t(PlayerStyles::kCount));
        QCOMPARE(symbols.size(), size_t(PlayerStyles::kCount));

        // Out of range ids do not crash: they wrap around.
        QCOMPARE(PlayerStyles::forPlayer(1).color, PlayerStyles::forPlayer(1 + PlayerStyles::kCount).color);
        QCOMPARE(PlayerStyles::forPlayer(0).color, PlayerStyles::forPlayer(1).color);
    }

    // ---- the computer opponent: levels, command line, which games it can play ---------------------
    void everyLevelHasTextsAndASpecOfTheRightShape()
    {
        // type[:key=value,...] (see `connectx-ai --list-agents`); the levels use these types only.
        const QRegularExpression shape(QStringLiteral("^(greedy|alphabeta|mcts|az)(:[a-z]+=[A-Za-z0-9_.]+(,[a-z]+=[A-Za-z0-9_.]+)*)?$"));
        const GameConfig standard(6, 7, 2, 4);
        const GameConfig other(5, 9, 2, 4);

        QSet<QString> names;
        for (int level = 1; level <= ComputerLevels::kCount; ++level)
        {
            QVERIFY(ComputerLevels::isValid(level));
            const QString name = ComputerLevels::name(level);
            QVERIFY(!name.isEmpty());
            names.insert(name);
            QVERIFY(!ComputerLevels::summary(level).isEmpty());
            QVERIFY(!ComputerLevels::details(level, standard, true).isEmpty());
            QVERIFY(!ComputerLevels::details(level, other, false).isEmpty());
            QCOMPARE(ComputerLevels::playerName(level), QStringLiteral("Computer (%1)").arg(name));

            for (const GameConfig &config : {standard, other})
            {
                for (const bool network : {true, false})
                {
                    const QString spec = ComputerLevels::agentSpec(level, config, network);
                    QVERIFY2(shape.match(spec).hasMatch(), qPrintable(spec));
                    // The pretrained network exists for the standard board only, and only when it was found.
                    QCOMPARE(spec.startsWith(QLatin1String("az:")), network && ComputerLevels::isStandardBoard(config) && level >= 4);
                }
            }

            // No Elo in the texts that the user sees: they are for the README.
            QVERIFY(!ComputerLevels::summary(level).contains(QRegularExpression(QStringLiteral("\\d{3,}"))));
            QVERIFY(!ComputerLevels::summary(level).contains(QLatin1String("Elo")));
        }
        QCOMPARE(names.size(), ComputerLevels::kCount); // five different names

        // Invalid levels do not crash: they fall back on the default level.
        QVERIFY(!ComputerLevels::isValid(0));
        QVERIFY(!ComputerLevels::isValid(ComputerLevels::kCount + 1));
        QCOMPARE(ComputerLevels::agentSpec(0, standard, true), ComputerLevels::agentSpec(ComputerLevels::kDefaultLevel, standard, true));
    }

    void theStrongestLevelsUseTheNetworkOnlyWhereItExists()
    {
        const GameConfig standard(6, 7, 2, 4);
        QCOMPARE(ComputerLevels::agentSpec(4, standard, true), QStringLiteral("az:model=connectx_6x7,sims=100"));
        QCOMPARE(ComputerLevels::agentSpec(5, standard, true), QStringLiteral("az:model=connectx_6x7,sims=400"));
        QCOMPARE(ComputerLevels::agentSpec(4, standard, false), QStringLiteral("mcts:sims=3000,rollout=tactical"));
        QCOMPARE(ComputerLevels::agentSpec(5, GameConfig(7, 8, 2, 4), true), QStringLiteral("mcts:sims=10000,rollout=tactical"));
        QCOMPARE(ComputerLevels::agentSpec(5, GameConfig(6, 7, 2, 5), true), QStringLiteral("mcts:sims=10000,rollout=tactical")); // connect 5
        // The first three levels do not depend on the board.
        QCOMPARE(ComputerLevels::agentSpec(1, standard, true), QStringLiteral("greedy"));
        QCOMPARE(ComputerLevels::agentSpec(2, GameConfig(5, 9, 2, 4), true), QStringLiteral("alphabeta:depth=4"));
        QCOMPARE(ComputerLevels::agentSpec(3, standard, false), QStringLiteral("alphabeta:depth=8"));
        // The texts tell how the level plays on this board.
        QVERIFY(ComputerLevels::details(4, standard, true).contains(QLatin1String("neural network")));
        QVERIFY(!ComputerLevels::details(4, standard, false).contains(QLatin1String("neural network")));
        QVERIFY(!ComputerLevels::details(4, GameConfig(5, 9, 2, 4), true).contains(QLatin1String("neural network")));
    }

    void theHelperPlaysTwoPlayerGamesOnBoardsThatFit()
    {
        QString reason;
        QVERIFY(ComputerLevels::isSupported(GameConfig(6, 7, 2, 4)));
        QVERIFY(ComputerLevels::isSupported(GameConfig(7, 8, 2, 4))); // 8 columns x 8 = 64 cells: the largest
        QVERIFY(ComputerLevels::isSupported(GameConfig(5, 9, 2, 4)));
        QVERIFY(ComputerLevels::isSupported(GameConfig(4, 12, 2, 4)));
        QVERIFY(ComputerLevels::isSupported(GameConfig(4, 4, 2, 3)));
        QVERIFY(ComputerLevels::isSupported(GameConfig(6, 7, 2, 7))); // the longest line the board allows

        QVERIFY(!ComputerLevels::isSupported(GameConfig(6, 7, 3, 4), &reason));
        QVERIFY2(reason.contains(QLatin1String("two-player")), qPrintable(reason));
        QVERIFY(!ComputerLevels::isSupported(GameConfig(6, 7, 4, 4)));

        QVERIFY(!ComputerLevels::isSupported(GameConfig(8, 8, 2, 4), &reason)); // 72 cells
        QVERIFY2(reason.contains(QLatin1String("64")), qPrintable(reason));
        QVERIFY(!ComputerLevels::isSupported(GameConfig(7, 9, 2, 4)));  // 72
        QVERIFY(!ComputerLevels::isSupported(GameConfig(9, 7, 2, 4)));  // 70
        QVERIFY(!ComputerLevels::isSupported(GameConfig(10, 6, 2, 4))); // 66
        QVERIFY(!ComputerLevels::isSupported(GameConfig(4, 13, 2, 4))); // 65
        QVERIFY(!ComputerLevels::isSupported(GameConfig(4, 20, 2, 4)));
        QVERIFY(!ComputerLevels::isSupported(GameConfig(20, 20, 2, 4)));

        QVERIFY(!ComputerLevels::isSupported(GameConfig(6, 7, 2, 2), &reason)); // too short for the helper
        QVERIFY2(reason.contains(QLatin1String("at least 3")), qPrintable(reason));
        QVERIFY(!ComputerLevels::isSupported(GameConfig(4, 4, 2, 5), &reason)); // longer than the board
        QVERIFY2(reason.contains(QLatin1String("does not fit")), qPrintable(reason));
        QVERIFY(!ComputerLevels::isSupported(GameConfig(3, 7, 2, 4))); // too small a board
    }

    void theStandardBoardIsSixBySevenConnectFour()
    {
        QVERIFY(ComputerLevels::isStandardBoard(GameConfig(6, 7, 2, 4)));
        QVERIFY(!ComputerLevels::isStandardBoard(GameConfig(7, 6, 2, 4)));
        QVERIFY(!ComputerLevels::isStandardBoard(GameConfig(6, 7, 2, 5)));
        QVERIFY(!ComputerLevels::isStandardBoard(GameConfig(6, 7, 3, 4)));
    }

    void theComputerGetsItsCommandLine()
    {
        ComputerPlayer::Options options;
        options.program = QStringLiteral("connectx-ai");
        options.host = QStringLiteral("192.168.1.20");
        options.port = 9000;
        options.gameId = 42;
        options.agent = ComputerLevels::agentSpec(3, GameConfig(6, 7, 2, 4), true);
        options.playerName = ComputerLevels::playerName(3);
        options.delayMs = 250;

        const QStringList expected = {"--host", "192.168.1.20", "--port", "9000", "-g", "42", "--agent", "alphabeta:depth=8",
                                      "-n", "Computer (Medium)", "--games", "1", "--delay", "250"};
        QCOMPARE(ComputerPlayer::arguments(options), expected);
        QCOMPARE(ComputerPlayer::Options().delayMs, 500); // the default pace: the answer does not appear instantly
    }

    void helperProgramsAreFoundInTheGivenFolderOrNowhere()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());

#ifdef Q_OS_WIN
        const QString fileName = QStringLiteral("gui-test-helper.exe");
#else
        const QString fileName = QStringLiteral("gui-test-helper");
#endif
        QVERIFY(HelperPrograms::find(QStringLiteral("gui-test-helper"), folder.path()).isEmpty());

        QFile program(QDir(folder.path()).filePath(fileName));
        QVERIFY(program.open(QIODevice::WriteOnly));
        program.write("not really a program");
        program.close();
        QVERIFY(program.setPermissions(program.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup));

        const QString found = HelperPrograms::find(QStringLiteral("gui-test-helper"), folder.path());
        QCOMPARE(QFileInfo(found).fileName(), fileName);

        // The pretrained network: looked for where the computer player looks.
        QVERIFY(!HelperPrograms::findsNetwork(QString()));
        const QByteArray modelPath = qgetenv("CONNECTX_MODEL_PATH");
        qunsetenv("CONNECTX_MODEL_PATH");
        const QString here = QDir::currentPath();
        QVERIFY(QDir().mkpath(folder.filePath(QStringLiteral("cwd"))));
        QVERIFY(QDir::setCurrent(folder.filePath(QStringLiteral("cwd")))); // (no "models" there)

        QVERIFY(!HelperPrograms::findsNetwork(found));
        QVERIFY(QDir().mkpath(folder.filePath(QStringLiteral("models"))));
        QFile model(folder.filePath(QStringLiteral("models/connectx_6x7.cxnn")));
        QVERIFY(model.open(QIODevice::WriteOnly));
        model.close();
        QVERIFY(HelperPrograms::findsNetwork(found));

        QVERIFY(QDir::setCurrent(here));
        if (!modelPath.isEmpty())
        {
            qputenv("CONNECTX_MODEL_PATH", modelPath);
        }

        QVERIFY(!HelperPrograms().canPlayOffline());
        HelperPrograms both;
        both.computerPlayer = found;
        QVERIFY(both.hasComputerPlayer() && !both.hasServer() && !both.canPlayOffline());
        both.server = found;
        QVERIFY(both.canPlayOffline());
    }

    void modelWaitsForTheComputerWhenTheOpponentIsOne()
    {
        const GameInfo info = makeInfo(5, "", 6, 7, 2, 4, 1);
        GameModel model;
        model.begin(info, 1, QStringLiteral("Alice"));
        QVERIFY(!model.hasComputerOpponent());
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for players (1/2)…"));

        model.setComputerOpponent(QStringLiteral("Computer (Easy)"));
        QVERIFY(model.hasComputerOpponent());
        QCOMPARE(model.phase(), GameModel::Phase::Waiting);
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for the computer…"));
        QCOMPARE(model.bannerTitle(), QStringLiteral("Waiting for the computer"));
        QCOMPARE(model.bannerSubtitle(), QStringLiteral("Computer (Easy) is joining the game"));
        const auto seats = model.players();
        QCOMPARE(seats.size(), 2);
        QVERIFY(seats[0].placeholder.isEmpty()); // (the user's own seat has a name)
        QCOMPARE(seats[1].placeholder, QStringLiteral("Waiting for the computer…"));

        // Once it has joined the game goes on as usual, with its name.
        model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 2, {"Alice", "Computer (Easy)"}));
        QCOMPARE(model.phase(), GameModel::Phase::Playing);
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for Computer (Easy)…"));
        QVERIFY(model.players()[1].placeholder.isEmpty());

        // A new game forgets about the computer: begin() starts from scratch.
        model.begin(makeInfo(6, "", 6, 7, 2, 4, 1), 1, QStringLiteral("Alice"));
        QVERIFY(!model.hasComputerOpponent());
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for players (1/2)…"));
    }

    // ---- game model -------------------------------------------------------------------
    void modelStartsWaiting()
    {
        GameModel model;
        QCOMPARE(model.phase(), GameModel::Phase::Idle);

        model.begin(makeInfo(5, "Arena", 6, 7, 2, 4, 1), 1, QStringLiteral("Alice"));
        QCOMPARE(model.phase(), GameModel::Phase::Waiting);
        QCOMPARE(model.rows(), 6);
        QCOMPARE(model.cols(), 7);
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for players (1/2)…"));
        QVERIFY(!model.canMove());
        QVERIFY(!model.isInProgress());

        const auto players = model.players();
        QCOMPARE(players.size(), 2);
        QCOMPARE(players[0].name, QStringLiteral("Alice"));
        QVERIFY(players[0].isMe);
        QVERIFY(players[0].present);
        QVERIFY(players[1].name.isEmpty()); // free seat
        QVERIFY(!players[1].present);
        QVERIFY(!players[1].left);
    }

    void modelFollowsTheLobby()
    {
        GameModel model;
        const GameInfo info = makeInfo(5, "", 6, 7, 3, 4, 1);
        model.begin(info, 2, QStringLiteral("Bob"));

        // Alice joined first, Bob (us) second, a third seat is free.
        QVERIFY(model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::NOT_STARTED, 1, {"Alice", "Bob"})));
        QCOMPARE(model.phase(), GameModel::Phase::Waiting);
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for players (2/3)…"));
        QCOMPARE(model.bannerTitle(), QStringLiteral("Waiting for players"));
        QCOMPARE(model.bannerSubtitle(), QStringLiteral("2 of 3 players joined"));
        QCOMPARE(model.players().size(), 3);
        QVERIFY(model.isFirstState());
    }

    void modelTurnsAndStatusTexts()
    {
        GameModel model;
        const GameInfo info = makeInfo(5, "Arena", 6, 7, 2, 4, 2);
        model.begin(info, 1, QStringLiteral("Alice"));

        const Grid empty = makeEmptyBoard(6, 7);
        model.applyState(makeState(info, empty, ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
        QCOMPARE(model.phase(), GameModel::Phase::Playing);
        QVERIFY(model.canMove());
        QVERIFY(model.isInProgress());
        QCOMPARE(model.statusText(), QStringLiteral("Your turn"));
        QVERIFY(model.players()[0].isCurrent);
        QVERIFY(!model.players()[1].isCurrent);
        QVERIFY(model.isFirstState());

        model.applyState(makeState(info, boardFromArt({".......", ".......", ".......", ".......", ".......", "1......"}),
                                   ProtocolGameStatus::IN_PROGRESS, 2, {"Alice", "Bob"}));
        QVERIFY(!model.canMove());
        QCOMPARE(model.statusText(), QStringLiteral("Waiting for Bob…"));
        QVERIFY(!model.isFirstState());
        QVERIFY(model.players()[1].isCurrent);
        QCOMPARE(countDiscs(model.board()), 1);
        QVERIFY(model.bannerTitle().isEmpty()); // no banner while playing
    }

    void modelResultsAreReported()
    {
        const GameInfo info = makeInfo(5, "Arena", 6, 7, 2, 4, 2);
        // Player 1 has four discs in column 0.
        const Grid verticalWin = boardFromArt({".......",
                                                ".......",
                                                "1......",
                                                "1......",
                                                "12.....",
                                                "12....."});

        {
            GameModel model;
            model.begin(info, 1, QStringLiteral("Alice"));
            model.applyState(makeState(info, verticalWin, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Bob"}, 1));
            QCOMPARE(model.phase(), GameModel::Phase::Won);
            QCOMPARE(model.winnerId(), 1);
            QCOMPARE(model.statusText(), QStringLiteral("You win!"));
            QCOMPARE(model.bannerTitle(), QStringLiteral("You win!"));
            QCOMPARE(model.bannerSubtitle(), QStringLiteral("4 in a row"));
            QVERIFY(model.isFinished());
            QVERIFY(!model.canMove());
            QVERIFY(model.players()[0].isWinner);
            QCOMPARE(model.winningLines().size(), size_t(1));
            QCOMPARE(model.winningLines()[0].size(), size_t(4));
        }
        {
            GameModel model;
            model.begin(info, 2, QStringLiteral("Bob"));
            model.applyState(makeState(info, verticalWin, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Bob"}, 1));
            QCOMPARE(model.statusText(), QStringLiteral("Alice wins"));
            QCOMPARE(model.bannerTitle(), QStringLiteral("Alice wins!"));
        }
        {
            GameModel model;
            model.begin(info, 2, QStringLiteral("Bob"));
            model.applyState(makeState(info, verticalWin, ProtocolGameStatus::FINISHED_DRAW, 1, {"Alice", "Bob"}));
            QCOMPARE(model.phase(), GameModel::Phase::Drawn);
            QCOMPARE(model.statusText(), QStringLiteral("It's a draw"));
            QCOMPARE(model.bannerTitle(), QStringLiteral("Draw"));
            QVERIFY(model.winningLines().empty());
            QVERIFY(model.isFinished());
        }
    }

    void modelDetectsAnAbandonedGame()
    {
        const GameInfo info = makeInfo(5, "", 6, 7, 2, 4, 2);
        GameModel model;
        model.begin(info, 1, QStringLiteral("Alice"));
        model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
        QCOMPARE(model.phase(), GameModel::Phase::Playing);

        // Bob disconnects: the server keeps the game but only lists Alice.
        GameStateUpdate left = makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice"});
        model.applyState(left);
        QCOMPARE(model.phase(), GameModel::Phase::Abandoned);
        QVERIFY(!model.canMove()); // nobody to play against
        QCOMPARE(model.statusText(), QStringLiteral("All other players left"));
        const auto players = model.players();
        QVERIFY(players[1].left);
        QVERIFY(!players[1].present);
        QCOMPARE(players[1].name, QStringLiteral("Bob")); // the name is remembered
    }

    void nobodyHasLeftOnceTheGameIsOver()
    {
        // The computer (like any player) disconnects right after the end: that is not "leaving" a finished game.
        const GameInfo info = makeInfo(5, "", 6, 7, 2, 4, 2);
        const Grid win = boardFromArt({".......", ".......", ".......", "2......", "2.1....", "2.11..."});
        GameModel model;
        model.begin(info, 1, QStringLiteral("Alice"));
        model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
        model.applyState(makeState(info, win, ProtocolGameStatus::FINISHED_WIN, 2, {"Alice"}, 2)); // Bob is gone already

        QCOMPARE(model.phase(), GameModel::Phase::Won);
        const auto players = model.players();
        QCOMPARE(players.size(), 2);
        QVERIFY(players[0].present && !players[0].left);
        QVERIFY(players[1].present && !players[1].left);
        QVERIFY(players[1].isWinner);
        QCOMPARE(players[1].name, QStringLiteral("Bob"));
    }

    void modelKeepsPlayingWhenSomeoneLeavesAThreePlayerGame()
    {
        const GameInfo info = makeInfo(5, "", 6, 7, 3, 4, 3);
        GameModel model;
        model.begin(info, 3, QStringLiteral("Carol"));
        model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 3, {"Alice", "Bob", "Carol"}));

        GameStateUpdate left = makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 3, {});
        left.players = {1, 3};
        left.player_names = {"Alice", "Carol"};
        model.applyState(left);
        QCOMPARE(model.phase(), GameModel::Phase::Playing);
        QVERIFY(model.canMove());
        QVERIFY(model.players()[1].left); // Bob
    }

    void modelToleratesAStateBeforeTheJoinNotification()
    {
        const GameInfo info = makeInfo(5, "", 6, 7, 2, 4, 2);
        GameModel model;

        // The broadcast overtook the join response...
        QVERIFY(model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 2, {"Alice", "Bob"})));
        QCOMPARE(model.phase(), GameModel::Phase::Idle);

        // ...and is applied as soon as the model knows which game and player it is about.
        model.begin(info, 2, QStringLiteral("Bob"));
        QCOMPARE(model.phase(), GameModel::Phase::Playing);
        QVERIFY(model.canMove());
        QCOMPARE(model.statusText(), QStringLiteral("Your turn"));
        QCOMPARE(model.players()[0].name, QStringLiteral("Alice"));
    }

    void modelIgnoresForeignOrMalformedStates()
    {
        const GameInfo info = makeInfo(5, "", 6, 7, 2, 4, 2);
        GameModel model;
        model.begin(info, 1, QStringLiteral("Alice"));

        GameInfo other = info;
        other.game_id = 99;
        QVERIFY(!model.applyState(makeState(other, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"})));

        GameStateUpdate broken = makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"});
        broken.board.pop_back(); // 5 rows while the state says 6
        QVERIFY(!model.applyState(broken));

        QCOMPARE(model.phase(), GameModel::Phase::Waiting); // nothing was applied
        QVERIFY(!model.isFirstState());
        QCOMPARE(countDiscs(model.board()), 0);
        QCOMPARE(model.rows(), 6);
    }

    void modelRemembersTheResultAfterADisconnection()
    {
        const GameInfo info = makeInfo(5, "", 6, 7, 2, 4, 2);
        const Grid win = boardFromArt({".......", ".......", "1......", "1......", "12.....", "12....."});

        {
            GameModel model;
            model.begin(info, 1, QStringLiteral("Alice"));
            model.applyState(makeState(info, makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
            model.markDisconnected();
            QCOMPARE(model.phase(), GameModel::Phase::Disconnected);
            QVERIFY(model.connectionLost());
            QVERIFY(!model.canMove());
            QCOMPARE(model.statusText(), QStringLiteral("Connection lost"));
        }
        {
            GameModel model;
            model.begin(info, 1, QStringLiteral("Alice"));
            model.applyState(makeState(info, win, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Bob"}, 1));
            model.markDisconnected();
            QCOMPARE(model.phase(), GameModel::Phase::Won); // the result stays
            QVERIFY(model.connectionLost());
        }
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    LogicTest test;
    return OffscreenSupport::runTest(test, argc, argv);
}

#include "test_logic.moc"
