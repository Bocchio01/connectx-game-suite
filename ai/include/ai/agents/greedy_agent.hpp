#pragma once

#include "ai/agent.hpp"

namespace ai
{
    /**
     * Level 1: a rule-based player that only looks one move ahead.
     *
     * In order of priority it
     *   1. wins immediately if it can,
     *   2. blocks an immediate win of the opponent,
     *   3. avoids moves that hand the opponent an immediate win (playing right
     *      under one of his winning cells),
     *   4. otherwise plays the most central column.
     *
     * Ties are broken at random. It is a surprisingly tough opponent for a
     * human beginner and the building block of the "tactical" rollouts of MCTS.
     */
    class GreedyAgent : public Agent
    {
    public:
        explicit GreedyAgent(uint64_t seed = randomSeed()) : rng_(seed) {}

        std::string name() const override { return "greedy"; }
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override { rng_.reseed(value); }

    private:
        /** Among the cells of `candidates`, picks one of the most central columns (random tie-break). */
        uint64_t mostCentral(const Position &position, uint64_t candidates);

        Rng rng_;
    };
}
