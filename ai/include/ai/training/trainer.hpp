#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ai/alphazero.hpp"
#include "ai/rng.hpp"
#include "ai/training/dataset.hpp"
#include "nn/network.hpp"
#include "nn/optimizer.hpp"

namespace ai
{
    namespace training
    {
        /** A training example: a position, the policy the network should output and the value it should predict. */
        struct Example
        {
            uint64_t current = 0;
            uint64_t mask = 0;
            std::array<float, kMaxCols> policy;
            float value = 0.0f;
            uint32_t optimal_mask = 0; // columns that keep the result (only for the accuracy metrics; 0 if unknown)

            Example() { policy.fill(0.0f); }
        };

        /** Builds the examples of a labelled dataset (first `count` items, or all if 0). */
        std::vector<Example> makeExamples(const Dataset &dataset, PolicyTarget target, double soft_temperature = 1.0);

        /** The network configuration for a board: the inputs of the chosen encoding, one policy output per column. */
        nn::NetworkConfig makeNetworkConfig(int rows, int cols, int connect, const std::vector<size_t> &hidden,
                                            Encoding encoding = Encoding::RAW);

        // --------------------------------------------------------------------
        // One optimisation step
        // --------------------------------------------------------------------

        struct LossStats
        {
            double policy_loss = 0.0;
            double value_loss = 0.0;
            double gradient_norm = 0.0;
        };

        /**
         * Runs mini-batch gradient descent on a network: assembles the batch, forward pass, the two
         * losses (policy cross-entropy over the legal moves + value_weight * value MSE), backward
         * pass, gradient clipping and an AdamW update.
         */
        class BatchTrainer
        {
        public:
            struct Settings
            {
                double learning_rate = 1e-3;
                double weight_decay = 1e-4;
                double value_weight = 1.0;
                double max_gradient_norm = 5.0;
            };

            BatchTrainer(nn::PolicyValueNet &network, int rows, int cols, int connect, const Settings &settings);

            void setLearningRate(double learning_rate) { optimizer_.setLearningRate(static_cast<nn::Real>(learning_rate)); }

            /**
             * One gradient step on a batch.
             * @param augment_mirror mirror every example left-right with probability 1/2 (the game is symmetric,
             *                       so this doubles the data for free)
             */
            LossStats step(const std::vector<const Example *> &batch, bool augment_mirror, Rng &rng);

        private:
            nn::PolicyValueNet &network_;
            int rows_, cols_, connect_;
            Encoding encoding_;
            Settings settings_;
            nn::Adam optimizer_;
            std::vector<nn::Parameter *> parameters_;
            nn::Matrix inputs_, masks_, policy_targets_, value_targets_;
            nn::Matrix logits_, values_, dlogits_, dvalues_;
        };

        // --------------------------------------------------------------------
        // Evaluation of a network against labelled examples
        // --------------------------------------------------------------------

        struct EvalStats
        {
            double policy_loss = 0.0;       // cross-entropy against the target distribution
            double value_mse = 0.0;
            double optimal_move_rate = 0.0; // how often the best legal logit is a move that keeps the result
            double outcome_accuracy = 0.0;  // value rounded to win/draw/loss equals the true outcome
            size_t samples = 0;
        };

        EvalStats evaluate(const nn::PolicyValueNet &network, const std::vector<Example> &examples, int rows, int cols, int connect);

        // --------------------------------------------------------------------
        // Supervised learning from the solver
        // --------------------------------------------------------------------

        struct SupervisedOptions
        {
            std::vector<size_t> hidden = {128, 128};
            Encoding encoding = Encoding::RAW;
            int epochs = 10;
            size_t batch_size = 256;
            double learning_rate = 1e-3;
            double final_learning_rate_fraction = 0.05; // the learning rate decays (cosine) to this fraction
            double weight_decay = 1e-4;
            double value_weight = 1.0;
            double max_gradient_norm = 5.0;
            PolicyTarget target = PolicyTarget::OUTCOME;
            double soft_temperature = 1.0;
            bool augment_mirror = true;
            double validation_fraction = 0.05;
            uint64_t seed = 1;

