#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ai/agents/alphabeta_agent.hpp"
#include "ai/evaluation.hpp"

namespace ai
{
    namespace
    {
        constexpr int kInfinity = 2 * kWinScore;

        // Scores of proven wins/losses are "kWinScore - ply": they depend on the distance from
        // the root, so before caching them they are converted to a distance from the node.
        constexpr int kMateBound = kWinScore - 1000;

        int scoreToTable(int score, int ply)
        {
            if (score > kMateBound)
                return score + ply;
            if (score < -kMateBound)
                return score - ply;
            return score;
        }

        int scoreFromTable(int score, int ply)
        {
            if (score > kMateBound)
                return score - ply;
            if (score < -kMateBound)
                return score + ply;
            return score;
        }

        size_t floorPowerOfTwo(size_t n)
        {
            size_t power = 1;
            while (power * 2 <= n)
                power *= 2;
            return power;
        }
    }

    AlphaBetaAgent::AlphaBetaAgent() : AlphaBetaAgent(Options())
    {
    }

    AlphaBetaAgent::AlphaBetaAgent(const Options &options, uint64_t seed)
        : options_(options),
          iterative_(options.iterative || options.time_limit_ms > 0),
          table_enabled_(options.use_table < 0 ? iterative_ : options.use_table != 0),
          table_mask_(0),
          nodes_(0),
          aborted_(false),
          has_deadline_(false),
          previous_best_(-1),
          proven_(false),
          rng_(seed)
    {
        options_.depth = std::max(1, std::min(options_.depth, 64));

        if (table_enabled_)
        {
            const size_t entries = floorPowerOfTwo(std::max<size_t>(options_.table_entries, 1024));
            table_.assign(entries, Entry());
            table_mask_ = entries - 1;
        }
    }

    std::string AlphaBetaAgent::name() const
    {
        std::string text = "alphabeta(depth=" + std::to_string(options_.depth);
        if (options_.time_limit_ms > 0)
            text += ",time=" + std::to_string(options_.time_limit_ms) + "ms";
        else if (iterative_)
            text += ",iterative";
        if (table_enabled_)
            text += ",tt";
        return text + ")";
    }

    void AlphaBetaAgent::newGame()
    {
        std::fill(table_.begin(), table_.end(), Entry());
        previous_best_ = -1;
    }

    bool AlphaBetaAgent::timeIsUp()
    {
        return has_deadline_ && std::chrono::steady_clock::now() >= deadline_;
    }

    int AlphaBetaAgent::negamax(const Position &position, int depth, int alpha, int beta, int ply)
    {
        ++nodes_;
        if ((nodes_ & 2047) == 0 && timeIsUp())
            aborted_ = true;
        if (aborted_)
            return 0;

        // The player who has just moved has won (only reachable without tactics)
        if (position.lastMoveWon())
            return -(kWinScore - ply);

        if (position.isDraw())
            return 0;

        // Moves that can be searched at this node
        uint64_t candidates = position.legalMoveMask();

        if (options_.tactics)
        {
            // Tactics first: they are exact and cheap.
            // 1. A move that wins immediately ends the search of this node.
            if ((position.currentThreats() & candidates) != 0)
                return kWinScore - (ply + 1);

            // 2. Moves after which the opponent wins at once are never worth searching. If every
            //    move is like that, we lose on the opponent's next move.
            candidates = position.nonLosingMoves();
            if (candidates == 0)
                return -(kWinScore - (ply + 2));
        }

        if (depth == 0)
            return evaluate(position);

        // Transposition table
        int table_move = -1;
        Entry *entry = nullptr;
        const uint64_t key = position.key();
        if (table_enabled_)
        {
            entry = &table_[(key * 0x9E3779B97F4A7C15ULL) >> 20 & table_mask_];
            if (entry->key == key && entry->depth >= 0)
            {
                table_move = entry->move;
                if (entry->depth >= depth)
                {
                    const int stored = scoreFromTable(entry->score, ply);
                    if (entry->bound == EXACT)
                        return stored;
                    if (entry->bound == LOWER)
                        alpha = std::max(alpha, stored);
                    else
                        beta = std::min(beta, stored);
                    if (alpha >= beta)
                        return stored;
                }
            }
        }

        const Geometry &geometry = position.geometry();
        const int alpha_original = alpha;
        int best = -kInfinity;
        int best_move = -1;

        // The table move (if any) is tried first, then the centre columns
        for (int i = -1; i < geometry.cols; ++i)
        {
            const int col = (i < 0) ? table_move : geometry.move_order[i];
            if (col < 0 || (i >= 0 && col == table_move))
                continue;

            const uint64_t cell = position.playableCell(col);
            if ((cell & candidates) == 0)
                continue;

            Position child = position;
            child.playCell(cell);

            const int score = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
            if (aborted_)
                return 0;

            if (score > best)
            {
                best = score;
                best_move = col;
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta)
                break; // beta cutoff: the opponent will never allow this position
        }

        if (entry != nullptr)
        {
            entry->key = key;
            entry->score = scoreToTable(best, ply);
            entry->depth = static_cast<int8_t>(depth);
            entry->move = static_cast<int8_t>(best_move);
            entry->bound = best <= alpha_original ? UPPER : (best >= beta ? LOWER : EXACT);
        }

        return best;
    }

