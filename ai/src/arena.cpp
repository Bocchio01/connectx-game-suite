#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "ai/arena.hpp"
#include "ai/factory.hpp"

namespace ai
{
    GameResult playGame(Agent &first, Agent &second, Position start,
                        double *first_seconds, int *first_moves,
                        double *second_seconds, int *second_moves)
    {
        GameResult result;
        result.moves.clear();

        Position position = start;
        Agent *agents[2] = {&first, &second};
        double *seconds[2] = {first_seconds, second_seconds};
        int *counts[2] = {first_moves, second_moves};
        int turn = 0;

        first.newGame();
        second.newGame();

        while (!position.isTerminal())
        {
            const MoveChoice choice = agents[turn]->chooseMove(position);

            if (!position.canPlay(choice.column))
            {
                // An illegal move forfeits the game
                result.illegal_move = true;
                result.outcome = turn == 0 ? -1 : 1;
                return result;
            }

            if (seconds[turn] != nullptr)
                *seconds[turn] += choice.seconds;
            if (counts[turn] != nullptr)
                ++*counts[turn];

            position.play(choice.column);
            result.moves.push_back(choice.column);
            ++result.plies;

            if (position.lastMoveWon())
            {
                result.outcome = turn == 0 ? 1 : -1;
                return result;
            }

            turn ^= 1;
        }

        result.outcome = 0;
        return result;
    }

    PairResult playMatch(const std::string &spec_a, const std::string &spec_b, const ArenaOptions &options,
                         const std::function<void(int, int)> &progress)
    {
        PairResult pair;
        pair.a = spec_a;
        pair.b = spec_b;

        const int total = std::max(1, options.games);
        const int threads = std::max(1, std::min(options.threads, total));

        std::atomic<int> next_game(0);
        std::atomic<int> finished(0);
        std::mutex mutex;
        double a_seconds = 0, b_seconds = 0;
        int a_moves = 0, b_moves = 0;

        auto worker = [&]()
        {
            // Every thread owns its agents (they are not thread-safe)
            AgentPtr agent_a = createAgent(spec_a);
            AgentPtr agent_b = createAgent(spec_b);

            for (;;)
            {
                const int game = next_game.fetch_add(1);
                if (game >= total)
                    break;

                // Games 2k and 2k+1 share the same opening; colours are swapped between them
                const int opening_id = game / 2;
                const bool a_moves_first = (game % 2 == 0);

                Rng opening_rng(options.seed * 0x9E3779B97F4A7C15ULL + static_cast<uint64_t>(opening_id) * 7919 + 1);
                agent_a->seed(options.seed * 1000003ULL + static_cast<uint64_t>(game) * 2 + 1);
                agent_b->seed(options.seed * 1000003ULL + static_cast<uint64_t>(game) * 2 + 2);

                Position start(options.rows, options.cols, options.connect);
                std::vector<int> opening;
                for (int i = 0; i < options.random_plies && !start.isTerminal(); ++i)
                {
                    const std::vector<int> moves = start.legalMoves();
                    const int col = moves[opening_rng.below(static_cast<uint32_t>(moves.size()))];
                    start.play(col);
                    opening.push_back(col);
                }

                // The agent that moves first from the empty board is `first_agent`; after an odd
                // number of random plies it is the other one's turn.
                Agent *first_agent = a_moves_first ? agent_a.get() : agent_b.get();
                Agent *second_agent = a_moves_first ? agent_b.get() : agent_a.get();
                const bool odd_opening = (opening.size() % 2 == 1);
                Agent *to_move = odd_opening ? second_agent : first_agent;
                Agent *other = odd_opening ? first_agent : second_agent;

                double to_move_seconds = 0, other_seconds = 0;
                int to_move_moves = 0, other_moves = 0;

                GameResult game_result;
                if (start.isTerminal())
                {
                    game_result.outcome = 0;
                }
                else
                {
                    game_result = playGame(*to_move, *other, start, &to_move_seconds, &to_move_moves, &other_seconds, &other_moves);
                }

                // Winner as "A / B / draw"
                int a_result = 0; // +1 A won, -1 B won
                if (game_result.outcome != 0)
                {
                    Agent *winner = game_result.outcome == 1 ? to_move : other;
                    a_result = (winner == agent_a.get()) ? 1 : -1;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (a_result > 0)
                    {
                        ++pair.a_wins;
                        if (a_moves_first)
                            ++pair.a_wins_as_first;
                    }
                    else if (a_result < 0)
                    {
                        ++pair.b_wins;
                        if (!a_moves_first)
                            ++pair.b_wins_as_first;
                    }
                    else
                    {
                        ++pair.draws;
                    }
                    if (game_result.illegal_move)
                        ++pair.illegal_moves;

                    const bool a_is_to_move = (to_move == agent_a.get());
                    a_seconds += a_is_to_move ? to_move_seconds : other_seconds;
                    a_moves += a_is_to_move ? to_move_moves : other_moves;
                    b_seconds += a_is_to_move ? other_seconds : to_move_seconds;
                    b_moves += a_is_to_move ? other_moves : to_move_moves;
                }

                const int done = ++finished;
                if (progress)
                    progress(done, total);
            }
        };

        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i)
            pool.emplace_back(worker);
        for (auto &thread : pool)
            thread.join();

