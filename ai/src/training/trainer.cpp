#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>

#include "ai/agents/neural_agent.hpp"
#include "ai/training/trainer.hpp"
#include "nn/loss.hpp"

namespace ai
{
    namespace training
    {
        namespace
        {
            using Clock = std::chrono::steady_clock;

            double secondsSince(Clock::time_point start)
            {
                return std::chrono::duration<double>(Clock::now() - start).count();
            }

            /** The learning rate of a cosine schedule from `initial` down to `initial * final_fraction`. */
            double cosineLearningRate(double initial, double final_fraction, long step, long total_steps)
            {
                if (total_steps <= 1)
                    return initial;
                const double progress = std::min(1.0, static_cast<double>(step) / static_cast<double>(total_steps));
                const double final_rate = initial * final_fraction;
                return final_rate + 0.5 * (initial - final_rate) * (1.0 + std::cos(3.14159265358979323846 * progress));
            }

            int outcomeClass(double value)
            {
                return value > 0.33 ? 1 : (value < -0.33 ? -1 : 0);
            }
        }

        // ====================================================================
        // Examples and networks
        // ====================================================================

        std::vector<Example> makeExamples(const Dataset &dataset, PolicyTarget target, double soft_temperature)
        {
            std::vector<Example> examples;
            examples.reserve(dataset.size());

            for (const LabeledPosition &item : dataset.items())
            {
                const Targets targets = makeTargets(item, dataset.cols(), target, soft_temperature);

                Example example;
                example.current = item.current;
                example.mask = item.mask;
                example.policy = targets.policy;
                example.value = targets.value;
                example.optimal_mask = targets.optimal_mask;
                examples.push_back(example);
            }

            return examples;
        }

        nn::NetworkConfig makeNetworkConfig(int rows, int cols, int connect, const std::vector<size_t> &hidden, Encoding encoding)
        {
            nn::NetworkConfig config;
            config.input_size = static_cast<size_t>(encodedSize(Geometry::get(rows, cols, connect), encoding));
            config.encoding = static_cast<uint32_t>(encoding);
            config.policy_size = static_cast<size_t>(cols);
            config.hidden = hidden;
            config.board_rows = static_cast<uint32_t>(rows);
            config.board_cols = static_cast<uint32_t>(cols);
            config.connect = static_cast<uint32_t>(connect);
            return config;
        }

        // ====================================================================
        // BatchTrainer
        // ====================================================================

        BatchTrainer::BatchTrainer(nn::PolicyValueNet &network, int rows, int cols, int connect, const Settings &settings)
            : network_(network),
              rows_(rows),
              cols_(cols),
              connect_(connect),
              encoding_(static_cast<Encoding>(network.config().encoding)),
              settings_(settings),
              optimizer_(static_cast<nn::Real>(settings.learning_rate), nn::Real(0.9), nn::Real(0.999), nn::Real(1e-8),
                         static_cast<nn::Real>(settings.weight_decay)),
              parameters_(network.parameters())
        {
        }

        LossStats BatchTrainer::step(const std::vector<const Example *> &batch, bool augment_mirror, Rng &rng)
        {
            const size_t n = batch.size();
            const size_t columns = static_cast<size_t>(cols_);

            inputs_.resize(n, network_.config().input_size);
            masks_.resize(n, columns);
            policy_targets_.resize(n, columns);
            value_targets_.resize(n, 1);

            for (size_t i = 0; i < n; ++i)
            {
                const Example &example = *batch[i];
                Position position = Position::fromBitboards(rows_, cols_, connect_, example.current, example.mask);

                const bool mirror = augment_mirror && (rng() & 1ULL) != 0;
                if (mirror)
                    position = position.mirrored();

                encodeInto(position, encoding_, i, inputs_, masks_);
                for (size_t c = 0; c < columns; ++c)
                    policy_targets_(i, c) = mirror ? example.policy[columns - 1 - c] : example.policy[c];
                value_targets_(i, 0) = example.value;
            }

            network_.zeroGrad();
            network_.forward(inputs_, logits_, values_);

            LossStats stats;
            stats.policy_loss = nn::softmaxCrossEntropy(logits_, policy_targets_, dlogits_, &masks_);
            stats.value_loss = nn::meanSquaredError(values_, value_targets_, dvalues_);

            for (size_t i = 0; i < dvalues_.size(); ++i)
                dvalues_.data()[i] *= static_cast<nn::Real>(settings_.value_weight);

            network_.backward(dlogits_, dvalues_);
            stats.gradient_norm = nn::clipGradientNorm(parameters_, static_cast<nn::Real>(settings_.max_gradient_norm));
            optimizer_.step(parameters_);
            return stats;
        }

