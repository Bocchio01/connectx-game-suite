// Tests of the bitboard Position against the reference engine in core/

#include <map>
#include <random>
#include <utility>
#include <vector>

#include "core/game/engine.hpp"
#include "ai/position.hpp"
#include "ai/rng.hpp"
#include "test_util.hpp"

using namespace ai;

namespace
{
    struct Shape
    {
        int rows, cols, connect;
    };

    const std::vector<Shape> kShapes = {
        {6, 7, 4}, {4, 4, 3}, {4, 4, 4}, {5, 5, 4}, {6, 7, 3}, {7, 7, 5},
        {4, 12, 4}, {7, 8, 4}, {5, 9, 4}, {6, 6, 4}, {5, 7, 5}, {4, 5, 3}};

    /** Compares every observable property of Position and of the reference engine. */
    void compareWithEngine(const Position &position, const GameEngine &engine)
    {
        const GameState &state = engine.getState();
        const Board &board = state.getBoard();
        const uint8_t current = state.getCurrentPlayer();

        // Board content, from the point of view of the player to move
        for (int row = 0; row < position.rows(); ++row)
        {
            for (int col = 0; col < position.cols(); ++col)
            {
                const uint8_t value = board.get(row, col);
                int expected = 0;
                if (value != 0)
                    expected = (value == current) ? 1 : 2;

                // (when the game is over the engine does not advance the player: skip the relative check)
                if (!engine.isGameOver())
                    CHECK_EQ(position.cell(row, col), expected);
            }
        }

        // Legal moves
        for (int col = 0; col < position.cols(); ++col)
            CHECK_EQ(position.canPlay(col), engine.getRules().isValidMove(board, col));

        // Game result
        CHECK_EQ(position.isTerminal(), engine.isGameOver());
        CHECK_EQ(position.lastMoveWon(), state.getStatus() == GameStatus::FINISHED_WIN);
        CHECK_EQ(position.isDraw(), state.getStatus() == GameStatus::FINISHED_DRAW);
    }
}

void test_geometry()
{
    std::string reason;
    CHECK(Geometry::isSupported(6, 7, 4));
    CHECK(Geometry::isSupported(7, 8, 4)); // 8 * 8 = 64 bits: the largest possible
    CHECK(!Geometry::isSupported(8, 8, 4, &reason));
    CHECK(!reason.empty());
    CHECK(!Geometry::isSupported(3, 7, 3));
    CHECK(!Geometry::isSupported(6, 7, 2));
    CHECK(!Geometry::isSupported(6, 7, 8)); // longer than the board
    CHECK(!Geometry::isSupported(20, 20, 4));

    const Geometry &g = Geometry::get(6, 7, 4);
    CHECK_EQ(g.stride, 7);
    CHECK_EQ(g.cells, 42);
    CHECK_EQ(popcount(g.board_mask), 42);
    CHECK_EQ(popcount(g.bottom_mask), 7);
    CHECK_EQ(g.windows.size(), static_cast<size_t>(69)); // 24 horizontal + 21 vertical + 2 * 12 diagonal
    CHECK_EQ(g.move_order[0], 3);
    CHECK_EQ(g.move_order[1], 2);
    CHECK_EQ(g.move_order[2], 4);
    CHECK(&g == &Geometry::get(6, 7, 4)); // cached

    // The move order is a permutation of the columns, for every width
    for (const Shape &s : kShapes)
    {
        const Geometry &geo = Geometry::get(s.rows, s.cols, s.connect);
        std::vector<int> seen(geo.cols, 0);
        for (int i = 0; i < geo.cols; ++i)
            ++seen[geo.move_order[i]];
        for (int col = 0; col < geo.cols; ++col)
            CHECK_EQ(seen[col], 1);
    }
}

