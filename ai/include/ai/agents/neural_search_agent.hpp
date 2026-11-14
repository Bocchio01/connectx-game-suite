#pragma once

#include <memory>
#include <vector>

#include "ai/agent.hpp"
#include "nn/network.hpp"

namespace ai
{
    /**
     * Level 7: classic alpha-beta search whose evaluation function is the *value head* of a neural
     * network, instead of the hand-written "open lines" heuristic of AlphaBetaAgent.
     *
     * This is the bridge between the two halves of the project: the search is the one of the
     * classical agents (negamax, alpha-beta, exact detection of wins and forced moves) and the
     * network only answers the one question search cannot answer by itself: "how good is this
     * position for the player to move?". A depth of 1-3 is already much stronger than the policy
     * head alone, and it is cheap enough to run on a microcontroller (depth 2 on a 6x7 board needs
     * at most 49 network evaluations per move).
     */
    class NeuralSearchAgent : public Agent
    {
    public:
        struct Options
        {
            /** Search depth in plies (1 = pick the move whose resulting position the network likes best). */
            int depth = 3;

            /** Choose uniformly among moves with the same score (otherwise the most central one). */
            bool randomize = false;
        };

        explicit NeuralSearchAgent(std::shared_ptr<const nn::PolicyValueNet> network, uint64_t seed = randomSeed());
        NeuralSearchAgent(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override { rng_.reseed(value); }

    private:
        double negamax(const Position &position, int depth, double alpha, double beta, int ply);
        double networkValue(const Position &position);

        std::shared_ptr<const nn::PolicyValueNet> network_;
        Options options_;
        std::vector<nn::Real> input_;
        std::vector<nn::Real> logits_;
        uint64_t evaluations_;
        Rng rng_;
    };
}
