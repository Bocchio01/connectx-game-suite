#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "ai/alphazero.hpp"
#include "ai/arena.hpp"
#include "ai/training/trainer.hpp"

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

            double cosineLearningRate(double initial, double final_fraction, long step, long total_steps)
            {
                if (total_steps <= 1)
                    return initial;
                const double progress = std::min(1.0, static_cast<double>(step) / static_cast<double>(total_steps));
                const double final_rate = initial * final_fraction;
                return final_rate + 0.5 * (initial - final_rate) * (1.0 + std::cos(3.14159265358979323846 * progress));
            }

        }

        /**
         * Plays one game against itself. Every position is searched with the network-guided tree
         * search; the visit counts become the policy target and, when the game is over, the
         * result becomes the value target of all the positions of the game (+1 for the positions
         * where the eventual winner was to move, -1 for the loser, 0 for a draw).
         */
        SelfPlayGame playSelfPlayGame(AlphaZeroSearch &search, const SelfPlayOptions &options, Rng &rng)
        {
            Position position(options.rows, options.cols, options.connect);
            SelfPlayGame game;

            while (!position.isTerminal())
            {
                const AlphaZeroSearch::Result result = search.search(position);

                int total = 0;
                for (int visits : result.visits)
                    total += visits;

                Example example;
                example.current = position.current();
                example.mask = position.mask();
                for (int c = 0; c < options.cols; ++c)
                    example.policy[static_cast<size_t>(c)] = total > 0 ? static_cast<float>(result.visits[static_cast<size_t>(c)]) / static_cast<float>(total) : 0.0f;
                game.examples.push_back(example);

                // Early moves are sampled in proportion to the visits (exploration), later ones are the most visited
                int column = -1;
                if (game.plies < options.temperature_plies && total > 0)
                {
                    int ticket = static_cast<int>(rng.below(static_cast<uint32_t>(total)));
                    for (int c = 0; c < options.cols; ++c)
                    {
                        ticket -= result.visits[static_cast<size_t>(c)];
                        if (ticket < 0)
                        {
                            column = c;
                            break;
                        }
                    }
                }
                if (column < 0)
                {
                    for (int c = 0; c < options.cols; ++c)
                    {
                        if (position.canPlay(c) && (column < 0 || result.visits[static_cast<size_t>(c)] > result.visits[static_cast<size_t>(column)]))
                            column = c;
                    }
                }

                position.play(column);
                ++game.plies;
            }

            // The player who made the last move (index plies-1) has won, unless the board is full
            if (position.lastMoveWon())
            {
                game.result = (game.plies % 2 == 1) ? 1 : -1; // an odd number of plies: the first player moved last
                for (size_t i = 0; i < game.examples.size(); ++i)
                {
                    const bool same_player_as_winner = ((game.plies - 1 - static_cast<int>(i)) % 2 == 0);
                    game.examples[i].value = same_player_as_winner ? 1.0f : -1.0f;
                }
            }
            else
            {
                game.result = 0;
            }

            return game;
        }

        std::shared_ptr<nn::PolicyValueNet> runSelfPlay(const SelfPlayOptions &options,
                                                        const std::function<void(const std::string &)> &log)
        {
            std::string reason;
            if (!Geometry::isSupported(options.rows, options.cols, options.connect, &reason))
                throw std::invalid_argument("Unsupported board: " + reason);

            auto say = [&](const std::string &text)
            {
                if (log)
                    log(text);
            };

            // The network
            std::shared_ptr<nn::PolicyValueNet> network;
            if (!options.init_model.empty())
            {
                network = std::make_shared<nn::PolicyValueNet>(nn::PolicyValueNet::load(options.init_model));
                const nn::NetworkConfig &config = network->config();
                if (config.policy_size != static_cast<size_t>(options.cols) || config.board_rows != static_cast<uint32_t>(options.rows) ||
                    config.board_cols != static_cast<uint32_t>(options.cols))
                    throw std::invalid_argument("The initial model does not match the board");
                say("Starting from " + options.init_model);
            }
            else
            {
                network = std::make_shared<nn::PolicyValueNet>(makeNetworkConfig(options.rows, options.cols, options.connect, options.hidden, options.encoding), options.seed);
            }

            if (!options.output_dir.empty())
                std::filesystem::create_directories(options.output_dir);

            BatchTrainer::Settings settings;
            settings.learning_rate = options.learning_rate;
            settings.weight_decay = options.weight_decay;
            settings.value_weight = options.value_weight;
            settings.max_gradient_norm = options.max_gradient_norm;
            BatchTrainer trainer(*network, options.rows, options.cols, options.connect, settings);

            std::vector<Example> teacher;
            if (options.teacher != nullptr)
                teacher = makeExamples(*options.teacher, options.teacher_target);

            std::deque<Example> buffer;
            Rng rng(options.seed ^ 0xC0FFEEULL);

            const long total_steps = static_cast<long>(options.iterations) * options.train_steps_per_iteration;
            long global_step = 0;

            {
                char line[256];
                std::snprintf(line, sizeof(line), "Self-play: %zu parameters, %d iterations x %d games, %d simulations per move, %d threads%s",
                              network->parameterCount(), options.iterations, options.games_per_iteration, options.simulations, options.threads,
                              teacher.empty() ? "" : ", teacher data mixed in");
                say(line);
            }

            const std::string latest_path = options.output_dir.empty() ? std::string() : (std::filesystem::path(options.output_dir) / "latest.cxnn").string();

            for (int iteration = 1; iteration <= options.iterations; ++iteration)
            {
                // ---- 1. self-play: generate games with the current network ---------------------------
                const auto play_start = Clock::now();

                AlphaZeroSearch::Options search_options;
                search_options.simulations = options.simulations;
                search_options.c_puct = options.c_puct;
                search_options.dirichlet_alpha = options.dirichlet_alpha;
                search_options.dirichlet_fraction = options.dirichlet_fraction;

                std::atomic<int> next_game(0);
                std::mutex mutex;
                int first_wins = 0, second_wins = 0, draws = 0;
                long plies = 0;
                std::vector<Example> new_examples;

                auto worker = [&](int worker_index)
                {
                    std::shared_ptr<const nn::PolicyValueNet> frozen = network; // not modified while the games are played
                    AlphaZeroSearch search(frozen, search_options,
                                           options.seed * 1000003ULL + static_cast<uint64_t>(iteration) * 7919ULL + static_cast<uint64_t>(worker_index));
                    Rng game_rng(options.seed * 31ULL + static_cast<uint64_t>(iteration) * 977ULL + static_cast<uint64_t>(worker_index) * 13ULL + 1);

                    for (;;)
                    {
                        if (next_game.fetch_add(1) >= options.games_per_iteration)
                            break;

                        SelfPlayGame game = playSelfPlayGame(search, options, game_rng);

                        std::lock_guard<std::mutex> lock(mutex);
                        new_examples.insert(new_examples.end(), game.examples.begin(), game.examples.end());
                        plies += game.plies;
                        if (game.result > 0)
                            ++first_wins;
                        else if (game.result < 0)
                            ++second_wins;
                        else
                            ++draws;
                    }
                };

                std::vector<std::thread> pool;
                for (int i = 0; i < std::max(1, options.threads); ++i)
                    pool.emplace_back(worker, i);
                for (auto &thread : pool)
                    thread.join();

                for (const Example &example : new_examples)
                    buffer.push_back(example);
                while (buffer.size() > options.buffer_capacity)
                    buffer.pop_front();

                const double play_seconds = secondsSince(play_start);

                // ---- 2. learning: train on the replay buffer -------------------------------------
                const auto train_start = Clock::now();
                std::vector<const Example *> batch(options.batch_size);
                double policy_sum = 0, value_sum = 0;

                const size_t teacher_per_batch = teacher.empty() ? 0 : static_cast<size_t>(std::lround(options.teacher_fraction * static_cast<double>(options.batch_size)));

                for (int s = 0; s < options.train_steps_per_iteration; ++s, ++global_step)
                {
                    trainer.setLearningRate(cosineLearningRate(options.learning_rate, options.final_learning_rate_fraction, global_step, total_steps));

                    for (size_t i = 0; i < options.batch_size; ++i)
                    {
                        if (i < teacher_per_batch)
                            batch[i] = &teacher[rng.below(static_cast<uint32_t>(teacher.size()))];
                        else
                            batch[i] = &buffer[rng.below(static_cast<uint32_t>(buffer.size()))];
                    }

                    const LossStats stats = trainer.step(batch, options.augment_mirror, rng);
                    policy_sum += stats.policy_loss;
                    value_sum += stats.value_loss;
                }

                const double train_seconds = secondsSince(train_start);
                const double games = std::max(1, first_wins + second_wins + draws);

                {
                    char line[512];
                    std::snprintf(line, sizeof(line),
                                  "iter %3d/%d | %d games, %.1f plies avg, first %.0f%% second %.0f%% draw %.0f%% | buffer %zu | loss: policy %.3f value %.3f | %.1fs + %.1fs",
                                  iteration, options.iterations, options.games_per_iteration, static_cast<double>(plies) / games,
                                  100.0 * first_wins / games, 100.0 * second_wins / games, 100.0 * draws / games, buffer.size(),
                                  policy_sum / std::max(1, options.train_steps_per_iteration), value_sum / std::max(1, options.train_steps_per_iteration),
                                  play_seconds, train_seconds);
                    say(line);
                }

                // ---- 3. checkpoint and evaluation ------------------------------------------------
                if (!latest_path.empty())
                {
                    std::ostringstream metadata;
                    metadata << options.metadata << " | self-play iteration " << iteration << "/" << options.iterations
                             << ", " << options.simulations << " simulations, " << options.games_per_iteration << " games per iteration";
                    network->save(latest_path, metadata.str());

                    const bool last = iteration == options.iterations;
                    if (options.eval_every > 0 && (iteration % options.eval_every == 0 || last) && !options.eval_opponents.empty())
                    {
                        const auto eval_start = Clock::now();

                        ArenaOptions arena;
                        arena.rows = options.rows;
                        arena.cols = options.cols;
                        arena.connect = options.connect;
                        arena.games = options.eval_games;
                        arena.threads = options.threads;
                        arena.seed = 100 + static_cast<uint64_t>(iteration);

                        const std::string az_spec = "az:model=" + latest_path + ",sims=" + std::to_string(options.eval_simulations);
                        const std::string nn_spec = "nn:model=" + latest_path;

                        std::ostringstream line;
                        line << "      eval (score %, " << options.eval_games << " games each) ";
                        for (const std::string &spec : {az_spec, nn_spec})
                        {
                            line << (spec == az_spec ? "| az(" + std::to_string(options.eval_simulations) + ") vs " : "| nn vs ");
                            for (const std::string &opponent : options.eval_opponents)
                            {
                                const PairResult result = playMatch(spec, opponent, arena);
                                line << opponent << " " << static_cast<int>(std::lround(100.0 * result.aScore())) << "  ";
                            }
                        }
                        line << "(" << static_cast<int>(secondsSince(eval_start)) << "s)";
                        say(line.str());
                    }

                    if (iteration % 10 == 0)
                    {
                        const std::string snapshot = (std::filesystem::path(options.output_dir) / ("iteration_" + std::to_string(iteration) + ".cxnn")).string();
                        network->save(snapshot, metadata.str());
                    }
                }
            }

            return network;
        }
    }
}