void test_basic_moves_and_wins()
{
    Position p(6, 7, 4);
    CHECK_EQ(p.moves(), 0);
    CHECK_EQ(p.legalMoveCount(), 7);
    CHECK(!p.isTerminal());

    // Vertical win: x plays column 1 four times, o plays column 2
    Position vertical = Position::fromMoves(6, 7, 4, "1212121");
    CHECK(vertical.lastMoveWon());
    CHECK(vertical.isTerminal());
    CHECK(!vertical.isDraw());
    CHECK(!Position::fromMoves(6, 7, 4, "121212").isTerminal());

    // Horizontal win on the bottom row: x has columns 1-4
    CHECK(Position::fromMoves(6, 7, 4, "1122334").lastMoveWon());

    // Diagonal "/" win for x on (1,0) (2,1) (3,2) (4,3) -- the last move closes it
    const Position before_diagonal = Position::fromMoves(6, 7, 4, "1223433414");
    CHECK(!before_diagonal.isTerminal());
    CHECK(before_diagonal.canWinNext());
    CHECK(before_diagonal.isWinningMove(3));
    CHECK(!before_diagonal.isWinningMove(2));
    CHECK(Position::fromMoves(6, 7, 4, "12234334144").lastMoveWon());

    // The mirror image is a diagonal "\" win
    CHECK(Position::fromMoves(6, 7, 4, "76654554744").lastMoveWon());

    // A full column is no longer playable, and the other six still are
    Position stacked = Position::fromMoves(6, 7, 4, "111111");
    CHECK(!stacked.isTerminal());
    CHECK(!stacked.canPlay(0));
    CHECK_EQ(stacked.legalMoveCount(), 6);
    CHECK_EQ(stacked.playableCell(0), static_cast<uint64_t>(0));
    CHECK(!stacked.canPlay(-1));
    CHECK(!stacked.canPlay(7));

    // Connect-3 on a 4x4 board is much easier
    CHECK(Position::fromMoves(4, 4, 3, "11223").lastMoveWon());
    CHECK(!Position::fromMoves(4, 4, 4, "11223").isTerminal());

    // The side to move alternates and the stones are seen from the right side
    Position p2(6, 7, 4);
    p2.play(3); // first player
    CHECK_EQ(p2.cell(5, 3), 2); // seen from the second player the stone is the opponent's
    p2.play(2);
    CHECK_EQ(p2.cell(5, 3), 1);
    CHECK_EQ(p2.cell(5, 2), 2);

    bool threw = false;
    try
    {
        Position::fromMoves(6, 7, 4, "9");
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    CHECK(threw);

    // Moves after the end are rejected
    threw = false;
    try
    {
        Position::fromMoves(6, 7, 4, "12121212");
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    CHECK(threw);
}

void test_random_games_match_reference_engine()
{
    Rng rng(12345);

    for (const Shape &shape : kShapes)
    {
        for (int game = 0; game < 150; ++game)
        {
            GameEngine engine(shape.rows, shape.cols, 2, shape.connect);
            engine.startGame();
            Position position(shape.rows, shape.cols, shape.connect);
            compareWithEngine(position, engine);

            while (!engine.isGameOver())
            {
                const std::vector<int> moves = position.legalMoves();
                CHECK(!moves.empty());
                const int col = moves[rng.below(static_cast<uint32_t>(moves.size()))];

                // The tactical helper must predict the outcome of the move
                const bool predicted_win = position.isWinningMove(col);

                CHECK(engine.makeMove(static_cast<uint8_t>(col)));
                const bool won = position.play(col);

                CHECK_EQ(predicted_win, won);
                compareWithEngine(position, engine);
            }
        }
    }
}

void test_winning_cells_and_non_losing_moves()
{
    Rng rng(777);

    for (const Shape &shape : kShapes)
    {
        for (int game = 0; game < 60; ++game)
        {
            Position position(shape.rows, shape.cols, shape.connect);

            while (!position.isTerminal())
            {
                const Geometry &geo = position.geometry();
                const uint64_t empty = geo.board_mask & ~position.mask();

                // winningCells == every empty cell that completes a line (brute force)
                uint64_t expected_mine = 0;
                uint64_t expected_theirs = 0;
                for (uint64_t cells = empty; cells != 0; cells = clearLowestBit(cells))
                {
                    const uint64_t bit = cells & (~cells + 1);
                    if (position.hasConnect(position.current() | bit))
                        expected_mine |= bit;
                    if (position.hasConnect(position.opponent() | bit))
                        expected_theirs |= bit;
                }
                CHECK_EQ(position.currentThreats(), expected_mine);
                CHECK_EQ(position.opponentThreats(), expected_theirs);
                CHECK_EQ(position.canWinNext(), (expected_mine & position.legalMoveMask()) != 0);

                // nonLosingMoves == moves after which the opponent cannot win at once
                if (!position.canWinNext())
                {
                    uint64_t expected_safe = 0;
                    for (int col = 0; col < position.cols(); ++col)
                    {
                        if (!position.canPlay(col))
                            continue;
                        Position child = position;
                        child.play(col);
                        if (!child.canWinNext())
                            expected_safe |= position.playableCell(col);
                    }
                    CHECK_EQ(position.nonLosingMoves(), expected_safe);
                }

                const std::vector<int> moves = position.legalMoves();
                position.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }
        }
    }
}

void test_mirror_encode_and_key()
{
    Rng rng(99);
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> seen;

    for (const Shape &shape : kShapes)
    {
        seen.clear();
        for (int game = 0; game < 80; ++game)
        {
            Position position(shape.rows, shape.cols, shape.connect);
            while (!position.isTerminal())
            {
                const Position mirrored = position.mirrored();
                CHECK(mirrored.mirrored() == position);
                CHECK_EQ(mirrored.moves(), position.moves());
                CHECK_EQ(mirrored.lastMoveWon(), position.lastMoveWon());
                for (int row = 0; row < position.rows(); ++row)
                    for (int col = 0; col < position.cols(); ++col)
                        CHECK_EQ(mirrored.cell(row, col), position.cell(row, position.cols() - 1 - col));

                // Encoding: two planes, mine first
                std::vector<float> encoded(position.encodedSize());
                position.encode(encoded.data());
                int mine = 0, theirs = 0;
                for (int row = 0; row < position.rows(); ++row)
                {
                    for (int col = 0; col < position.cols(); ++col)
                    {
                        const float *cell_ptr = &encoded[(row * position.cols() + col) * 2];
                        CHECK((cell_ptr[0] == 1.0f) == (position.cell(row, col) == 1));
                        CHECK((cell_ptr[1] == 1.0f) == (position.cell(row, col) == 2));
                        mine += cell_ptr[0] == 1.0f;
                        theirs += cell_ptr[1] == 1.0f;
                    }
                }
                CHECK_EQ(mine + theirs, position.moves());

                // Keys identify positions uniquely
                auto it = seen.find(position.key());
                if (it == seen.end())
                    seen[position.key()] = {position.current(), position.mask()};
                else
                    CHECK(it->second.first == position.current() && it->second.second == position.mask());

                const std::vector<int> moves = position.legalMoves();
                position.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }
        }
    }
}

void test_from_grid()
{
    Rng rng(4242);

    for (const Shape &shape : kShapes)
    {
        for (int game = 0; game < 40; ++game)
        {
            Position position(shape.rows, shape.cols, shape.connect);
            const int stop_after = static_cast<int>(rng.below(static_cast<uint32_t>(shape.rows * shape.cols)));

            while (!position.isTerminal() && position.moves() < stop_after)
            {
                const std::vector<int> moves = position.legalMoves();
                position.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }

            // Server-style grid: row 0 on top, ids 1/2, with the second player as "current"
            const uint8_t current_id = 2, opponent_id = 1;
            std::vector<std::vector<uint8_t>> grid(shape.rows, std::vector<uint8_t>(shape.cols, 0));
            for (int row = 0; row < shape.rows; ++row)
            {
                for (int col = 0; col < shape.cols; ++col)
                {
                    const int value = position.cell(row, col);
                    grid[row][col] = value == 0 ? 0 : (value == 1 ? current_id : opponent_id);
                }
            }

            const Position rebuilt = Position::fromGrid(grid, current_id, opponent_id, shape.connect);
            CHECK(rebuilt == position);
            CHECK_EQ(rebuilt.lastMoveWon(), position.lastMoveWon());
        }
    }

    // Floating stones and unknown ids are rejected
    std::vector<std::vector<uint8_t>> floating(6, std::vector<uint8_t>(7, 0));
    floating[2][3] = 1;
    bool threw = false;
    try
    {
        Position::fromGrid(floating, 1, 2, 4);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    CHECK(threw);

    std::vector<std::vector<uint8_t>> unknown(6, std::vector<uint8_t>(7, 0));
    unknown[5][3] = 7;
    threw = false;
    try
    {
        Position::fromGrid(unknown, 1, 2, 4);
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    CHECK(threw);
}

int main()
{
    RUN_TEST(test_geometry);
    RUN_TEST(test_basic_moves_and_wins);
    RUN_TEST(test_random_games_match_reference_engine);
    RUN_TEST(test_winning_cells_and_non_losing_moves);
    RUN_TEST(test_mirror_encode_and_key);
    RUN_TEST(test_from_grid);
    return testSummary("test_position");
}