        // ====================================================================
        // Evaluation
        // ====================================================================

        EvalStats evaluate(const nn::PolicyValueNet &network, const std::vector<Example> &examples, int rows, int cols, int connect)
        {
            EvalStats stats;
            if (examples.empty())
                return stats;

            const size_t columns = static_cast<size_t>(cols);
            const Encoding encoding = static_cast<Encoding>(network.config().encoding);
            std::vector<nn::Real> input(network.config().input_size);
            std::vector<nn::Real> logits(columns), probabilities(columns);
            std::vector<unsigned char> legal(columns);

            double policy_loss = 0, value_mse = 0;
            size_t optimal_hits = 0, optimal_total = 0, outcome_hits = 0;

            for (const Example &example : examples)
            {
                const Position position = Position::fromBitboards(rows, cols, connect, example.current, example.mask);
                encodePosition(position, encoding, input.data());

                nn::Real value = 0;
                network.predict(input.data(), logits.data(), &value);

                for (size_t c = 0; c < columns; ++c)
                    legal[c] = position.canPlay(static_cast<int>(c)) ? 1 : 0;
                nn::softmax(logits.data(), columns, probabilities.data(), legal.data());

                double loss = 0;
                for (size_t c = 0; c < columns; ++c)
                {
                    if (legal[c])
                        loss -= static_cast<double>(example.policy[c]) * std::log(static_cast<double>(probabilities[c]) + 1e-12);
                }
                policy_loss += loss;

                const double error = static_cast<double>(value) - static_cast<double>(example.value);
                value_mse += error * error;

                if (outcomeClass(value) == static_cast<int>(std::lround(example.value)))
                    ++outcome_hits;

                if (example.optimal_mask != 0)
                {
                    int best = -1;
                    for (size_t c = 0; c < columns; ++c)
                    {
                        if (legal[c] && (best < 0 || logits[c] > logits[static_cast<size_t>(best)]))
                            best = static_cast<int>(c);
                    }
                    ++optimal_total;
                    if (best >= 0 && (example.optimal_mask & (1u << best)) != 0)
                        ++optimal_hits;
                }
            }

            const double n = static_cast<double>(examples.size());
            stats.samples = examples.size();
            stats.policy_loss = policy_loss / n;
            stats.value_mse = value_mse / n;
            stats.outcome_accuracy = static_cast<double>(outcome_hits) / n;
            stats.optimal_move_rate = optimal_total > 0 ? static_cast<double>(optimal_hits) / static_cast<double>(optimal_total) : 0.0;
            return stats;
        }

        // ====================================================================
        // Supervised learning
        // ====================================================================