            /** Start from the weights of this model instead of a random network (empty = random). */
            std::string init_model;

            /** Where to save the best model (by validation loss); empty = do not save. */
            std::string output_model;

            /** Free text stored in the model file. */
            std::string metadata;
        };

        struct SupervisedReport
        {
            int best_epoch = 0;
            EvalStats best_validation;
            size_t parameters = 0;
        };

        /**
         * Trains a policy/value network on a labelled dataset (the "teacher" is the exact solver):
         * the policy learns which moves keep the game-theoretic result, the value learns who is winning.
         * @param log receives one line of text per epoch
         * @return the best network (lowest validation loss) and a report
         */
        std::shared_ptr<nn::PolicyValueNet> trainSupervised(const Dataset &dataset, const SupervisedOptions &options,
                                                            SupervisedReport &report,
                                                            const std::function<void(const std::string &)> &log);

        // --------------------------------------------------------------------
        // Reinforcement learning by self-play (AlphaZero style)
        // --------------------------------------------------------------------

        struct SelfPlayOptions
        {
            int rows = 6;
            int cols = 7;
            int connect = 4;

            std::vector<size_t> hidden = {128, 128};
            Encoding encoding = Encoding::RAW;

            /** Start from this model (e.g. a supervised one) instead of a random network. */
            std::string init_model;

            int iterations = 40;
            int games_per_iteration = 100;
            int threads = 4;

            // Search
            int simulations = 100;
            double c_puct = 1.5;
            double dirichlet_alpha = 1.0;
            double dirichlet_fraction = 0.25;

            /** The first moves of a game are sampled in proportion to the visit counts (exploration);
             *  afterwards the most visited move is played. */
            int temperature_plies = 8;

            // Learning
            size_t batch_size = 256;
            int train_steps_per_iteration = 300;
            double learning_rate = 1e-3;
            double final_learning_rate_fraction = 0.1;
            double weight_decay = 1e-4;
            double value_weight = 1.0;
            double max_gradient_norm = 5.0;
            size_t buffer_capacity = 120000;
            bool augment_mirror = true;

            /**
             * Optional "teacher" examples (positions labelled by the solver) mixed into every batch:
             * a `teacher_fraction` of each batch comes from there. Keeps the early iterations on track.
             */
            const Dataset *teacher = nullptr;
            PolicyTarget teacher_target = PolicyTarget::OUTCOME;
            double teacher_fraction = 0.25;

            uint64_t seed = 1;

            /** Directory for the checkpoints (latest.cxnn, iteration_N.cxnn). */
            std::string output_dir;

            // Evaluation against fixed opponents (agent specs), with the current network
            int eval_every = 5;
            int eval_games = 40;
            int eval_simulations = 100;
            std::vector<std::string> eval_opponents = {"random", "greedy", "alphabeta:depth=4", "mcts:sims=1000,rollout=tactical"};

            std::string metadata;
        };

        /** One game of self-play and the training examples it produces. */
        struct SelfPlayGame
        {
            std::vector<Example> examples; // one per position, in game order
            int result = 0;                // +1: the player who moved first won, -1: the second, 0: draw
            int plies = 0;
        };

        /**
         * Plays one game against itself. Every position is searched with the network-guided tree
         * search; the visit counts become the policy target and, when the game is over, the
         * result becomes the value target of all the positions of the game (+1 for the positions
         * where the eventual winner was to move, -1 for the loser, 0 for a draw).
         * Only the search options (simulations, noise...) and the board of `options` are used.
         */
        SelfPlayGame playSelfPlayGame(AlphaZeroSearch &search, const SelfPlayOptions &options, Rng &rng);

        /** Runs the self-play loop; `log` receives progress lines. Returns the final network. */
        std::shared_ptr<nn::PolicyValueNet> runSelfPlay(const SelfPlayOptions &options,
                                                        const std::function<void(const std::string &)> &log);
    }
}
