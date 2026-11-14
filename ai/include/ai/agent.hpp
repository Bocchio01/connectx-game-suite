#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "ai/bitops.hpp"
#include "ai/position.hpp"
#include "ai/rng.hpp"

namespace ai
{
    /**
     * What an agent answers when asked for a move.
     */
    struct MoveChoice
    {
        /** The chosen column (0-based). Always a legal move. */
        int column = -1;

        /** Estimated outcome for the player to move in [-1, 1] (NaN when the agent has no estimate). */
        double value = std::numeric_limits<double>::quiet_NaN();

        /** Optional per-column preference, higher is better (-infinity for illegal columns). */
        std::vector<double> scores;

        /** Search nodes / simulations / network evaluations spent on this move. */
        uint64_t nodes = 0;

        /** Wall-clock time spent thinking. */
        double seconds = 0.0;

        /** Search depth reached (search-based agents). */
        int depth = 0;

        /** Free-form text for logs. */
        std::string info;
    };

    /**
     * Interface of every game-playing algorithm.
     *
     * Agents are stateless between calls unless documented otherwise (e.g. the
     * transposition table of the search agents), so the same object can be
     * reused for many games. They are NOT thread-safe: use one per thread.
     */
    class Agent
    {
    public:
        virtual ~Agent() = default;

        /** Short human readable description, e.g. "alphabeta(depth=8)". */
        virtual std::string name() const = 0;

        /**
         * Picks a move for the player to move.
         * Precondition: the position is not terminal (there is at least one legal move).
         */
        virtual MoveChoice chooseMove(const Position &position) = 0;

        /** Called before the first move of a new game (drop caches, search trees ...). */
        virtual void newGame() {}

        /** Makes the agent's random choices reproducible. */
        virtual void seed(uint64_t /*seed*/) {}
    };

    using AgentPtr = std::unique_ptr<Agent>;

    // ------------------------------------------------------------------------
    // Helpers shared by several agents
    // ------------------------------------------------------------------------

    /** Column of a one-bit cell mask (as returned by Position::playableCell()). */
    inline int columnOfCell(const Position &position, uint64_t cell)
    {
        return lowestBit(cell) / position.geometry().stride;
    }

    /** Picks, uniformly at random, one of the cells set in `cells` and returns it as a one-bit mask. */
    inline uint64_t randomCell(uint64_t cells, Rng &rng)
    {
        int skip = static_cast<int>(rng.below(static_cast<uint32_t>(popcount(cells))));
        while (skip-- > 0)
            cells = clearLowestBit(cells);
        return cells & (~cells + 1);
    }

    /** Builds a "scores" vector with -infinity in every column. */
    inline std::vector<double> emptyScores(const Position &position)
    {
        return std::vector<double>(static_cast<size_t>(position.cols()), -std::numeric_limits<double>::infinity());
    }
}
