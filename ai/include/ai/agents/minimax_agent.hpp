#pragma once

#include "ai/agent.hpp"

namespace ai
{
    /**
     * Level 2: plain depth-limited minimax with the heuristic evaluation of
     * ai/evaluation.hpp. No pruning, no tricks: the number of nodes grows as
     * roughly 7^depth, which is exactly why alpha-beta pruning exists.
     *
     * The classic formulation is used: a maximizing and a minimizing branch,
     * scores always measured from the point of view of the player who has to
     * move at the root.
     */
    class MinimaxAgent : public Agent
    {
    public:
        explicit MinimaxAgent(int depth = 4, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override { rng_.reseed(value); }

        /**
         * Minimax value of `position` for the player to move, searching `depth`
         * plies. Exposed for the tests that compare it with alpha-beta.
         */
        int value(const Position &position, int depth);

        uint64_t nodes() const { return nodes_; }

    private:
        int minimax(const Position &position, int depth, int ply, bool maximizing);

        int depth_;
        uint64_t nodes_;
        Rng rng_;
    };
}
