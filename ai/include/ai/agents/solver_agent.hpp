#pragma once

#include "ai/agent.hpp"
#include "ai/agents/alphabeta_agent.hpp"
#include "ai/solver.hpp"

namespace ai
{
    /**
     * Level 4: perfect play. Uses the exact Solver to score every move and plays
     * the best one (the fastest win, or the slowest defeat).
     *
     * Solving early positions of a 6x7 board can take minutes, so a time limit
     * can be set: when the exact search does not finish in time the agent
     * falls back to an iterative-deepening alpha-beta search.
     * The transposition table is kept between moves and games (a position
     * solved once stays solved).
     */
    class SolverAgent : public Agent
    {
    public:
        struct Options
        {
            /** Time budget per move in milliseconds (0 = no limit: always play perfectly). */
            int time_limit_ms = 0;

            /** Only distinguish win / draw / loss: much faster, but the winner may not choose the fastest win. */
            bool weak = false;

            /** Transposition table size. */
            size_t table_entries = size_t(1) << 22;

            /** Depth of the fallback alpha-beta search. */
            int fallback_depth = 14;
        };

        SolverAgent();
        explicit SolverAgent(const Options &options, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override;

        /** Access to the solver (the tools use it to label training data). */
        Solver &solver() { return solver_; }

    private:
        Options options_;
        Solver solver_;
        AlphaBetaAgent fallback_;
        Rng rng_;
    };
}
