// Correctness tests of the search based agents: minimax, alpha-beta and the exact solver

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "ai/agents/alphabeta_agent.hpp"
#include "ai/agents/minimax_agent.hpp"
#include "ai/evaluation.hpp"
#include "ai/rng.hpp"
#include "ai/solver.hpp"
#include "test_util.hpp"

using namespace ai;

namespace
{
    /** Exact score by exhaustive memoised search, same convention as Solver (no pruning tricks at all). */
    int bruteForce(const Position &p, std::unordered_map<uint64_t, int> &memo)
    {
        if (p.isDraw())
            return 0;
        if (p.canWinNext())
            return Solver::immediateWinScore(p);

        auto it = memo.find(p.key());
        if (it != memo.end())
            return it->second;

        int best = -1000;
        for (int col = 0; col < p.cols(); ++col)
        {
            if (!p.canPlay(col))
                continue;
            Position child = p;
            child.play(col);
            best = std::max(best, -bruteForce(child, memo));
        }

        memo[p.key()] = best;
        return best;
    }

    /** A random position with `stones` stones in which nobody has won yet (may be shorter if the game ends first). */
    Position randomPosition(int rows, int cols, int connect, int stones, Rng &rng)
    {
        for (;;)
        {
            Position p(rows, cols, connect);
            while (p.moves() < stones && !p.isTerminal())
            {
                const std::vector<int> moves = p.legalMoves();
                p.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }
            if (!p.isTerminal())
                return p;
        }
    }
}

void test_alphabeta_matches_minimax()
{
    Rng rng(2024);

    AlphaBetaAgent::Options options;
    options.tactics = false; // textbook alpha-beta: must return exactly the minimax value
    options.use_table = 0;
    AlphaBetaAgent alphabeta(options, 1);
    MinimaxAgent minimax(1, 1);

    struct Shape
    {
        int rows, cols, connect;
    };
    for (const Shape &s : {Shape{6, 7, 4}, Shape{5, 5, 4}, Shape{4, 5, 3}, Shape{6, 7, 3}})
    {
        for (int round = 0; round < 25; ++round)
        {
            const Position p = randomPosition(s.rows, s.cols, s.connect, static_cast<int>(rng.below(20)), rng);
            for (int depth = 1; depth <= 4; ++depth)
            {
                const int expected = minimax.value(p, depth);
                const uint64_t minimax_nodes = minimax.nodes();
                const int actual = alphabeta.value(p, depth);
                CHECK_MSG(actual == expected, "depth " << depth << " position\n" << p.toString() << "alphabeta " << actual << " minimax " << expected);

                // Pruning must never visit more nodes than the full tree
                CHECK(alphabeta.nodes() <= minimax_nodes);
            }
        }
    }
}

void test_pruning_saves_nodes()
{
    // From the empty 6x7 board, depth 6: alpha-beta needs a small fraction of minimax's nodes
    const Position empty(6, 7, 4);

    MinimaxAgent minimax(6, 1);
    minimax.value(empty, 6);
    const uint64_t full_tree = minimax.nodes();

    AlphaBetaAgent::Options options;
    options.tactics = false;
    options.use_table = 0;
    AlphaBetaAgent alphabeta(options, 1);
    alphabeta.value(empty, 6);

    CHECK(full_tree > 100000);
    CHECK_MSG(alphabeta.nodes() * 4 < full_tree, "alpha-beta " << alphabeta.nodes() << " vs minimax " << full_tree);
}

void test_alphabeta_with_tactics_and_table()
{
    Rng rng(31337);

    // The tactics only prune moves that lose at once and recognise wins: the move chosen must
    // be at least as good as what the textbook search finds at the same depth, and agents must
    // always return legal moves.
    AlphaBetaAgent::Options engine_options;
    engine_options.depth = 6;
    engine_options.iterative = true;
    AlphaBetaAgent engine(engine_options, 5);

    for (int round = 0; round < 30; ++round)
    {
        const Position p = randomPosition(6, 7, 4, static_cast<int>(rng.below(30)), rng);
        const MoveChoice choice = engine.chooseMove(p);
        CHECK(p.canPlay(choice.column));
        CHECK(choice.depth >= 1);
        engine.newGame();
    }

    // x has columns 1-3 of the bottom row, o has the row above: x to move wins in column 4
    const Position can_win = Position::fromMoves(6, 7, 4, "112233");
    // x threatens column 4 on the bottom row and o (to move) cannot win: o must block there
    const Position must_block = Position::fromMoves(6, 7, 4, "17273");
    // o owns columns 1-3 of the second row: if x plays column 4 (bottom) o wins right above it
    const Position poisoned = Position::fromMoves(6, 7, 4, "13217273");

    for (int depth : {1, 2, 3, 4, 6})
    {
        AlphaBetaAgent::Options options;
        options.depth = depth;
        AlphaBetaAgent agent(options, 3);

        CHECK_EQ(agent.chooseMove(can_win).column, 3);
        CHECK_EQ(agent.chooseMove(must_block).column, 3);
        CHECK(agent.chooseMove(poisoned).column != 3);
    }
}