        pair.a_seconds_per_move = a_moves > 0 ? a_seconds / a_moves : 0.0;
        pair.b_seconds_per_move = b_moves > 0 ? b_seconds / b_moves : 0.0;
        return pair;
    }

    std::vector<std::vector<PairResult>> playRoundRobin(const std::vector<std::string> &specs, const ArenaOptions &options,
                                                        const std::function<void(const PairResult &)> &on_pair_done)
    {
        const size_t n = specs.size();
        std::vector<std::vector<PairResult>> results(n, std::vector<PairResult>(n));

        for (size_t i = 0; i < n; ++i)
        {
            for (size_t j = i + 1; j < n; ++j)
            {
                results[i][j] = playMatch(specs[i], specs[j], options);
                if (on_pair_done)
                    on_pair_done(results[i][j]);
            }
        }

        return results;
    }

    std::vector<double> estimateElo(const std::vector<std::string> &specs,
                                    const std::vector<std::vector<PairResult>> &results,
                                    double center)
    {
        const size_t n = specs.size();
        std::vector<double> strength(n, 1.0);

        // Points of i against j, with a pseudo-game (half a point each) to keep the estimate
        // finite when an agent wins or loses every game
        auto points = [&](size_t i, size_t j, double &won, double &played)
        {
            const PairResult &r = i < j ? results[i][j] : results[j][i];
            const double games = r.games();
            const double a_points = r.a_wins + 0.5 * r.draws;
            won = (i < j ? a_points : games - a_points) + 0.5;
            played = games + 1.0;
        };

        // Zermelo / minorisation-maximisation iteration of the Bradley-Terry model
        for (int iteration = 0; iteration < 500; ++iteration)
        {
            std::vector<double> updated(n, 1.0);
            for (size_t i = 0; i < n; ++i)
            {
                double won_total = 0, denominator = 0;
                for (size_t j = 0; j < n; ++j)
                {
                    if (i == j)
                        continue;
                    double won, played;
                    points(i, j, won, played);
                    won_total += won;
                    denominator += played / (strength[i] + strength[j]);
                }
                updated[i] = denominator > 0 ? won_total / denominator : strength[i];
            }

            // Normalise (geometric mean 1)
            double log_sum = 0;
            for (double s : updated)
                log_sum += std::log(s);
            const double scale = std::exp(log_sum / static_cast<double>(n));
            for (size_t i = 0; i < n; ++i)
                strength[i] = updated[i] / scale;
        }

        std::vector<double> elo(n);
        for (size_t i = 0; i < n; ++i)
            elo[i] = center + 400.0 * std::log10(strength[i]);
        return elo;
    }

    std::string formatTournament(const std::vector<std::string> &specs,
                                 const std::vector<std::vector<PairResult>> &results)
    {
        const size_t n = specs.size();
        std::ostringstream out;

        auto cell = [&](size_t i, size_t j) -> double
        {
            const PairResult &r = i < j ? results[i][j] : results[j][i];
            return i < j ? r.aScore() : 1.0 - r.aScore();
        };

        // Standings
        std::vector<double> points(n, 0.0);
        std::vector<int> games(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            for (size_t j = 0; j < n; ++j)
            {
                if (i == j)
                    continue;
                const PairResult &r = i < j ? results[i][j] : results[j][i];
                points[i] += cell(i, j) * r.games();
                games[i] += r.games();
            }
        }

        const std::vector<double> elo = estimateElo(specs, results);

        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
                  { return elo[a] > elo[b]; });

        char line[512];
        out << "Standings (ranked by Elo; 1000 = average of the field)\n";
        std::snprintf(line, sizeof(line), "  %-4s %-44s %7s %9s %6s\n", "rank", "agent", "score", "points", "Elo");
        out << line;
        int rank = 1;
        for (size_t i : order)
        {
            std::snprintf(line, sizeof(line), "  %-4d %-44s %6.1f%% %5.1f/%-4d %6.0f\n", rank++, specs[i].c_str(),
                          games[i] > 0 ? 100.0 * points[i] / games[i] : 0.0, points[i], games[i], elo[i]);
            out << line;
        }

        // Matrix: score of the row agent against the column agent
        out << "\nScore of the row agent against the column agent (win = 1, draw = 0.5)\n";
        std::snprintf(line, sizeof(line), "  %-4s", "");
        out << line;
        for (size_t j = 0; j < n; ++j)
        {
            std::snprintf(line, sizeof(line), " %5s", ("#" + std::to_string(j + 1)).c_str());
            out << line;
        }
        out << "\n";
        for (size_t i = 0; i < n; ++i)
        {
            std::snprintf(line, sizeof(line), "  #%-3d", static_cast<int>(i + 1));
            out << line;
            for (size_t j = 0; j < n; ++j)
            {
                if (i == j)
                {
                    out << "     -";
                    continue;
                }
                std::snprintf(line, sizeof(line), " %5.2f", cell(i, j));
                out << line;
            }
            out << "   " << specs[i] << "\n";
        }

        // Thinking time
        out << "\nAverage thinking time per move\n";
        for (size_t i = 0; i < n; ++i)
        {
            double seconds = 0;
            int count = 0;
            for (size_t j = 0; j < n; ++j)
            {
                if (i == j)
                    continue;
                const PairResult &r = i < j ? results[i][j] : results[j][i];
                const double per_move = i < j ? r.a_seconds_per_move : r.b_seconds_per_move;
                seconds += per_move * r.games();
                count += r.games();
            }
            std::snprintf(line, sizeof(line), "  %-44s %10.3f ms\n", specs[i].c_str(), count > 0 ? 1000.0 * seconds / count : 0.0);
            out << line;
        }

        int illegal = 0;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                illegal += results[i][j].illegal_moves;
        if (illegal > 0)
            out << "\nWARNING: " << illegal << " games ended with an illegal move!\n";

        return out.str();
    }
}
