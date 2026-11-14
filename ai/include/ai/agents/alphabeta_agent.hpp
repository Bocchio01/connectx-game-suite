#pragma once

#include <chrono>
#include <vector>

#include "ai/agent.hpp"

namespace ai
{
    /**
     * Level 3: negamax with alpha-beta pruning.
     *
     * Same minimax value as MinimaxAgent (the tests check it) but it visits a
     * tiny fraction of the nodes thanks to
     *   - alpha-beta pruning,
     *   - move ordering (centre columns first, then the best move found so far),
     *   - tactical shortcuts: an immediate win ends the search of a node and
     *     moves that let the opponent win at once are never searched.
     *
     * Optional "engine" features, enabled with `time_limit_ms` or `iterative`:
     *   - iterative deepening: depth 1, 2, 3 ... until the depth or the time
     *     limit is reached, always keeping the answer of the last completed
     *     depth; the previous iteration provides the move ordering;
     *   - a transposition table caching the result of already searched
     *     positions (the same position is reached by many move orders).
     */
    class AlphaBetaAgent : public Agent
    {
    public:
        struct Options
        {
            /** Maximum search depth in plies. */
            int depth = 8;

            /** If > 0: iterative deepening, stop when this many milliseconds have elapsed. */
            int time_limit_ms = 0;

            /** Iterative deepening even without a time limit. */
            bool iterative = false;

            /** Transposition table (default: on when iterative deepening is on). */
            int use_table = -1;

            /** Number of transposition table entries (rounded down to a power of two). */
            size_t table_entries = size_t(1) << 20;

            /** Choose uniformly among moves with the same score (otherwise the most central one). */
            bool randomize = true;

            /**
             * Exact tactical shortcuts: stop at once when the side to move can win, never search
             * moves that let the opponent win immediately. Turn off to get the textbook
             * algorithm, whose value is identical to plain minimax (the tests rely on that).
             */
            bool tactics = true;
        };

        /** Default options (a nested struct cannot be used as a default argument inside its own class). */
        AlphaBetaAgent();
        explicit AlphaBetaAgent(const Options &options, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void newGame() override;
        void seed(uint64_t value) override { rng_.reseed(value); }

        /**
         * Fixed-depth alpha-beta value of `position` for the player to move
         * (no iterative deepening, no table). Exposed for the tests.
         */
        int value(const Position &position, int depth);

        uint64_t nodes() const { return nodes_; }

    private:
        enum Bound : uint8_t
        {
            EXACT = 0,
            LOWER = 1, // the real score is >= the stored one (a beta cutoff happened)
            UPPER = 2  // the real score is <= the stored one (no move improved alpha)
        };

        struct Entry
        {
            uint64_t key = 0;
            int32_t score = 0;
            int8_t depth = -1;
            int8_t move = -1;
            uint8_t bound = EXACT;
        };

        int negamax(const Position &position, int depth, int alpha, int beta, int ply);
        bool searchRoot(const Position &position, int depth, MoveChoice &choice);
        bool timeIsUp();

        Options options_;
        bool iterative_;
        bool table_enabled_;

        std::vector<Entry> table_;
        size_t table_mask_;

        uint64_t nodes_;
        bool aborted_;
        std::chrono::steady_clock::time_point deadline_;
        bool has_deadline_;
        int previous_best_;
        bool proven_; // the last root search found a forced win/loss
        Rng rng_;
    };
}
