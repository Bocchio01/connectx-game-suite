#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ai/agent.hpp"

namespace ai
{
    /**
     * Result of a single game between two agents.
     */
    struct GameResult
    {
        /** +1: the agent that moved first from the start position won, -1: the other one, 0: draw. */
        int outcome = 0;

        /** Number of moves played by the agents (random opening moves excluded). */
        int plies = 0;

        /** The whole game as 0-based columns (opening included). */
        std::vector<int> moves;

        /** Set when an agent returned an illegal move (it forfeits the game). */
        bool illegal_move = false;
    };

    /**
     * Plays one game. `first` is the agent that has to move in `start`.
     * Time spent and moves played are accumulated in the optional counters.
     */
    GameResult playGame(Agent &first, Agent &second, Position start,
                        double *first_seconds = nullptr, int *first_moves = nullptr,
                        double *second_seconds = nullptr, int *second_moves = nullptr);

    struct ArenaOptions
    {
        int rows = 6;
        int cols = 7;
        int connect = 4;

        /** Games per pairing. They are played in pairs sharing the same random opening, colours swapped. */
        int games = 100;

        /** Number of games played in parallel (each thread owns its own agents). */
        int threads = 1;

        /** Random moves played before the agents take over, to diversify the games. */
        int random_plies = 2;

        uint64_t seed = 1;
    };

    /** Aggregated result of a pairing: agent A against agent B. */
    struct PairResult
    {
        std::string a;
        std::string b;
        int a_wins = 0;
        int b_wins = 0;
        int draws = 0;
        int a_wins_as_first = 0;  // (of the games in which A moved first from the empty board)
        int b_wins_as_first = 0;
        int illegal_moves = 0;
        double a_seconds_per_move = 0.0;
        double b_seconds_per_move = 0.0;

        int games() const { return a_wins + b_wins + draws; }

        /** Points of A per game: win = 1, draw = 0.5. */
        double aScore() const { return games() == 0 ? 0.5 : (a_wins + 0.5 * draws) / games(); }
    };

    /**
     * Plays `options.games` games between two agents described by their spec
     * (see createAgent). Games are distributed over `options.threads` threads;
     * every thread creates its own pair of agents.
     * @param progress optional callback invoked after every finished game with (done, total)
     */
    PairResult playMatch(const std::string &spec_a, const std::string &spec_b, const ArenaOptions &options,
                         const std::function<void(int, int)> &progress = nullptr);

    /**
     * Round-robin tournament: every agent against every other one.
     * result[i][j] (i < j) is the pairing of specs[i] against specs[j].
     */
    std::vector<std::vector<PairResult>> playRoundRobin(const std::vector<std::string> &specs, const ArenaOptions &options,
                                                        const std::function<void(const PairResult &)> &on_pair_done = nullptr);

    /**
     * Maximum-likelihood Elo ratings (Bradley-Terry model) from a round-robin table.
     * The ratings are centred on `center`.
     */
    std::vector<double> estimateElo(const std::vector<std::string> &specs,
                                    const std::vector<std::vector<PairResult>> &results,
                                    double center = 1000.0);

    /** Human readable standings: score matrix, points and Elo. */
    std::string formatTournament(const std::vector<std::string> &specs,
                                 const std::vector<std::vector<PairResult>> &results);
}
