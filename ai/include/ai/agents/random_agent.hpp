#pragma once

#include "ai/agent.hpp"

namespace ai
{
    /**
     * Level 0: plays a uniformly random legal move.
     * The baseline every other agent must beat.
     */
    class RandomAgent : public Agent
    {
    public:
        explicit RandomAgent(uint64_t seed = randomSeed()) : rng_(seed) {}

        std::string name() const override { return "random"; }
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override { rng_.reseed(value); }

    private:
        Rng rng_;
    };
}
