// The widgets, rendered and driven offscreen: board, game list, lobby, dialogs, connect page, game page.
// No network: the pages are fed with synthetic data.

#include <algorithm>
#include <cstdlib>
#include <memory>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

#include "gui/board_widget.hpp"
#include "gui/computer_levels.hpp"
#include "gui/connect_page.hpp"
#include "gui/game_list_model.hpp"
#include "gui/game_logic.hpp"
#include "gui/game_page.hpp"
#include "gui/lobby_page.hpp"
#include "gui/new_game_dialog.hpp"
#include "gui/player_style.hpp"

#include "offscreen_support.hpp"
#include "test_builders.hpp"

using Grid = GameLogic::Board;
using TestBuilders::boardFromArt;
using TestBuilders::makeInfo;
using TestBuilders::makeState;

namespace
{
    QPushButton *findButton(QWidget *root, const QString &text)
    {
        for (QPushButton *button : root->findChildren<QPushButton *>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    bool hasVisibleLabel(QWidget *root, const QString &text)
    {
        for (QLabel *label : root->findChildren<QLabel *>())
        {
            if (label->isVisibleTo(root) && label->text() == text)
            {
                return true;
            }
        }
        return false;
    }

    bool isNear(const QColor &a, const QColor &b, int tolerance)
    {
        return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance &&
               std::abs(a.blue() - b.blue()) <= tolerance;
    }

    QString describe(const QColor &c)
    {
        return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
    }

    /** A shown, standalone board. */
    std::unique_ptr<BoardWidget> makeBoard(int rows, int cols, QSize size = QSize(700, 620))
    {
        auto board = std::make_unique<BoardWidget>();
        board->resize(size);
        board->reset(rows, cols);
        board->show();
        QCoreApplication::processEvents();
        return board;
    }

    /** Colour of the body of the disc in a hole: off-centre, to stay clear of the symbol. */
    QColor discBodyColor(const QImage &image, const BoardWidget &board, int row, int col)
    {
        const QPoint center = board.cellPoint(row, col);
        const int offset = static_cast<int>(board.cellSize() * 0.24);
        return image.pixelColor(center.x() + offset, center.y());
    }

    GameInfo gameInfo(uint32_t id, const char *name, int current, ProtocolGameStatus status, int players = 2)
    {
        return makeInfo(id, name, 6, 7, players, 4, current, status);
    }
}

class WidgetsTest : public QObject
{
    Q_OBJECT

private slots:
    // ================================ BoardWidget ================================
    void boardStartsEmpty()
    {
        auto board = makeBoard(6, 7);
        QCOMPARE(board->rows(), 6);
        QCOMPARE(board->cols(), 7);
        QCOMPARE(GameLogic::countDiscs(board->board()), 0);
        QVERIFY(!board->isAnimating());
        QVERIFY(!board->isInputEnabled());
        QVERIFY(!board->lastMove().has_value());
        QVERIFY(!board->hasWinningLine());
        QVERIFY(board->minimumSizeHint().width() > 0);
        QVERIFY(board->sizeHint().width() > board->minimumSizeHint().width());
    }

    void boardAnimatesOnlyTheNewDisc()
    {
        auto board = makeBoard(6, 7);

        // Several discs at once (e.g. the first state of a game): shown at once, no marker.
        const Grid start = boardFromArt({".......", ".......", ".......", "...2...", "..12...", "1.212.1"});
        board->setBoard(start, true);
        QVERIFY(!board->isAnimating());
        QVERIFY(!board->lastMove().has_value());

        // One new disc: it falls, and becomes the last move.
        Grid next = start;
        next[2][3] = 1;
        board->setBoard(next, true);
        QVERIFY(board->isAnimating());
        QVERIFY(board->lastMove().has_value());
        QCOMPARE(board->lastMove()->row, 2);
        QCOMPARE(board->lastMove()->col, 3);
        QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 3000);
        QCOMPARE(board->lastMove()->row, 2); // the marker stays after the landing

        // animate = false: marked, but not animated.
        Grid third = next;
        third[1][3] = 2;
        board->setBoard(third, false);
        QVERIFY(!board->isAnimating());
        QCOMPARE(board->lastMove()->row, 1);

        // The same board again changes nothing.
        board->setBoard(third, true);
        QVERIFY(!board->isAnimating());
        QCOMPARE(board->lastMove()->row, 1);
    }

    void boardCompletesAnAnimationWhenTheNextStateArrives()
    {
        auto board = makeBoard(6, 7);
        Grid one = GameLogic::makeEmptyBoard(6, 7);
        one[5][0] = 1;
        board->setBoard(one, false);

        Grid two = one;
        two[5][1] = 2;
        board->setBoard(two, true);
        QVERIFY(board->isAnimating());

        Grid three = two;
        three[4][1] = 1;
        board->setBoard(three, true); // the first fall is completed at once, the new one starts
        QVERIFY(board->isAnimating());
        QCOMPARE(board->lastMove()->row, 4);
        QCOMPARE(GameLogic::countDiscs(board->board()), 3);
        QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 3000);
    }

    void boardAdoptsANewShapeAndResets()
    {
        auto board = makeBoard(6, 7);
        board->setBoard(boardFromArt({"....", "....", ".12.", "2121"}), true);
        QCOMPARE(board->rows(), 4);
        QCOMPARE(board->cols(), 4);
        QVERIFY(!board->isAnimating());

        board->reset(8, 9);
        QCOMPARE(board->rows(), 8);
        QCOMPARE(board->cols(), 9);
        QCOMPARE(GameLogic::countDiscs(board->board()), 0);
        QVERIFY(!board->lastMove().has_value());
    }

    void boardClicksSelectAColumn()
    {
        auto board = makeBoard(6, 7);
        board->setInputEnabled(true);
        QSignalSpy spy(board.get(), &BoardWidget::columnActivated);

        for (int column = 0; column < 7; ++column)
        {
            QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->columnPoint(column));
            QCOMPARE(spy.count(), column + 1);
            QCOMPARE(spy.last().first().toInt(), column);
        }