void test_solver_matches_brute_force()
{
    struct Shape
    {
        int rows, cols, connect;
        int min_stones; // random positions start from this many stones (the brute force is exponential)
    };
    Rng rng(555);

    // Small boards are checked from the empty position; the bigger ones only from the middle game on
    for (const Shape &s : {Shape{4, 4, 3, 0}, Shape{4, 4, 4, 0}, Shape{4, 5, 3, 0}, Shape{4, 5, 4, 0},
                           Shape{5, 4, 4, 0}, Shape{5, 5, 3, 9}, Shape{5, 5, 4, 12}, Shape{4, 6, 4, 11}})
    {
        Solver solver(1 << 18);
        std::unordered_map<uint64_t, int> memo;

        std::vector<Position> positions;
        if (s.min_stones == 0)
            positions.emplace_back(s.rows, s.cols, s.connect);

        const int cells = s.rows * s.cols;
        for (int i = 0; i < 80; ++i)
        {
            const int stones = s.min_stones + static_cast<int>(rng.below(static_cast<uint32_t>(cells - 1 - s.min_stones)));
            positions.push_back(randomPosition(s.rows, s.cols, s.connect, stones, rng));
        }

        for (const Position &p : positions)
        {
            const int expected = bruteForce(p, memo);

            int score = 0;
            CHECK(solver.solve(p, score));
            CHECK_MSG(score == expected, s.rows << "x" << s.cols << " k=" << s.connect << "\n" << p.toString() << "solver " << score << " brute " << expected);

            int weak = 0;
            CHECK(solver.solve(p, weak, true));
            CHECK_EQ((weak > 0) - (weak < 0), (expected > 0) - (expected < 0));

            // Per-move scores: the best one is the score of the position
            std::array<int, kMaxCols> scores;
            CHECK(solver.scoreMoves(p, scores));
            int best = Solver::kIllegal;
            for (int col = 0; col < p.cols(); ++col)
            {
                CHECK_EQ(scores[col] == Solver::kIllegal, !p.canPlay(col));
                best = std::max(best, scores[col]);
            }
            CHECK_EQ(best, expected);
        }
    }
}

void test_solver_matches_exhaustive_alphabeta_on_6x7()
{
    // Late-game positions of the real board: the solver must agree with a full-depth search that
    // never uses the heuristic (depth >= remaining moves), also on the *speed* of the win.
    Rng rng(8080);
    Solver solver(1 << 20);

    AlphaBetaAgent::Options options;
    options.tactics = false;
    options.use_table = 0;
    options.randomize = false;
    AlphaBetaAgent exhaustive(options, 1);

    for (int round = 0; round < 12; ++round)
    {
        const Position p = randomPosition(6, 7, 4, 31 + static_cast<int>(rng.below(5)), rng);
        const int remaining = p.geometry().cells - p.moves();

        const int search = exhaustive.value(p, remaining);
        int score = 0;
        CHECK(solver.solve(p, score));

        // search score: +-(kWinScore - ply) where the winning stone is stone number moves + ply
        int expected;
        if (search == 0)
        {
            expected = 0;
        }
        else
        {
            const int ply = kWinScore - std::abs(search);
            const int winning_stone = p.moves() + ply;
            const int magnitude = (p.geometry().cells + 2 - winning_stone) / 2;
            expected = search > 0 ? magnitude : -magnitude;
        }
        CHECK_MSG(score == expected, "solver " << score << " search " << search << "\n" << p.toString());
    }
}

void test_solver_limits_and_table()
{
    Solver solver(1 << 16);
    Rng rng(1);

    // A position with something to search: nobody can win at once and the game is far from over
    Position p = randomPosition(6, 7, 4, 16, rng);
    while (p.canWinNext())
        p = randomPosition(6, 7, 4, 16, rng);

    // A tiny node budget aborts the search...
    solver.setLimits(1500, 0);
    int score = 0;
    CHECK(!solver.solve(p, score));
    CHECK(solver.nodes() >= 1500);

    // ...and removing it lets the very same solver finish (an aborted search must not poison the table)
    solver.setLimits(0, 0);
    CHECK(solver.solve(p, score));

    Solver fresh(1 << 16);
    int expected = 0;
    CHECK(fresh.solve(p, expected));
    CHECK_EQ(score, expected);
}

int main()
{
    RUN_TEST(test_alphabeta_matches_minimax);
    RUN_TEST(test_pruning_saves_nodes);
    RUN_TEST(test_alphabeta_with_tactics_and_table);
    RUN_TEST(test_solver_matches_brute_force);
    RUN_TEST(test_solver_matches_exhaustive_alphabeta_on_6x7);
    RUN_TEST(test_solver_limits_and_table);
    return testSummary("test_search");
}
