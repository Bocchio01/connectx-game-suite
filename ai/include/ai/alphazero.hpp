#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ai/agent.hpp"
#include "nn/network.hpp"

namespace ai
{
    /**
     * Monte Carlo tree search guided by a policy/value network (AlphaZero style).
     *
     * It is MCTS (see MctsAgent) with two changes that make the search both much
     * more selective and much cheaper than random rollouts:
     *   - instead of simulating a game to the end, a new leaf is evaluated by the network's
     *     *value* head (who is winning?);
     *   - instead of treating all moves alike, the *policy* head gives every move a prior
     *     probability P and the selection rule (PUCT) favours moves that are both promising
     *     on average (Q) and that the network likes but have not been explored yet:
     *
     *         score(move) = Q(move) + c_puct * P(move) * sqrt(N_parent) / (1 + N(move))
     *
     * The visit counts at the root are an improved policy: better than the raw network output.
     * In self-play they are the target the network is trained towards (the policy-improvement
     * step), and the game result is the target for the value (policy evaluation).
     */
    class AlphaZeroSearch
    {
    public:
        struct Options
        {
            /** Network evaluations (= tree leaves) per move. */
            int simulations = 200;

            /** Exploration constant of the PUCT rule. */
            double c_puct = 1.5;

            /** Dirichlet noise mixed into the root priors to force exploration (self-play only). */
            double dirichlet_alpha = 1.0;
            double dirichlet_fraction = 0.0; // 0 = no noise; self-play uses 0.25
        };

        struct Result
        {
            /** Visits of each column (0 for illegal columns). */
            std::vector<int> visits;

            /** Mean value of each visited column for the player to move at the root, in [-1, 1]. */
            std::vector<double> values;

            /** Estimate of the root position for the player to move, in [-1, 1]. */
            double value = 0.0;

            /** Network evaluations performed. */
            uint64_t evaluations = 0;
        };

        AlphaZeroSearch(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed);

        /** @param limit_seconds optional wall-clock limit (0 = none), checked between simulations */
        Result search(const Position &root, double limit_seconds = 0.0);

        void seed(uint64_t value) { rng_.reseed(value); }
        const Options &options() const { return options_; }

    private:
        struct Node
        {
            int32_t first_child = -1;
            int32_t visits = 0;
            double value_sum = 0.0; // for the player who made the move leading to this node
            float prior = 0.0f;
            uint8_t move = 0;
            uint8_t child_count = 0;
            uint8_t terminal = 0; // 0 = not terminal, 1 = the player who moved here has won, 2 = draw
        };

        void iterate(const Position &root, bool add_noise);
        double expand(int32_t node, const Position &position);
        int32_t selectChild(const Node &parent) const;

        std::shared_ptr<const nn::PolicyValueNet> network_;
        Options options_;
        std::vector<Node> nodes_;
        std::vector<int32_t> path_;
        std::vector<nn::Real> input_;
        std::vector<nn::Real> logits_;
        uint64_t evaluations_;
        Rng rng_;
    };

    /**
     * Level 8: the network plus tree search. Very strong for its size: the search fixes the
     * mistakes of the raw policy, the policy keeps the search focused.
     */
    class AlphaZeroAgent : public Agent
    {
    public:
        struct Options
        {
            int simulations = 200;
            double c_puct = 1.5;

            /** > 0: pick the move by sampling visits^(1/temperature) instead of the most visited one. */
            double temperature = 0.0;

            /** > 0: also stop after this many milliseconds. */
            int time_limit_ms = 0;
        };

        AlphaZeroAgent(std::shared_ptr<const nn::PolicyValueNet> network, uint64_t seed = randomSeed());
        AlphaZeroAgent(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override;

    private:
        Options options_;
        AlphaZeroSearch search_;
        Rng rng_;
    };
}
