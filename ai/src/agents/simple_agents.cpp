#include <cstdlib>

#include "ai/agents/greedy_agent.hpp"
#include "ai/agents/random_agent.hpp"

namespace ai
{
    // ========================================================================
    // RandomAgent
    // ========================================================================

    MoveChoice RandomAgent::chooseMove(const Position &position)
    {
        MoveChoice choice;
        choice.column = columnOfCell(position, randomCell(position.legalMoveMask(), rng_));
        choice.info = "random";
        return choice;
    }

    // ========================================================================
    // GreedyAgent
    // ========================================================================

    uint64_t GreedyAgent::mostCentral(const Position &position, uint64_t candidates)
    {
        const Geometry &geometry = position.geometry();

        // Distance from the centre, doubled so that even-width boards stay integral
        auto distance = [&](uint64_t cell)
        {
            const int col = columnOfCell(position, cell);
            return std::abs(2 * col - (geometry.cols - 1));
        };

        int best_distance = 1 << 30;
        uint64_t best_cells = 0;
        for (uint64_t rest = candidates; rest != 0; rest = clearLowestBit(rest))
        {
            const uint64_t cell = rest & (~rest + 1);
            const int d = distance(cell);
            if (d < best_distance)
            {
                best_distance = d;
                best_cells = cell;
            }
            else if (d == best_distance)
            {
                best_cells |= cell;
            }
        }

        return randomCell(best_cells, rng_);
    }

    MoveChoice GreedyAgent::chooseMove(const Position &position)
    {
        const uint64_t legal = position.legalMoveMask();
        MoveChoice choice;

        // 1. Win now
        const uint64_t wins = position.currentThreats() & legal;
        if (wins != 0)
        {
            choice.column = columnOfCell(position, mostCentral(position, wins));
            choice.value = 1.0;
            choice.info = "immediate win";
            return choice;
        }

        // 2.+3. Block, and never play under an opponent's winning cell
        const uint64_t safe = position.nonLosingMoves();
        if (safe != 0)
        {
            choice.column = columnOfCell(position, mostCentral(position, safe));
            choice.info = "safe move";
            return choice;
        }

        // Every move loses: play the most central one anyway
        choice.column = columnOfCell(position, mostCentral(position, legal));
        choice.value = -1.0;
        choice.info = "lost anyway";
        return choice;
    }
}
