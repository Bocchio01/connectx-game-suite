#include <algorithm>
#include <chrono>

#include "ai/agents/solver_agent.hpp"

namespace ai
{
    namespace
    {
        AlphaBetaAgent::Options fallbackOptions(const SolverAgent::Options &options)
        {
            AlphaBetaAgent::Options fallback;
            fallback.depth = options.fallback_depth;
            fallback.iterative = true;
            fallback.time_limit_ms = options.time_limit_ms > 0 ? std::max(50, options.time_limit_ms / 3) : 500;
            return fallback;
        }
    }

    SolverAgent::SolverAgent() : SolverAgent(Options())
    {
    }

    SolverAgent::SolverAgent(const Options &options, uint64_t seed)
        : options_(options),
          solver_(options.table_entries),
          fallback_(fallbackOptions(options), seed ^ 0x5DEECE66DULL),
          rng_(seed)
    {
    }

    std::string SolverAgent::name() const
    {
        std::string text = "solver(";
        text += options_.weak ? "weak" : "exact";
        if (options_.time_limit_ms > 0)
            text += ",time=" + std::to_string(options_.time_limit_ms) + "ms";
        return text + ")";
    }

    void SolverAgent::seed(uint64_t value)
    {
        rng_.reseed(value);
        fallback_.seed(value ^ 0x5DEECE66DULL);
    }

    MoveChoice SolverAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();

        // Two thirds of the budget go to the exact search, the rest is kept for the fallback
        solver_.setLimits(0, options_.time_limit_ms > 0 ? options_.time_limit_ms * 2 / 3 : 0);

        std::array<int, kMaxCols> scores;
        MoveChoice choice;

        if (!solver_.scoreMoves(position, scores, options_.weak))
        {
            choice = fallback_.chooseMove(position);
            choice.info = "fallback (" + choice.info + ")";
            choice.nodes += solver_.nodes();
            choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return choice;
        }

        int best_score = Solver::kIllegal;
        for (int col = 0; col < position.cols(); ++col)
            best_score = std::max(best_score, scores[col]);

        // Among the best moves, pick one at random (they have the same game-theoretic value)
        std::vector<int> best_columns;
        for (int i = 0; i < position.cols(); ++i)
        {
            const int col = position.geometry().move_order[i];
            if (scores[col] == best_score)
                best_columns.push_back(col);
        }

        choice.column = best_columns[rng_.below(static_cast<uint32_t>(best_columns.size()))];
        choice.scores = emptyScores(position);
        for (int col = 0; col < position.cols(); ++col)
        {
            if (scores[col] != Solver::kIllegal)
                choice.scores[col] = scores[col];
        }

        choice.value = best_score > 0 ? 1.0 : (best_score < 0 ? -1.0 : 0.0);
        choice.nodes = solver_.nodes();
        choice.info = best_score > 0 ? "exact: win" : (best_score < 0 ? "exact: loss" : "exact: draw");
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }
}
