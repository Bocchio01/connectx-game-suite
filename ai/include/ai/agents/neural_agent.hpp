#pragma once

#include <memory>
#include <vector>

#include "ai/agent.hpp"
#include "nn/network.hpp"

namespace ai
{
    /**
     * Throws std::invalid_argument if the network was not built for the board of `position`
     * (input size, number of columns and, when recorded in the model, rows and connect length).
     */
    void checkNetworkMatches(const nn::NetworkConfig &config, const Position &position);

    /**
     * Level 6: the policy network plays alone, with no search at all.
     *
     * The position is encoded as two planes (my stones / the opponent's stones), the
     * network produces one score per column, illegal columns are masked out and the
     * best one is played. A move costs a single forward pass (microseconds): this is
     * also exactly what the Arduino sketch does with the TensorFlow Lite version of the model.
     *
     * Optional extras:
     *   - temperature > 0 samples the move from softmax(score / temperature) instead of taking the best;
     *   - mirror averages the answer with the one for the left-right mirrored board
     *     (Connect-X is symmetric, so both must agree: the average cancels some noise);
     *   - tactics plays an immediate win / blocks an immediate loss without asking the network.
     */
    class NeuralAgent : public Agent
    {
    public:
        struct Options
        {
            double temperature = 0.0;
            bool mirror = false;
            bool tactics = false;
        };

        explicit NeuralAgent(std::shared_ptr<const nn::PolicyValueNet> network, uint64_t seed = randomSeed());
        NeuralAgent(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override { rng_.reseed(value); }

    private:
        std::shared_ptr<const nn::PolicyValueNet> network_;
        Options options_;
        std::vector<nn::Real> input_;
        Rng rng_;
    };
}