    bool AlphaBetaAgent::searchRoot(const Position &position, int depth, MoveChoice &choice)
    {
        const Geometry &geometry = position.geometry();

        std::vector<double> scores = emptyScores(position);
        std::vector<int> best_columns;
        int best_score = -kInfinity;

        for (int i = -1; i < geometry.cols; ++i)
        {
            // The best move of the previous iteration goes first: the earlier the best move is
            // found, the more the following ones can be pruned
            const int col = (i < 0) ? previous_best_ : geometry.move_order[i];
            if (col < 0 || !position.canPlay(col) || (i >= 0 && col == previous_best_))
                continue;

            Position child = position;
            child.play(col);

            int score;
            if (child.lastMoveWon())
            {
                score = kWinScore - 1;
            }
            else
            {
                // To detect moves that are as good as the best one, the window is opened one
                // point below the best score (otherwise they would only report an upper bound).
                const int alpha = options_.randomize ? best_score - 1 : best_score;
                score = -negamax(child, depth - 1, -kInfinity, -std::max(alpha, -kInfinity), 1);
            }

            if (aborted_)
                return false;

            scores[col] = score;
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

        // `best_columns` is in search order: the first one is the most central / previous best
        const int pick = options_.randomize ? static_cast<int>(rng_.below(static_cast<uint32_t>(best_columns.size()))) : 0;
        choice.column = best_columns[pick];
        choice.scores = std::move(scores);
        choice.depth = depth;

        proven_ = std::abs(best_score) > kMateBound;
        if (proven_)
            choice.value = best_score > 0 ? 1.0 : -1.0;
        else
            choice.value = std::max(-1.0, std::min(1.0, best_score / static_cast<double>(kMaxHeuristic)));

        choice.info = best_score > kMateBound ? "forced win" : (best_score < -kMateBound ? "forced loss" : "heuristic");
        return true;
    }

    MoveChoice AlphaBetaAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();
        nodes_ = 0;
        aborted_ = false;
        has_deadline_ = options_.time_limit_ms > 0;
        if (has_deadline_)
            deadline_ = start + std::chrono::milliseconds(options_.time_limit_ms);

        MoveChoice choice;
        const int first_depth = iterative_ ? 1 : options_.depth;

        for (int depth = first_depth; depth <= options_.depth; ++depth)
        {
            MoveChoice candidate;
            if (!searchRoot(position, depth, candidate))
                break; // time is up: keep the result of the previous depth

            choice = std::move(candidate);
            previous_best_ = choice.column;

            // A forced win/loss is a proof: more depth cannot change it
            if (proven_)
                break;
        }

        if (choice.column < 0)
        {
            // Not even depth 1 completed (absurdly small time limit): play the first safe central move
            const Geometry &geometry = position.geometry();
            for (int i = 0; i < geometry.cols && choice.column < 0; ++i)
            {
                if (position.canPlay(geometry.move_order[i]))
                    choice.column = geometry.move_order[i];
            }
            choice.info = "out of time";
        }

        choice.nodes = nodes_;
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }

    int AlphaBetaAgent::value(const Position &position, int depth)
    {
        nodes_ = 0;
        aborted_ = false;
        has_deadline_ = false;

        const bool saved_table = table_enabled_;
        table_enabled_ = false;

        const Geometry &geometry = position.geometry();
        int best = -kInfinity;
        for (int i = 0; i < geometry.cols; ++i)
        {
            const int col = geometry.move_order[i];
            if (!position.canPlay(col))
                continue;

            Position child = position;
            child.play(col);
            const int score = child.lastMoveWon() ? kWinScore - 1 : -negamax(child, depth - 1, -kInfinity, -best, 1);
            best = std::max(best, score);
        }

        table_enabled_ = saved_table;
        return best;
    }
}
