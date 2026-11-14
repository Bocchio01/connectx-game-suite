#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

#include "ai/position.hpp"

namespace ai
{
    /**
     * Exact ("strong") solver: computes the game-theoretic value of a position,
     * assuming both players play perfectly until the end of the game.
     *
     * It is a negamax search with alpha-beta pruning made fast by
     *   - the bitboard Position (a move is a handful of CPU instructions),
     *   - never searching moves that lose immediately (Position::nonLosingMoves),
     *   - move ordering by the number of winning threats a move creates,
     *   - a transposition table,
     *   - an iterative null-window search that narrows [min, max] around the true score
     *     with a few very cheap "is the score greater than m?" questions.
     *
     * Scores follow the convention of Pascal Pons' Connect-4 solver:
     *   score = 0   the game is a draw;
     *   score > 0   the player to move wins; he still holds (score - 1) stones in
     *               his hand when he connects, so larger = faster win;
     *   score < 0   the player to move loses, -score is how quickly (-1 = the
     *               opponent wins with his very last stone).
     * On the empty 6x7 board the score is +1: the first player wins, but only
     * with the last stone he can play.
     *
     * Everything is exact for any supported board and any connect length; the
     * running time grows quickly for early positions of large boards (an empty
     * 6x7 board takes minutes), so the search can be bounded in nodes and time.
     */
    class Solver
    {
    public:
        /** Marker in the arrays returned by scoreMoves() for columns that cannot be played. */
        static constexpr int kIllegal = std::numeric_limits<int>::min();

        /** @param table_entries size of the transposition table (rounded down to a power of two, 16 bytes each) */
        explicit Solver(size_t table_entries = size_t(1) << 22);

        /** Forgets everything stored in the transposition table. */
        void clear();

        /**
         * Bounds the effort of the next solve()/scoreMoves() calls. 0 means unlimited.
         * When a limit is hit the call returns false and its result must be discarded.
         */
        void setLimits(uint64_t max_nodes, int max_milliseconds);

        /**
         * Exact score of a position for the player to move.
         * @param weak only the sign of the score is computed (win / draw / loss): much faster
         * @return false if a limit stopped the search
         * Precondition: the game is not over.
         */
        bool solve(const Position &position, int &score, bool weak = false);

        /**
         * Exact score of every move (kIllegal for full columns), seen from the player to move.
         * @return false if a limit stopped the search
         */
        bool scoreMoves(const Position &position, std::array<int, kMaxCols> &scores, bool weak = false);

        /** Score of a position in which the player to move wins at once. */
        static int immediateWinScore(const Position &position)
        {
            return (position.geometry().cells + 1 - position.moves()) / 2;
        }

        /** Nodes visited by the last public call. */
        uint64_t nodes() const { return nodes_; }

    private:
        enum Bound : uint8_t
        {
            NONE = 0,
            LOWER = 1, // the real score is >= value
            UPPER = 2  // the real score is <= value
        };

        struct Entry
        {
            uint64_t key = 0;
            int8_t value = 0;
            uint8_t bound = NONE;
        };

        bool solveImpl(const Position &position, int &score, bool weak);
        int negamax(const Position &position, int alpha, int beta);
        bool limitsExceeded() const;
        void startClock();

        size_t index(uint64_t key) const
        {
            return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 24) & mask_;
        }

        std::vector<Entry> table_;
        size_t mask_;

        uint64_t nodes_;
        uint64_t max_nodes_;
        int max_milliseconds_;
        bool aborted_;
        std::chrono::steady_clock::time_point deadline_;
    };
}
