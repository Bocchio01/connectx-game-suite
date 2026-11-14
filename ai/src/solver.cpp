#include <algorithm>

#include "ai/solver.hpp"

namespace ai
{
    namespace
    {
        size_t floorPowerOfTwo(size_t n)
        {
            size_t power = 1;
            while (power * 2 <= n)
                power *= 2;
            return power;
        }
    }

    Solver::Solver(size_t table_entries)
        : nodes_(0),
          max_nodes_(0),
          max_milliseconds_(0),
          aborted_(false)
    {
        const size_t entries = floorPowerOfTwo(std::max<size_t>(table_entries, 1024));
        table_.assign(entries, Entry());
        mask_ = entries - 1;
    }

    void Solver::clear()
    {
        std::fill(table_.begin(), table_.end(), Entry());
    }

    void Solver::setLimits(uint64_t max_nodes, int max_milliseconds)
    {
        max_nodes_ = max_nodes;
        max_milliseconds_ = max_milliseconds;
    }

    void Solver::startClock()
    {
        nodes_ = 0;
        aborted_ = false;
        if (max_milliseconds_ > 0)
            deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_milliseconds_);
    }

    bool Solver::limitsExceeded() const
    {
        if (max_nodes_ > 0 && nodes_ >= max_nodes_)
            return true;
        return max_milliseconds_ > 0 && std::chrono::steady_clock::now() >= deadline_;
    }

    bool Solver::solve(const Position &position, int &score, bool weak)
    {
        startClock();
        return solveImpl(position, score, weak);
    }

    bool Solver::scoreMoves(const Position &position, std::array<int, kMaxCols> &scores, bool weak)
    {
        startClock();
        scores.fill(kIllegal);

        for (int col = 0; col < position.cols(); ++col)
        {
            if (!position.canPlay(col))
                continue;

            if (position.isWinningMove(col))
            {
                scores[col] = weak ? 1 : immediateWinScore(position);
                continue;
            }

            Position child = position;
            child.play(col);

            int child_score = 0;
            if (!solveImpl(child, child_score, weak))
                return false;
            scores[col] = -child_score;
        }

        return true;
    }

    bool Solver::solveImpl(const Position &position, int &score, bool weak)
    {
        if (position.isDraw())
        {
            score = 0;
            return true;
        }

        // negamax() does not handle positions in which the player to move wins at once
        if (position.canWinNext())
        {
            score = weak ? 1 : immediateWinScore(position);
            return true;
        }

        const int cells = position.geometry().cells;
        const int moves = position.moves();

        // Bounds of the score: the opponent cannot win right now, we cannot lose before his next move...
        int lower = -(cells - moves) / 2;
        int upper = (cells + 1 - moves) / 2;
        if (weak)
        {
            lower = -1;
            upper = 1;
        }

        // Iteratively narrow the [lower, upper] interval. Each probe is a null-window search
        // (alpha = m, beta = m + 1) that only answers "is the score <= m?", which is cheap.
        // Probing near 0 first is faster: most positions are close to a draw in score terms.
        while (lower < upper)
        {
            int probe = lower + (upper - lower) / 2;
            if (probe <= 0 && lower / 2 < probe)
                probe = lower / 2;
            else if (probe >= 0 && upper / 2 > probe)
                probe = upper / 2;

            const int result = negamax(position, probe, probe + 1);
            if (aborted_)
                return false;

            if (result <= probe)
                upper = result;
            else
                lower = result;
        }

        score = lower;
        return true;
    }

    int Solver::negamax(const Position &position, int alpha, int beta)
    {
        // Precondition: the player to move cannot win immediately (guaranteed by the callers:
        // children are generated only from non-losing moves).
        ++nodes_;
        if ((nodes_ & 0x3FF) == 0 && limitsExceeded())
            aborted_ = true;
        if (aborted_)
            return 0;

        const int cells = position.geometry().cells;
        const int moves = position.moves();

        // Moves that do not let the opponent win at once. None: we lose on his next move.
        const uint64_t candidates = position.nonLosingMoves();
        if (candidates == 0)
            return -(cells - moves) / 2;

        // With at most two cells left and no immediate win for anybody, the game is drawn
        if (moves >= cells - 2)
            return 0;

        // Tighter bounds of the score: the opponent cannot win next move, and we cannot win now
        const int lower = -(cells - 2 - moves) / 2;
        if (alpha < lower)
        {
            alpha = lower;
            if (alpha >= beta)
                return alpha;
        }

        const int upper = (cells - 1 - moves) / 2;
        if (beta > upper)
        {
            beta = upper;
            if (alpha >= beta)
                return beta;
        }

        // Transposition table
        const uint64_t key = position.key();
        Entry &entry = table_[index(key)];
        if (entry.key == key && entry.bound != NONE)
        {
            if (entry.bound == LOWER)
            {
                if (alpha < entry.value)
                {
                    alpha = entry.value;
                    if (alpha >= beta)
                        return alpha;
                }
            }
            else if (beta > entry.value)
            {
                beta = entry.value;
                if (alpha >= beta)
                    return beta;
            }
        }

        // Move ordering: the more winning threats a move creates, the earlier it is tried.
        // Ties keep the centre-first order of the columns.
        struct Candidate
        {
            uint64_t cell;
            int threats;
        };
        Candidate ordered[kMaxCols];
        int count = 0;

        const Geometry &geometry = position.geometry();
        for (int i = 0; i < geometry.cols; ++i)
        {
            const uint64_t cell = position.playableCell(geometry.move_order[i]);
            if ((cell & candidates) == 0)
                continue;

            const int threats = popcount(position.winningCells(position.current() | cell, position.mask() | cell));

            // insertion sort, stable, descending threats
            int j = count++;
            while (j > 0 && ordered[j - 1].threats < threats)
            {
                ordered[j] = ordered[j - 1];
                --j;
            }
            ordered[j] = {cell, threats};
        }

        for (int i = 0; i < count; ++i)
        {
            Position child = position;
            child.playCell(ordered[i].cell);

            const int score = -negamax(child, -beta, -alpha);
            if (aborted_)
                return 0;

            if (score >= beta)
            {
                // fail high: the real score is at least `score`
                entry.key = key;
                entry.value = static_cast<int8_t>(score);
                entry.bound = LOWER;
                return score;
            }

            if (score > alpha)
                alpha = score;
        }

        // fail low: no move did better than `alpha`
        entry.key = key;
        entry.value = static_cast<int8_t>(alpha);
        entry.bound = UPPER;
        return alpha;
    }
}
