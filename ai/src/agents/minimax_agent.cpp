#include <algorithm>
#include <chrono>

#include "ai/agents/minimax_agent.hpp"
#include "ai/evaluation.hpp"

namespace ai
{
    MinimaxAgent::MinimaxAgent(int depth, uint64_t seed)
        : depth_(std::max(1, depth)),
          nodes_(0),
          rng_(seed)
    {
    }

    std::string MinimaxAgent::name() const
    {
        return "minimax(depth=" + std::to_string(depth_) + ")";
    }

    int MinimaxAgent::minimax(const Position &position, int depth, int ply, bool maximizing)
    {
        ++nodes_;

        // `maximizing` tells whether the root player is the one to move here.
        // If the game is already decided, the player who just moved has won.
        if (position.lastMoveWon())
            return maximizing ? -(kWinScore - ply) : (kWinScore - ply);

        if (position.isDraw())
            return 0;

        if (depth == 0)
        {
            const int score = evaluate(position); // from the side to move: flip for the minimizer
            return maximizing ? score : -score;
        }

        const Geometry &geometry = position.geometry();

        if (maximizing)
        {
            int best = -2 * kWinScore;
            for (int i = 0; i < geometry.cols; ++i)
            {
                const int col = geometry.move_order[i];
                if (!position.canPlay(col))
                    continue;

                Position child = position;
                child.play(col);
                best = std::max(best, minimax(child, depth - 1, ply + 1, false));
            }
            return best;
        }

        int best = 2 * kWinScore;
        for (int i = 0; i < geometry.cols; ++i)
        {
            const int col = geometry.move_order[i];
            if (!position.canPlay(col))
                continue;

            Position child = position;
            child.play(col);
            best = std::min(best, minimax(child, depth - 1, ply + 1, true));
        }
        return best;
    }

    MoveChoice MinimaxAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();
        nodes_ = 0;

        const Geometry &geometry = position.geometry();
        MoveChoice choice;
        choice.scores = emptyScores(position);

        int best_score = -2 * kWinScore;
        std::vector<int> best_columns;

        for (int i = 0; i < geometry.cols; ++i)
        {
            const int col = geometry.move_order[i];
            if (!position.canPlay(col))
                continue;

            Position child = position;
            child.play(col);
            const int score = minimax(child, depth_ - 1, 1, false);
            choice.scores[col] = score;

            if (score > best_score)
            {
                best_score = score;
                best_columns.assign(1, col);
            }
            else if (score == best_score)
            {
                best_columns.push_back(col);
            }
        }

        choice.column = best_columns[rng_.below(static_cast<uint32_t>(best_columns.size()))];
        choice.nodes = nodes_;
        choice.depth = depth_;
        choice.value = std::max(-1.0, std::min(1.0, best_score / static_cast<double>(kMaxHeuristic)));
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }

    int MinimaxAgent::value(const Position &position, int depth)
    {
        nodes_ = 0;
        const Geometry &geometry = position.geometry();

        int best = -2 * kWinScore;
        for (int i = 0; i < geometry.cols; ++i)
        {
            const int col = geometry.move_order[i];
            if (!position.canPlay(col))
                continue;

            Position child = position;
            child.play(col);
            best = std::max(best, minimax(child, depth - 1, 1, false));
        }
        return best;
    }
}