        // Clicking on a hole of the column works too.
        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->cellPoint(3, 4));
        QCOMPARE(spy.count(), 8);
        QCOMPARE(spy.last().first().toInt(), 4);

        // Not with the other buttons, nor outside the board.
        QTest::mouseClick(board.get(), Qt::RightButton, Qt::NoModifier, board->columnPoint(1));
        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));
        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, QPoint(board->width() - 3, board->height() - 3));
        QCOMPARE(spy.count(), 8);
    }

    void boardIgnoresInputWhenDisabled()
    {
        auto board = makeBoard(6, 7);
        QSignalSpy spy(board.get(), &BoardWidget::columnActivated);

        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->columnPoint(2));
        QTest::keyClick(board.get(), Qt::Key_3);
        QTest::keyClick(board.get(), Qt::Key_Return);
        QCOMPARE(spy.count(), 0);

        board->setInputEnabled(true);
        QTest::keyClick(board.get(), Qt::Key_3);
        QCOMPARE(spy.count(), 1);

        board->setInputEnabled(false);
        QTest::keyClick(board.get(), Qt::Key_3);
        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->columnPoint(2));
        QCOMPARE(spy.count(), 1);
    }

    void boardRefusesFullColumns()
    {
        auto board = makeBoard(6, 7);
        board->setBoard(boardFromArt({"..1....",
                                      "..2....",
                                      "..1....",
                                      "..2....",
                                      "..1....",
                                      "..2...."}),
                        false);
        board->setInputEnabled(true);
        QSignalSpy spy(board.get(), &BoardWidget::columnActivated);

        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->columnPoint(2));
        QTest::keyClick(board.get(), Qt::Key_3);
        QCOMPARE(spy.count(), 0);

        QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->columnPoint(3));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().first().toInt(), 3);
    }

    void boardKeyboardControl()
    {
        auto board = makeBoard(6, 7);
        board->setInputEnabled(true);
        QSignalSpy spy(board.get(), &BoardWidget::columnActivated);
        auto last = [&spy]()
        { return spy.last().first().toInt(); };

        // Number keys drop straight away.
        QTest::keyClick(board.get(), Qt::Key_4);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(last(), 3);

        // Arrows move a cursor, Enter/Space drop.
        QTest::keyClick(board.get(), Qt::Key_Left);
        QCOMPARE(spy.count(), 1);
        QTest::keyClick(board.get(), Qt::Key_Return);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(last(), 2);

        QTest::keyClick(board.get(), Qt::Key_Right);
        QTest::keyClick(board.get(), Qt::Key_Right);
        QTest::keyClick(board.get(), Qt::Key_Space);
        QCOMPARE(spy.count(), 3);
        QCOMPARE(last(), 4);

        QTest::keyClick(board.get(), Qt::Key_Home);
        QTest::keyClick(board.get(), Qt::Key_Enter);
        QCOMPARE(last(), 0);

        QTest::keyClick(board.get(), Qt::Key_End);
        QTest::keyClick(board.get(), Qt::Key_Return);
        QCOMPARE(last(), 6);

        // The cursor stops at the edges.
        QTest::keyClick(board.get(), Qt::Key_Right);
        QTest::keyClick(board.get(), Qt::Key_Return);
        QCOMPARE(last(), 6);

        // Keys beyond the number of columns do nothing.
        const int before = spy.count();
        QTest::keyClick(board.get(), Qt::Key_8);
        QTest::keyClick(board.get(), Qt::Key_9);
        QTest::keyClick(board.get(), Qt::Key_0);
        QCOMPARE(spy.count(), before);
    }

    void boardKeepsItsAspectRatioWhenResized()
    {
        const QList<QSize> sizes = {QSize(300, 330), QSize(900, 330), QSize(300, 900), QSize(1200, 800), QSize(640, 480)};
        for (const QSize &size : sizes)
        {
            auto board = makeBoard(6, 7, size);
            board->setInputEnabled(true);
            QSignalSpy spy(board.get(), &BoardWidget::columnActivated);

            const double cell = (board->columnPoint(6).x() - board->columnPoint(0).x()) / 6.0;
            QVERIFY2(cell > 8.0, qPrintable(QStringLiteral("cell %1 in %2x%3").arg(cell).arg(size.width()).arg(size.height())));

            // Columns equally spaced, cells square.
            for (int c = 1; c < 7; ++c)
            {
                QVERIFY(std::abs((board->columnPoint(c).x() - board->columnPoint(c - 1).x()) - cell) <= 1.5);
            }
            for (int r = 1; r < 6; ++r)
            {
                QVERIFY(std::abs((board->cellPoint(r, 0).y() - board->cellPoint(r - 1, 0).y()) - cell) <= 1.5);
            }

            // The whole board is inside the widget, drop zone included.
            QVERIFY(board->columnPoint(0).x() - cell / 2 >= 0);
            QVERIFY(board->columnPoint(6).x() + cell / 2 <= size.width());
            QVERIFY(board->columnPoint(0).y() - cell / 2 >= 0);
            QVERIFY(board->cellPoint(5, 0).y() + cell / 2 <= size.height());

            // And the mouse maps to the right column at every size.
            for (int c = 0; c < 7; ++c)
            {
                QTest::mouseClick(board.get(), Qt::LeftButton, Qt::NoModifier, board->cellPoint(2, c));
                QCOMPARE(spy.last().first().toInt(), c);
            }
            QCOMPARE(spy.count(), 7);
        }
    }

    void boardPaintsDiscsHolesAndGhost()
    {
        auto board = makeBoard(6, 7);
        board->setLocalPlayer(1);
        board->setBoard(boardFromArt({".......", ".......", ".......", ".......", ".......", "1234..."}), false);

        const QColor window = board->palette().color(QPalette::Window);
        QImage image = board->grab().toImage();

        // Discs of the four players are visible through the holes, in their own colours.
        for (int player = 1; player <= 4; ++player)
        {
            const QColor body = discBodyColor(image, *board, 5, player - 1);
            const QColor expected = PlayerStyles::forPlayer(player).color;
            QVERIFY2(isNear(body, expected, 55), qPrintable(QStringLiteral("player %1: %2 instead of %3").arg(player).arg(describe(body), describe(expected))));
        }

        // Empty holes really are holes: they show the background of the page.
        const QColor hole = image.pixelColor(board->cellPoint(1, 5));
        QVERIFY2(isNear(hole, window, 6), qPrintable(describe(hole)));

        // Between two holes there is the (blue) face of the board.
        const QPoint a = board->cellPoint(2, 2);
        const QPoint b = board->cellPoint(2, 3);
        const QColor face = image.pixelColor((a.x() + b.x()) / 2, a.y());
        QVERIFY2(face.blue() > face.red() + 50 && face.blue() > 120, qPrintable(describe(face)));

        // No ghost without input...
        const QPoint drop3 = board->columnPoint(3);
        QVERIFY(isNear(image.pixelColor(drop3), window, 6));

        // ...and a ghost disc of the local player over the hovered column with it.
        board->setInputEnabled(true);
        QTest::mouseMove(board.get(), drop3);
        image = board->grab().toImage();
        const int offset = static_cast<int>(board->cellSize() * 0.24);
        const QColor ghost = image.pixelColor(drop3.x() + offset, drop3.y());
        QVERIFY2(ghost.red() > ghost.green() + 25 && ghost.red() > ghost.blue() + 25, qPrintable(describe(ghost))); // reddish
        QVERIFY(isNear(image.pixelColor(board->columnPoint(5)), window, 6)); // other columns stay clear
    }

    void boardShowsTheWinningLineOnlyAfterTheLanding()
    {
        auto board = makeBoard(6, 7);
        const Grid before = boardFromArt({".......", ".......", ".......", "1......", "1......", "1....2."});
        const Grid after = boardFromArt({".......", ".......", "1......", "1......", "1......", "1....2."});
        const QColor yellow = PlayerStyles::forPlayer(2).color;

        board->setBoard(before, false);
        board->setBoard(after, true); // player 1 completes the column: the disc falls
        board->setWinningLines(GameLogic::findWinningLines(after, 1, 4));
        QVERIFY(board->hasWinningLine());
        QVERIFY(board->isAnimating());

        // While it falls the other discs are untouched...
        QImage image = board->grab().toImage();
        QColor loser = discBodyColor(image, *board, 5, 5);
        QVERIFY2(isNear(loser, yellow, 55), qPrintable(describe(loser)));

        // ...once it has landed they fade, so that the line stands out.
        QTRY_VERIFY_WITH_TIMEOUT(!board->isAnimating(), 3000);
        QTest::qWait(60);
        image = board->grab().toImage();
        loser = discBodyColor(image, *board, 5, 5);
        QVERIFY2(!isNear(loser, yellow, 55), qPrintable(describe(loser)));
        const QColor winner = discBodyColor(image, *board, 5, 0);
        QVERIFY2(isNear(winner, PlayerStyles::forPlayer(1).color, 55), qPrintable(describe(winner)));
    }

    void boardCentersItsContent()
    {
        auto board = makeBoard(6, 7, QSize(900, 500));
        const QImage image = board->grab().toImage();

        // Bounding box of the blue face.
        int left = image.width();
        int right = -1;
        for (int y = 0; y < image.height(); y += 3)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                const QColor c = image.pixelColor(x, y);
                if (c.blue() > c.red() + 60 && c.blue() > 120)
                {
                    left = std::min(left, x);
                    right = std::max(right, x);
                }
            }
        }
        QVERIFY(right > left);
        QVERIFY2(std::abs((left + right) / 2 - image.width() / 2) <= 3, qPrintable(QStringLiteral("%1..%2 in %3").arg(left).arg(right).arg(image.width())));
    }

    // ================================ lobby ================================
    void listModelMergesInPlace()
    {
        GameListModel model;
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

        const GameInfo g1 = gameInfo(1, "Alpha", 1, ProtocolGameStatus::NOT_STARTED);
        const GameInfo g2 = gameInfo(2, "", 2, ProtocolGameStatus::IN_PROGRESS);
        const GameInfo g3 = gameInfo(3, "Gamma", 1, ProtocolGameStatus::NOT_STARTED, 3);

        model.setGames({g3, g1, g2}); // any order in, sorted by id out
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.gameAt(0)->game_id, 1u);
        QCOMPARE(model.gameAt(1)->game_id, 2u);
        QCOMPARE(model.gameAt(2)->game_id, 3u);
        QCOMPARE(model.rowForId(3), 2);
        QCOMPARE(model.rowForId(42), -1);
        QCOMPARE(inserted.count(), 3);

        // Texts.
        QCOMPARE(model.data(model.index(0, GameListModel::NameColumn)).toString(), QStringLiteral("Alpha"));
        QCOMPARE(model.data(model.index(1, GameListModel::NameColumn)).toString(), QStringLiteral("(unnamed)"));
        QCOMPARE(model.data(model.index(0, GameListModel::BoardColumn)).toString(), QStringLiteral("6 × 7"));
        QCOMPARE(model.data(model.index(0, GameListModel::PlayersColumn)).toString(), QStringLiteral("1 / 2"));
        QCOMPARE(model.data(model.index(2, GameListModel::PlayersColumn)).toString(), QStringLiteral("1 / 3"));
        QCOMPARE(model.data(model.index(0, GameListModel::ConnectColumn)).toString(), QStringLiteral("4"));
        QCOMPARE(model.data(model.index(0, GameListModel::StatusColumn)).toString(), QStringLiteral("Waiting for players"));
        QCOMPARE(model.data(model.index(1, GameListModel::StatusColumn)).toString(), QStringLiteral("In progress"));
        QVERIFY(model.data(model.index(0, 0), GameListModel::JoinableRole).toBool());
        QVERIFY(!model.data(model.index(1, 0), GameListModel::JoinableRole).toBool());
        QCOMPARE(model.data(model.index(2, 0), GameListModel::GameIdRole).toUInt(), 3u);

        // g2 is gone, g1 got a player, g4 is new: one removal, one insertion, one update, no reset.
        inserted.clear();
        GameInfo g1b = g1;
        g1b.current_players = 2;
        const GameInfo g4 = gameInfo(4, "Delta", 0, ProtocolGameStatus::NOT_STARTED);
        model.setGames({g1b, g3, g4});
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(inserted.count(), 1);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(model.gameAt(0)->game_id, 1u);
        QCOMPARE(model.gameAt(1)->game_id, 3u);
        QCOMPARE(model.gameAt(2)->game_id, 4u);
        QCOMPARE(model.data(model.index(0, GameListModel::PlayersColumn)).toString(), QStringLiteral("2 / 2"));

        // The same list again: nothing happens.
        inserted.clear();
        removed.clear();
        changed.clear();
        model.setGames({g1b, g3, g4});
        QCOMPARE(inserted.count() + removed.count() + changed.count(), 0);

        QVERIFY(GameListModel::isJoinable(g1));
        QVERIFY(!GameListModel::isJoinable(g1b)); // full
        QVERIFY(!GameListModel::isJoinable(g2));  // started
    }

    void lobbyKeepsTheSelectionAcrossRefreshes()
    {
        LobbyPage page;
        page.resize(900, 600);
        page.show();

        const GameInfo g1 = gameInfo(1, "Alpha", 1, ProtocolGameStatus::NOT_STARTED);
        const GameInfo g2 = gameInfo(2, "Beta", 1, ProtocolGameStatus::NOT_STARTED);
        const GameInfo g3 = gameInfo(3, "Gamma", 1, ProtocolGameStatus::NOT_STARTED);
        page.setGames({g1, g2, g3});
        page.selectGame(2);
        QCOMPARE(page.selectedGameId(), 2u);

        // The list changes around the selected game.
        GameInfo g2b = g2;
        g2b.current_players = 2; // now full
        const GameInfo g0 = gameInfo(7, "Zeta", 1, ProtocolGameStatus::NOT_STARTED);
        page.setGames({g2b, g3, g0});
        QCOMPARE(page.selectedGameId(), 2u);

        // When the selected game disappears the selection is cleared.
        page.setGames({g3, g0});
        QCOMPARE(page.selectedGameId(), 0u);
    }

    void lobbyButtonsFollowTheSelection()
    {
        LobbyPage page;
        page.resize(900, 600);
        page.show();
        QPushButton *join = findButton(&page, QStringLiteral("Join selected"));
        QPushButton *quick = findButton(&page, QStringLiteral("Quick play"));
        QPushButton *create = findButton(&page, QStringLiteral("New game…"));
        QPushButton *disconnect = findButton(&page, QStringLiteral("Disconnect"));
        QVERIFY(join && quick && create && disconnect);
        QTableView *table = page.findChild<QTableView *>();
        QVERIFY(table);

        // Nothing to list: the hint replaces the table.
        QVERIFY(!table->isVisibleTo(&page));
        QVERIFY(!join->isEnabled());
        QVERIFY(quick->isEnabled());

        QSignalSpy joins(&page, &LobbyPage::joinRequested);
        QSignalSpy quicks(&page, &LobbyPage::quickPlayRequested);
        QSignalSpy disconnects(&page, &LobbyPage::disconnectRequested);

        page.setGames({gameInfo(1, "Open", 1, ProtocolGameStatus::NOT_STARTED),
                       gameInfo(2, "Running", 2, ProtocolGameStatus::IN_PROGRESS),
                       gameInfo(3, "Over", 2, ProtocolGameStatus::FINISHED_WIN)});
        QVERIFY(table->isVisibleTo(&page));
        QVERIFY(!join->isEnabled()); // nothing selected

        page.selectGame(2);
        QVERIFY(!join->isEnabled()); // started
        page.selectGame(3);
        QVERIFY(!join->isEnabled()); // finished
        page.selectGame(1);
        QVERIFY(join->isEnabled()); // open

        QTest::mouseClick(join, Qt::LeftButton);
        QCOMPARE(joins.count(), 1);
        QCOMPARE(joins.last().first().toUInt(), 1u);

        QTest::mouseClick(quick, Qt::LeftButton);
        QCOMPARE(quicks.count(), 1);
        QTest::mouseClick(disconnect, Qt::LeftButton);
        QCOMPARE(disconnects.count(), 1);

        // Enter on the table joins the selected game.
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Return);
        QCOMPARE(joins.count(), 2);

        // Double click joins an open game; on a game that cannot be joined it only explains why.
        auto doubleClick = [table](const QModelIndex &index)
        {
            // QtTest sends a lone double click event: the item view wants the press before it.
            const QPoint where = table->visualRect(index).center();
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, where);
            QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, where);
        };
        doubleClick(page.model()->index(page.model()->rowForId(1), 1));
        QCOMPARE(joins.count(), 3);
        QCOMPARE(joins.last().first().toUInt(), 1u);
        doubleClick(page.model()->index(page.model()->rowForId(2), 1));
        QCOMPARE(joins.count(), 3); // a game that already started cannot be joined
        QVERIFY(!page.message().isEmpty());

        // Busy: no new action can be started.
        page.setBusy(true, QStringLiteral("Joining…"));
        QVERIFY(!quick->isEnabled());
        QVERIFY(!create->isEnabled());
        QVERIFY(!join->isEnabled());
        QCOMPARE(page.message(), QStringLiteral("Joining…"));
        QVERIFY(disconnect->isEnabled()); // but one can always give up
        page.setBusy(false);
        QVERIFY(quick->isEnabled());
        QVERIFY(create->isEnabled());
    }

    void lobbyRefreshesByItselfWhileVisible()
    {
        LobbyPage page;
        QSignalSpy spy(&page, &LobbyPage::refreshRequested);

        QElapsedTimer clock;
        page.show();
        clock.start();
        QCOMPARE(spy.count(), 1); // at once, when the page appears

        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 3500);
        QVERIFY2(clock.elapsed() > 1500 && clock.elapsed() < 3200, qPrintable(QString::number(clock.elapsed())));

        page.hide();
        const int stopped = spy.count();
        QTest::qWait(LobbyPage::kRefreshIntervalMs + 300);
        QCOMPARE(spy.count(), stopped); // nothing while hidden
    }

    // ================================ dialogs and connect page ================================
    void newGameDialogHonoursTheServerLimits()
    {
        NewGameDialog dialog{HelperPrograms()};
        const GameConfig defaults = dialog.config();
        QCOMPARE(int(defaults.rows), 6);
        QCOMPARE(int(defaults.cols), 7);
        QCOMPARE(int(defaults.num_players), 2);
        QCOMPARE(int(defaults.connect_length), 4);
        QVERIFY(dialog.gameName().isEmpty());

        // Everything is clamped into the accepted ranges (board 4..20, players 2..4, connect 3..max side).
        dialog.setValues(GameConfig(3, 30, 9, 2), QStringLiteral("  Rematch  "));
        GameConfig clamped = dialog.config();
        QCOMPARE(int(clamped.rows), 4);
        QCOMPARE(int(clamped.cols), 20);
        QCOMPARE(int(clamped.num_players), 4);
        QCOMPARE(int(clamped.connect_length), 3);
        QCOMPARE(dialog.gameName(), QStringLiteral("Rematch"));

        // The connect length cannot exceed the longest side of the board.
        dialog.setValues(GameConfig(4, 4, 2, 9), QString());
        QCOMPARE(int(dialog.config().connect_length), 4);
        dialog.setValues(GameConfig(20, 4, 2, 15), QString());
        QCOMPARE(int(dialog.config().connect_length), 15);
        dialog.setValues(GameConfig(5, 6, 3, 6), QString());
        QCOMPARE(int(dialog.config().connect_length), 6);
    }

    void newGameDialogRemembersTheLastNumbers()
    {
        {
            NewGameDialog dialog{HelperPrograms()};
            dialog.setValues(GameConfig(9, 11, 3, 5), QStringLiteral("not remembered"));
            dialog.accept();
        }
        NewGameDialog next{HelperPrograms()};
        const GameConfig config = next.config();
        QCOMPARE(int(config.rows), 9);
        QCOMPARE(int(config.cols), 11);
        QCOMPARE(int(config.num_players), 3);
        QCOMPARE(int(config.connect_length), 5);
        QVERIFY(next.gameName().isEmpty()); // names must be unique: never reused
    }

    void connectPageValidatesAndRemembers()
    {
        ConnectPage page;
        page.resize(900, 600);
        page.show();
        QSignalSpy requests(&page, &ConnectPage::connectRequested);
        QSignalSpy cancels(&page, &ConnectPage::cancelRequested);

        QVERIFY(!ConnectPage::defaultPlayerName().isEmpty());
        QVERIFY(ConnectPage::defaultPlayerName().size() <= 32);

        // Missing values are refused with a message, nothing is sent.
        page.setValues(QString(), 8080, QStringLiteral("Zoe"));
        page.submit();
        QCOMPARE(requests.count(), 0);
        page.setValues(QStringLiteral("localhost"), 8080, QStringLiteral("   "));
        page.submit();
        QCOMPARE(requests.count(), 0);

        // Valid values are trimmed, sent and remembered.
        page.setValues(QStringLiteral("  example.org "), 9123, QStringLiteral(" Zoe  "));
        page.submit();
        QCOMPARE(requests.count(), 1);
        QCOMPARE(requests.last().at(0).toString(), QStringLiteral("example.org"));
        QCOMPARE(requests.last().at(1).toInt(), 9123);
        QCOMPARE(requests.last().at(2).toString(), QStringLiteral("Zoe"));

        QSettings settings;
        QCOMPARE(settings.value(QStringLiteral("connection/host")).toString(), QStringLiteral("example.org"));
        QCOMPARE(settings.value(QStringLiteral("connection/port")).toInt(), 9123);
        QCOMPARE(settings.value(QStringLiteral("connection/name")).toString(), QStringLiteral("Zoe"));

        ConnectPage again; // a new page starts from the remembered values
        QCOMPARE(again.host(), QStringLiteral("example.org"));
        QCOMPARE(int(again.port()), 9123);
        QCOMPARE(again.playerName(), QStringLiteral("Zoe"));

        // Busy: the form is locked, only Cancel is offered, and a second submit does nothing.
        QPushButton *connectButton = findButton(&page, QStringLiteral("Connect"));
        QPushButton *cancelButton = findButton(&page, QStringLiteral("Cancel"));
        QVERIFY(connectButton && cancelButton);
        QVERIFY(connectButton->isVisibleTo(&page));
        QVERIFY(!cancelButton->isVisibleTo(&page));
        page.setBusy(true, QStringLiteral("Connecting to example.org:9123…"));
        QVERIFY(!connectButton->isVisibleTo(&page));
        QVERIFY(cancelButton->isVisibleTo(&page));
        QVERIFY(page.isBusy());
        page.submit();
        QCOMPARE(requests.count(), 1);
        QTest::mouseClick(cancelButton, Qt::LeftButton);
        QCOMPARE(cancels.count(), 1);

        page.setBusy(false);
        QVERIFY(connectButton->isVisibleTo(&page));
        QVERIFY(!cancelButton->isVisibleTo(&page));
        page.setError(QStringLiteral("Could not connect."));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Could not connect.")));
        page.setError(QString());
        QVERIFY(!hasVisibleLabel(&page, QStringLiteral("Could not connect.")));
    }

    // ================================ the computer opponent ================================
    void newGameDialogOffersTheComputerWhenItCanPlay()
    {
        QSettings().remove(QStringLiteral("newGame/opponent"));
        QSettings().remove(QStringLiteral("newGame/level"));

        HelperPrograms helpers;
        helpers.computerPlayer = QStringLiteral("connectx-ai");
        helpers.network = true;

        NewGameDialog dialog(helpers);
        dialog.setValues(NewGameDialog::defaultConfig(), QString()); // (not the numbers that other tests remembered)
        dialog.show();
        auto *person = dialog.findChild<QRadioButton *>(QStringLiteral("personRadio"));
        auto *computer = dialog.findChild<QRadioButton *>(QStringLiteral("computerRadio"));
        auto *levels = dialog.findChild<QComboBox *>(QStringLiteral("levelCombo"));
        QVERIFY(person && computer && levels);

        // By default the game is for another person, and the strength selector is not in the way.
        QVERIFY(person->isChecked());
        QVERIFY(computer->isEnabled());
        QVERIFY(!levels->isVisibleTo(&dialog));
        QVERIFY(!dialog.opponent().computer);
        QCOMPARE(dialog.opponent().level, 0);
        QVERIFY(dialog.isComputerAvailable());

        // Choosing the computer shows the five levels, with the default one selected.
        QTest::mouseClick(computer, Qt::LeftButton);
        QVERIFY(computer->isChecked());
        QVERIFY(levels->isVisibleTo(&dialog));
        QCOMPARE(levels->count(), ComputerLevels::kCount);
        QCOMPARE(levels->currentIndex(), ComputerLevels::kDefaultLevel - 1);
        QVERIFY(dialog.opponent().computer);
        QCOMPARE(dialog.opponent().level, ComputerLevels::kDefaultLevel);

        // Every level has a one-line description under the selector and the details as a tooltip.
        for (int level = 1; level <= ComputerLevels::kCount; ++level)
        {
            levels->setCurrentIndex(level - 1);
            QCOMPARE(levels->itemText(level - 1), ComputerLevels::name(level));
            QVERIFY(hasVisibleLabel(&dialog, ComputerLevels::summary(level)));
            QCOMPARE(levels->toolTip(), ComputerLevels::details(level, dialog.config(), true));
            QCOMPARE(levels->itemData(level - 1, Qt::ToolTipRole).toString(), ComputerLevels::details(level, dialog.config(), true));
            QCOMPARE(dialog.opponent().level, level);
            QVERIFY(dialog.opponent().computer);
        }

        // The sentence at the bottom follows the choice.
        levels->setCurrentIndex(0);
        QVERIFY(hasVisibleLabel(&dialog, QStringLiteral("You play against the computer (Beginner) on a 6 × 7 board. The first to line up 4 discs wins.")));
        QTest::mouseClick(person, Qt::LeftButton);
        QVERIFY(!levels->isVisibleTo(&dialog));
        QVERIFY(hasVisibleLabel(&dialog, QStringLiteral("2 players take turns on a 6 × 7 board. The first to line up 4 discs wins.")));

        // The strength is described for the board: the network only plays the standard one.
        QTest::mouseClick(computer, Qt::LeftButton);
        levels->setCurrentIndex(3);
        QVERIFY(levels->toolTip().contains(QLatin1String("neural network")));
        dialog.setValues(GameConfig(5, 9, 2, 4), QString());
        QVERIFY(dialog.opponent().computer); // (this board is fine for the computer too)
        QVERIFY(!levels->toolTip().contains(QLatin1String("neural network")));
    }

    void theComputerIsDisabledWithAReasonWhenItCannotPlay()
    {
        QSettings().remove(QStringLiteral("newGame/opponent"));
        QSettings().remove(QStringLiteral("newGame/level"));

        HelperPrograms helpers;
        helpers.computerPlayer = QStringLiteral("connectx-ai");

        NewGameDialog dialog(helpers);
        dialog.setValues(NewGameDialog::defaultConfig(), QString());
        dialog.show();
        auto *person = dialog.findChild<QRadioButton *>(QStringLiteral("personRadio"));
        auto *computer = dialog.findChild<QRadioButton *>(QStringLiteral("computerRadio"));
        auto *note = dialog.findChild<QLabel *>(QStringLiteral("computerNote"));
        QVERIFY(person && computer && note);
        QVERIFY(computer->isEnabled());
        QVERIFY(!note->isVisibleTo(&dialog));

        // More than two players: the computer plays two-player games only.
        dialog.setValues(GameConfig(6, 7, 3, 4), QString());
        QVERIFY(!computer->isEnabled());
        QVERIFY2(computer->toolTip().contains(QLatin1String("two-player")), qPrintable(computer->toolTip()));
        QVERIFY(note->isVisibleTo(&dialog));
        QVERIFY(note->text().contains(QLatin1String("two-player")));
        QCOMPARE(person->text(), QStringLiteral("Other people"));
        QTest::mouseClick(computer, Qt::LeftButton); // (nothing happens)
        QVERIFY(person->isChecked());
        QVERIFY(!dialog.opponent().computer);
        QString reason;
        QVERIFY(!dialog.isComputerAvailable(&reason));
        QVERIFY(reason.contains(QLatin1String("two-player")));

        // A board that does not fit the engine of the computer.
        dialog.setValues(GameConfig(8, 8, 2, 4), QString());
        QVERIFY(!computer->isEnabled());
        QVERIFY2(computer->toolTip().contains(QLatin1String("64")), qPrintable(computer->toolTip()));
        QCOMPARE(person->text(), QStringLiteral("Another person"));

        // Back to a game it can play: available again (but not chosen on its own).
        dialog.setValues(GameConfig(7, 8, 2, 4), QString());
        QVERIFY(computer->isEnabled());
        QVERIFY(!computer->isChecked());
        QVERIFY(!note->isVisibleTo(&dialog));
        QVERIFY(!computer->toolTip().contains(QLatin1String("64")));

        // Chosen, then the game is changed into one it cannot play: the choice falls back on a person.
        QTest::mouseClick(computer, Qt::LeftButton);
        QVERIFY(dialog.opponent().computer);
        dialog.setValues(GameConfig(7, 8, 4, 4), QString());
        QVERIFY(person->isChecked());
        QVERIFY(!dialog.opponent().computer);
        QVERIFY(!computer->isEnabled());

        // setOpponent() does not choose what is not available.
        dialog.setOpponent(OpponentChoice{true, 3});
        QVERIFY(!dialog.opponent().computer);
        dialog.setValues(GameConfig(6, 7, 2, 4), QString());
        dialog.setOpponent(OpponentChoice{true, 3});
        QVERIFY(dialog.opponent().computer);
        QCOMPARE(dialog.opponent().level, 3);

        // Without the helper program there is no computer at all, and the dialog says which program is missing.
        NewGameDialog without{HelperPrograms()};
        without.show();
        auto *noComputer = without.findChild<QRadioButton *>(QStringLiteral("computerRadio"));
        QVERIFY(noComputer != nullptr && !noComputer->isEnabled());
        QVERIFY2(noComputer->toolTip().contains(QLatin1String("connectx-ai")), qPrintable(noComputer->toolTip()));
        without.setOpponent(OpponentChoice{true, 2});
        QVERIFY(!without.opponent().computer);
        QVERIFY(!without.isComputerAvailable());
    }

    void newGameDialogRemembersTheOpponent()
    {
        QSettings().remove(QStringLiteral("newGame/opponent"));
        QSettings().remove(QStringLiteral("newGame/level"));

        HelperPrograms helpers;
        helpers.computerPlayer = QStringLiteral("connectx-ai");

        {
            NewGameDialog dialog(helpers);
            dialog.setValues(NewGameDialog::defaultConfig(), QString()); // (what is remembered with the choice)
            dialog.setOpponent(OpponentChoice{true, 4});
            dialog.accept();
        }
        {
            NewGameDialog next(helpers);
            QVERIFY(next.opponent().computer);
            QCOMPARE(next.opponent().level, 4);

            // A game that the computer cannot play does not make the dialog forget the choice (nothing is
            // stored about the opponent for it).
            next.setValues(GameConfig(6, 7, 3, 4), QString());
            QVERIFY(!next.opponent().computer);
            next.accept();
        }
        QCOMPARE(QSettings().value(QStringLiteral("newGame/opponent")).toString(), QStringLiteral("computer"));
        QCOMPARE(QSettings().value(QStringLiteral("newGame/level")).toInt(), 4);
        {
            // It opens with the numbers of that game, which the computer cannot play: a person, until the user
            // changes them (and the computer is not chosen on its own when it becomes possible again).
            NewGameDialog third(helpers);
            QCOMPARE(int(third.config().num_players), 3);
            QVERIFY(!third.opponent().computer);
            third.setValues(NewGameDialog::defaultConfig(), QString());
            QVERIFY(!third.opponent().computer);
            third.setOpponent(OpponentChoice{true, 0}); // (level 0: the remembered level)
            QVERIFY(third.opponent().computer);
            QCOMPARE(third.opponent().level, 4);
            third.accept();
        }
        {
            NewGameDialog fourth(helpers);
            QVERIFY(fourth.opponent().computer);
            QCOMPARE(fourth.opponent().level, 4);

            // Choosing a person is remembered as well.
            fourth.setOpponent(OpponentChoice{false, 0});
            fourth.accept();
        }
        NewGameDialog last(helpers);
        QVERIFY(!last.opponent().computer);
    }

    void connectPageOffersOfflineGamesOnlyWhenPossible()
    {
        ConnectPage page;
        page.resize(900, 600);
        page.show();
        QPushButton *offline = findButton(&page, QStringLiteral("Play offline against the computer"));
        QVERIFY(offline != nullptr);
        QSignalSpy offlineRequests(&page, &ConnectPage::offlineRequested);
        QSignalSpy connectRequests(&page, &ConnectPage::connectRequested);

        // Until the window has looked for the helper programs the button is off.
        QVERIFY(!offline->isEnabled());
        QVERIFY(!page.isOfflineAvailable());
        page.submitOffline();
        QCOMPARE(offlineRequests.count(), 0);
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Offline games are not available."))); // (what `--offline` would show)

        // Without them it stays off, and its tooltip says why.
        page.setOfflineAvailable(false, QStringLiteral("Offline games need the connectx-server program."));
        QVERIFY(!offline->isEnabled());
        QCOMPARE(offline->toolTip(), QStringLiteral("Offline games need the connectx-server program."));
        page.submitOffline();
        QCOMPARE(offlineRequests.count(), 0);
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Offline games need the connectx-server program.")));

        QVERIFY(hasVisibleLabel(&page, QStringLiteral("No server yet? Run connectx-server on this machine and connect to localhost.")));

        page.setOfflineAvailable(true);
        QVERIFY(offline->isEnabled());
        QVERIFY(page.isOfflineAvailable());
        QVERIFY(offline->toolTip().contains(QLatin1String("private server")));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("No server yet? Play offline against the computer, or run connectx-server on this machine "
                                                      "and connect to localhost.")));

        // A name is needed, and nothing is sent without one.
        page.setValues(QStringLiteral("example.org"), 9123, QStringLiteral("   "));
        QTest::mouseClick(offline, Qt::LeftButton);
        QCOMPARE(offlineRequests.count(), 0);
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Please enter a player name.")));

        // With one: the name (trimmed) is sent, remembered, and the server of the form is left alone.
        const QString rememberedHost = QSettings().value(QStringLiteral("connection/host")).toString();
        page.setValues(QStringLiteral("example.org"), 9123, QStringLiteral(" Zoe "));
        QTest::mouseClick(offline, Qt::LeftButton);
        QCOMPARE(offlineRequests.count(), 1);
        QCOMPARE(offlineRequests.last().at(0).toString(), QStringLiteral("Zoe"));
        QCOMPARE(connectRequests.count(), 0);
        QCOMPARE(QSettings().value(QStringLiteral("connection/name")).toString(), QStringLiteral("Zoe"));
        QCOMPARE(QSettings().value(QStringLiteral("connection/host")).toString(), rememberedHost);
        QVERIFY(!hasVisibleLabel(&page, QStringLiteral("Please enter a player name.")));

        // Busy (connecting, or starting the private server): the button goes off with the rest of the form.
        page.setBusy(true, QStringLiteral("Starting a private server on this computer…"));
        QVERIFY(!offline->isEnabled());
        page.submitOffline();
        QCOMPARE(offlineRequests.count(), 1);
        page.setBusy(false);
        QVERIFY(offline->isEnabled());

        // Becoming unavailable while busy is remembered for later.
        page.setBusy(true, QStringLiteral("..."));
        page.setOfflineAvailable(false, QStringLiteral("gone"));
        page.setBusy(false);
        QVERIFY(!offline->isEnabled());
    }

    void lobbyPageSaysWhenTheServerIsPrivate()
    {
        LobbyPage page;
        page.resize(900, 600);
        page.show();

        page.setConnection(QStringLiteral("example.org"), 8080, QStringLiteral("Zoe"));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Connected to example.org:8080 as Zoe")));

        QPushButton *quick = findButton(&page, QStringLiteral("Quick play"));
        QVERIFY(quick != nullptr);
        QVERIFY(quick->toolTip().contains(QLatin1String("Join the first open game")));

        page.setConnection(QStringLiteral("127.0.0.1"), 50123, QStringLiteral("Zoe"), true);
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Offline: playing on a private server of this computer (port 50123) as Zoe")));
        QCOMPARE(quick->toolTip(), QStringLiteral("Start a standard 6 × 7 game against the computer")); // nobody else is there
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("No games yet.\nPress “Quick play” to start a standard game against the computer, "
                                                      "or choose the game yourself with “New game…”.")));

        page.setConnection(QStringLiteral("example.org"), 8080, QStringLiteral("Zoe"));
        QVERIFY(quick->toolTip().contains(QLatin1String("Join the first open game")));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("No games yet.\nPress “Quick play” to join or start a standard game, or create your own with “New game…”.")));
    }

    void gamePageWaitsForTheComputerAndOffersARematch()
    {
        GamePage page;
        page.resize(1000, 700);
        page.show();
        QSignalSpy again(&page, &GamePage::playAgainRequested);
        QSignalSpy leaves(&page, &GamePage::leaveRequested);
        QSignalSpy retries(&page, &GamePage::retryComputerRequested);

        const GameInfo info = makeInfo(7, "", 6, 7, 2, 4, 1);
        const Grid empty = GameLogic::makeEmptyBoard(6, 7);
        page.startGame(info, 1, QStringLiteral("Alice"), QStringLiteral("Computer (Easy)"));
        QVERIFY(page.hasComputerOpponent());
        QCOMPARE(page.statusText(), QStringLiteral("Waiting for the computer…"));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("The game starts as soon as the computer has joined: this takes a moment.")));
        QVERIFY(findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));
        QVERIFY(!findButton(&page, QStringLiteral("Play again"))->isVisibleTo(&page));
        QVERIFY(!findButton(&page, QStringLiteral("Retry"))->isVisibleTo(&page));
        QVERIFY(!page.hasComputerProblem());

        // The helper cannot join: the page explains, with the last lines it printed, and offers the ways out.
        page.showComputerProblem(QStringLiteral("The computer player stopped with an error (exit code 1)."),
                                 QStringLiteral("Joining game 7...\nCould not join the game: Game is full"));
        QVERIFY(page.hasComputerProblem());
        QCOMPARE(page.statusText(), QStringLiteral("The computer could not join"));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("The computer player stopped with an error (exit code 1).")));
        auto *output = page.findChild<QPlainTextEdit *>(QStringLiteral("computerOutput"));
        QVERIFY(output != nullptr && output->isVisibleTo(&page));
        QVERIFY(output->toPlainText().contains(QLatin1String("Could not join the game: Game is full")));
        QVERIFY(output->isReadOnly());
        QVERIFY(findButton(&page, QStringLiteral("Retry"))->isVisibleTo(&page));
        QVERIFY(findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
        QVERIFY(!findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));
        QVERIFY(!findButton(&page, QStringLiteral("Play again"))->isVisibleTo(&page));
        QTest::mouseClick(findButton(&page, QStringLiteral("Retry")), Qt::LeftButton);
        QCOMPARE(retries.count(), 1);

        // Without any output there is no empty text box.
        page.showComputerProblem(QStringLiteral("The computer player (connectx-ai) was not found."), QString());
        QVERIFY(!output->isVisibleTo(&page));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("The computer player (connectx-ai) was not found.")));

        // Retrying clears the problem: the game waits for the computer again.
        page.clearComputerProblem();
        QVERIFY(!page.hasComputerProblem());
        QCOMPARE(page.statusText(), QStringLiteral("Waiting for the computer…"));
        QVERIFY(!findButton(&page, QStringLiteral("Retry"))->isVisibleTo(&page));
        QVERIFY(findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));

        // A problem reported after the game has begun does not turn the game page into an error page.
        page.showComputerProblem(QStringLiteral("late"), QString());
        page.applyState(makeState(info, empty, ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Computer (Easy)"}));
        QCOMPARE(page.statusText(), QStringLiteral("Your turn"));
        QVERIFY(!findButton(&page, QStringLiteral("Retry"))->isVisibleTo(&page));
        QVERIFY(findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));
        page.clearComputerProblem();

        // The game is over: "Play again" next to "Back to lobby" (not "Leave game").
        const Grid won = boardFromArt({".......", ".......", ".......", ".......", ".......", "1111222"});
        page.applyState(makeState(info, won, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Computer (Easy)"}, 1));
        QCOMPARE(page.statusText(), QStringLiteral("You win!"));
        QVERIFY(findButton(&page, QStringLiteral("Play again"))->isVisibleTo(&page));
        QVERIFY(findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
        QVERIFY(!findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));
        QTest::mouseClick(findButton(&page, QStringLiteral("Play again")), Qt::LeftButton);
        QCOMPARE(again.count(), 1);
        QCOMPARE(leaves.count(), 0);
        QTest::mouseClick(findButton(&page, QStringLiteral("Back to lobby")), Qt::LeftButton);
        QCOMPARE(leaves.count(), 1);

        // A new game forgets the computer; and a game against people never offers a rematch.
        page.startGame(info, 1, QStringLiteral("Alice"));
        QVERIFY(!page.hasComputerOpponent());
        QCOMPARE(page.statusText(), QStringLiteral("Waiting for players (1/2)…"));
        page.applyState(makeState(info, won, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Bob"}, 1));
        QVERIFY(!findButton(&page, QStringLiteral("Play again"))->isVisibleTo(&page));
        QVERIFY(findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
    }

    // ================================ game page ================================
    void gamePageFollowsAGame()
    {
        GamePage page;
        page.resize(1000, 700);
        page.show();
        QSignalSpy moves(&page, &GamePage::moveRequested);
        QSignalSpy leaves(&page, &GamePage::leaveRequested);

        const GameInfo info = makeInfo(7, "Arena", 6, 7, 2, 4, 1);
        page.startGame(info, 1, QStringLiteral("Alice"));
        QCOMPARE(page.statusText(), QStringLiteral("Waiting for players (1/2)…"));
        QVERIFY(!page.board()->isInputEnabled());
        QVERIFY(!page.needsLeaveConfirmation());
        QVERIFY(findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));
        QVERIFY(!findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("Arena")));

        // The game starts and it is our turn: the first state is shown without animation.
        const Grid empty = GameLogic::makeEmptyBoard(6, 7);
        page.applyState(makeState(info, empty, ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
        QCOMPARE(page.statusText(), QStringLiteral("Your turn"));
        QVERIFY(page.board()->isInputEnabled());
        QVERIFY(page.needsLeaveConfirmation());
        QVERIFY(!page.board()->isAnimating());

        // A click sends one move and locks the board until the answer arrives, even on a double click.
        QTest::mouseClick(page.board(), Qt::LeftButton, Qt::NoModifier, page.board()->columnPoint(2));
        QTest::mouseClick(page.board(), Qt::LeftButton, Qt::NoModifier, page.board()->columnPoint(2));
        QCOMPARE(moves.count(), 1);
        QCOMPARE(moves.last().first().toInt(), 2);
        QVERIFY(!page.board()->isInputEnabled());

        // The server accepted it: the new disc falls, and it is Bob's turn.
        Grid afterMine = empty;
        afterMine[5][2] = 1;
        page.applyState(makeState(info, afterMine, ProtocolGameStatus::IN_PROGRESS, 2, {"Alice", "Bob"}));
        QCOMPARE(page.statusText(), QStringLiteral("Waiting for Bob…"));
        QVERIFY(page.board()->isAnimating());
        QVERIFY(!page.board()->isInputEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!page.board()->isAnimating(), 3000);

        // Bob plays, it is our turn again.
        Grid afterBob = afterMine;
        afterBob[5][3] = 2;
        page.applyState(makeState(info, afterBob, ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
        QCOMPARE(page.statusText(), QStringLiteral("Your turn"));
        QVERIFY(page.board()->isInputEnabled());

        // A refused move gives the turn back, with an explanation.
        QTest::keyClick(page.board(), Qt::Key_1);
        QCOMPARE(moves.count(), 2);
        QVERIFY(!page.board()->isInputEnabled());
        page.showMessage(QStringLiteral("That column is full."));
        QVERIFY(page.board()->isInputEnabled());
        QVERIFY(hasVisibleLabel(&page, QStringLiteral("That column is full.")));

        // Leaving.
        QTest::mouseClick(findButton(&page, QStringLiteral("Leave game")), Qt::LeftButton);
        QCOMPARE(leaves.count(), 1);
    }

    void gamePageToleratesAStateBeforeTheJoin()
    {
        GamePage page;
        page.resize(1000, 700);
        page.show();

        const GameInfo info = makeInfo(8, "", 6, 7, 2, 4, 2);
        Grid board = GameLogic::makeEmptyBoard(6, 7);
        board[5][0] = 2;
        board[5][1] = 1;

        // The broadcast of the game state overtakes the join notification...
        page.applyState(makeState(info, board, ProtocolGameStatus::IN_PROGRESS, 2, {"Alice", "Bob"}));
        // ...and everything is shown properly once the page knows who we are.
        page.startGame(info, 2, QStringLiteral("Bob"));
        QCOMPARE(page.statusText(), QStringLiteral("Your turn"));
        QVERIFY(page.board()->isInputEnabled());
        QCOMPARE(GameLogic::countDiscs(page.board()->board()), 2);
        QVERIFY(!page.board()->isAnimating()); // what was already there is not animated
    }

    void gamePageShowsTheResult()
    {
        const GameInfo info = makeInfo(9, "Arena", 6, 7, 2, 4, 2);
        const Grid win = boardFromArt({".......", ".......", "1......", "1......", "12.....", "12....."});

        {
            GamePage page;
            page.resize(1000, 700);
            page.show();
            QSignalSpy leaves(&page, &GamePage::leaveRequested);
            page.startGame(info, 1, QStringLiteral("Alice"));
            page.applyState(makeState(info, GameLogic::makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
            page.applyState(makeState(info, win, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Bob"}, 1));

            QCOMPARE(page.statusText(), QStringLiteral("You win!"));
            QVERIFY(page.board()->hasWinningLine());
            QVERIFY(!page.board()->isInputEnabled());
            QVERIFY(!page.needsLeaveConfirmation());
            QVERIFY(findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
            QVERIFY(!findButton(&page, QStringLiteral("Leave game"))->isVisibleTo(&page));

            // Losing the connection after the end keeps the result on screen.
            page.setConnectionLost();
            QCOMPARE(page.statusText(), QStringLiteral("You win!"));

            QTest::mouseClick(findButton(&page, QStringLiteral("Back to lobby")), Qt::LeftButton);
            QCOMPARE(leaves.count(), 1);
        }
        {
            GamePage page;
            page.resize(1000, 700);
            page.show();
            page.startGame(info, 2, QStringLiteral("Bob"));
            page.applyState(makeState(info, win, ProtocolGameStatus::FINISHED_WIN, 1, {"Alice", "Bob"}, 1));
            QCOMPARE(page.statusText(), QStringLiteral("Alice wins"));
        }
        {
            GamePage page;
            page.resize(1000, 700);
            page.show();
            page.startGame(info, 2, QStringLiteral("Bob"));
            page.applyState(makeState(info, win, ProtocolGameStatus::FINISHED_DRAW, 1, {"Alice", "Bob"}));
            QCOMPARE(page.statusText(), QStringLiteral("It's a draw"));
            QVERIFY(!page.board()->hasWinningLine());
        }
        {
            // The server went away in the middle of the game.
            GamePage page;
            page.resize(1000, 700);
            page.show();
            page.startGame(info, 1, QStringLiteral("Alice"));
            page.applyState(makeState(info, GameLogic::makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
            QVERIFY(page.board()->isInputEnabled());
            page.setConnectionLost();
            QCOMPARE(page.statusText(), QStringLiteral("Connection lost"));
            QVERIFY(!page.board()->isInputEnabled());
            QVERIFY(findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
        }
        {
            // Everybody else left.
            GamePage page;
            page.resize(1000, 700);
            page.show();
            page.startGame(info, 1, QStringLiteral("Alice"));
            page.applyState(makeState(info, GameLogic::makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice", "Bob"}));
            page.applyState(makeState(info, GameLogic::makeEmptyBoard(6, 7), ProtocolGameStatus::IN_PROGRESS, 1, {"Alice"}));
            QCOMPARE(page.statusText(), QStringLiteral("All other players left"));
            QVERIFY(!page.board()->isInputEnabled());
            QVERIFY(!page.needsLeaveConfirmation());
            QVERIFY(findButton(&page, QStringLiteral("Back to lobby"))->isVisibleTo(&page));
        }
    }
};

int main(int argc, char *argv[])
{
    OffscreenSupport::prepare();
    QApplication app(argc, argv);
    OffscreenSupport::chooseFont();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QTemporaryDir settingsDir;
    OffscreenSupport::useIsolatedSettings(settingsDir.path());

    WidgetsTest test;
    const int result = OffscreenSupport::runTest(test, argc, argv);
    OffscreenSupport::removeIsolatedSettings();
    return result;
}

#include "test_widgets.moc"