        std::shared_ptr<nn::PolicyValueNet> trainSupervised(const Dataset &dataset, const SupervisedOptions &options,
                                                            SupervisedReport &report,
                                                            const std::function<void(const std::string &)> &log)
        {
            if (dataset.size() < 100)
                throw std::invalid_argument("The dataset is too small to train on");

            const int rows = dataset.rows(), cols = dataset.cols(), connect = dataset.connect();
            Rng rng(options.seed);

            // Examples, shuffled and split into training and validation sets
            std::vector<Example> all = makeExamples(dataset, options.target, options.soft_temperature);
            std::vector<size_t> order(all.size());
            std::iota(order.begin(), order.end(), size_t(0));
            for (size_t i = order.size(); i > 1; --i)
                std::swap(order[i - 1], order[rng.below(static_cast<uint32_t>(i))]);

            const size_t validation_count = std::max<size_t>(1, static_cast<size_t>(options.validation_fraction * static_cast<double>(all.size())));
            std::vector<Example> validation, training;
            validation.reserve(validation_count);
            training.reserve(all.size() - validation_count);
            for (size_t i = 0; i < order.size(); ++i)
                (i < validation_count ? validation : training).push_back(all[order[i]]);

            // The network
            std::shared_ptr<nn::PolicyValueNet> network;
            if (!options.init_model.empty())
            {
                network = std::make_shared<nn::PolicyValueNet>(nn::PolicyValueNet::load(options.init_model));
                const nn::NetworkConfig &config = network->config();
                if (config.policy_size != static_cast<size_t>(cols) || config.board_rows != static_cast<uint32_t>(rows) ||
                    config.board_cols != static_cast<uint32_t>(cols))
                    throw std::invalid_argument("The initial model does not match the dataset board");
            }
            else
            {
                network = std::make_shared<nn::PolicyValueNet>(makeNetworkConfig(rows, cols, connect, options.hidden, options.encoding), options.seed);
            }

            auto best = std::make_shared<nn::PolicyValueNet>(network->config(), 1);
            best->copyWeightsFrom(*network);

            BatchTrainer::Settings settings;
            settings.learning_rate = options.learning_rate;
            settings.weight_decay = options.weight_decay;
            settings.value_weight = options.value_weight;
            settings.max_gradient_norm = options.max_gradient_norm;
            BatchTrainer trainer(*network, rows, cols, connect, settings);

            const size_t steps_per_epoch = std::max<size_t>(1, training.size() / options.batch_size);
            const long total_steps = static_cast<long>(steps_per_epoch) * options.epochs;
            long step = 0;

            report = SupervisedReport();
            report.parameters = network->parameterCount();
            double best_loss = 1e30;

            if (log)
            {
                char line[256];
                std::snprintf(line, sizeof(line), "Training %zu parameters on %zu positions (%zu for validation), %zu steps per epoch",
                              network->parameterCount(), training.size(), validation.size(), steps_per_epoch);
                log(line);
            }

            std::vector<size_t> indices(training.size());
            std::iota(indices.begin(), indices.end(), size_t(0));
            std::vector<const Example *> batch(options.batch_size);

            for (int epoch = 1; epoch <= options.epochs; ++epoch)
            {
                const auto epoch_start = Clock::now();

                for (size_t i = indices.size(); i > 1; --i)
                    std::swap(indices[i - 1], indices[rng.below(static_cast<uint32_t>(i))]);

                double policy_sum = 0, value_sum = 0;
                for (size_t b = 0; b < steps_per_epoch; ++b, ++step)
                {
                    trainer.setLearningRate(cosineLearningRate(options.learning_rate, options.final_learning_rate_fraction, step, total_steps));

                    for (size_t i = 0; i < options.batch_size; ++i)
                        batch[i] = &training[indices[(b * options.batch_size + i) % indices.size()]];

                    const LossStats stats = trainer.step(batch, options.augment_mirror, rng);
                    policy_sum += stats.policy_loss;
                    value_sum += stats.value_loss;
                }

                const EvalStats validation_stats = evaluate(*network, validation, rows, cols, connect);
                const double total_loss = validation_stats.policy_loss + options.value_weight * validation_stats.value_mse;

                if (total_loss < best_loss)
                {
                    best_loss = total_loss;
                    best->copyWeightsFrom(*network);
                    report.best_epoch = epoch;
                    report.best_validation = validation_stats;

                    if (!options.output_model.empty())
                        best->save(options.output_model, options.metadata);
                }

                if (log)
                {
                    char line[512];
                    std::snprintf(line, sizeof(line),
                                  "epoch %2d/%d  lr %.5f  train: policy %.4f value %.4f | validation: policy %.4f value %.4f  best-move %.1f%%  outcome %.1f%%  (%.1fs)%s",
                                  epoch, options.epochs,
                                  cosineLearningRate(options.learning_rate, options.final_learning_rate_fraction, step, total_steps),
                                  policy_sum / static_cast<double>(steps_per_epoch), value_sum / static_cast<double>(steps_per_epoch),
                                  validation_stats.policy_loss, validation_stats.value_mse,
                                  100.0 * validation_stats.optimal_move_rate, 100.0 * validation_stats.outcome_accuracy,
                                  secondsSince(epoch_start), report.best_epoch == epoch ? "  *" : "");
                    log(line);
                }
            }

            return best;
        }
    }
}
