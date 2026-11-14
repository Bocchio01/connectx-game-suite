// connectx-train: datasets, supervised learning from the exact solver, AlphaZero-style self-play

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ai/agents/neural_agent.hpp"
#include "ai/encoding.hpp"
#include "ai/factory.hpp"
#include "ai/model_cache.hpp"
#include "ai/rng.hpp"
#include "ai/training/dataset.hpp"
#include "ai/training/trainer.hpp"

using namespace ai;
using namespace ai::training;

namespace
{
    // ------------------------------------------------------------------------
    // Tiny command line parser: "--key value", "--key=value" and "--flag"
    // ------------------------------------------------------------------------
    class Args
    {
    public:
        Args(int argc, char *argv[], int first)
        {
            for (int i = first; i < argc; ++i)
            {
                std::string arg = argv[i];
                if (arg.rfind("--", 0) != 0)
                    throw std::invalid_argument("Unexpected argument '" + arg + "'");

                arg = arg.substr(2);
                const size_t equals = arg.find('=');
                if (equals != std::string::npos)
                {
                    values_[arg.substr(0, equals)] = arg.substr(equals + 1);
                }
                else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                {
                    values_[arg] = argv[++i];
                }
                else
                {
                    values_[arg] = "1"; // a flag
                }
            }
        }

        bool has(const std::string &key) const { return values_.count(key) != 0; }

        std::string str(const std::string &key, const std::string &fallback = "") const
        {
            used_.push_back(key);
            auto it = values_.find(key);
            return it == values_.end() ? fallback : it->second;
        }

        long integer(const std::string &key, long fallback) const
        {
            used_.push_back(key);
            auto it = values_.find(key);
            if (it == values_.end())
                return fallback;
            try
            {
                return std::stol(it->second);
            }
            catch (const std::exception &)
            {
                throw std::invalid_argument("--" + key + " expects an integer, got '" + it->second + "'");
            }
        }

        double real(const std::string &key, double fallback) const
        {
            used_.push_back(key);
            auto it = values_.find(key);
            if (it == values_.end())
                return fallback;
            try
            {
                return std::stod(it->second);
            }
            catch (const std::exception &)
            {
                throw std::invalid_argument("--" + key + " expects a number, got '" + it->second + "'");
            }
        }

        bool flag(const std::string &key) const
        {
            used_.push_back(key);
            auto it = values_.find(key);
            return it != values_.end() && it->second != "0" && it->second != "false";
        }

        std::vector<std::string> list(const std::string &key) const
        {
            std::vector<std::string> items;
            std::stringstream stream(str(key));
            std::string item;
            while (std::getline(stream, item, ','))
            {
                if (!item.empty())
                    items.push_back(item);
            }
            return items;
        }

        std::vector<size_t> sizes(const std::string &key, const std::vector<size_t> &fallback) const
        {
            if (!has(key))
                return fallback;
            std::vector<size_t> result;
            for (const std::string &item : list(key))
                result.push_back(static_cast<size_t>(std::stoul(item)));
            return result;
        }

        /** Fails on options that no one asked for (typos). Call after reading all the options. */
        void rejectUnused() const
        {
            for (const auto &entry : values_)
            {
                if (std::find(used_.begin(), used_.end(), entry.first) == used_.end())
                    throw std::invalid_argument("Unknown option --" + entry.first);
            }
        }

    private:
        std::map<std::string, std::string> values_;
        mutable std::vector<std::string> used_;
    };

    void printHelp()
    {
        std::cout <<
            R"(connectx-train: datasets, supervised learning and self-play for the neural agents

Usage: connectx-train <command> [options]

Commands
  gen-data       Play games and label the positions with the exact solver (the "teacher")
      --out FILE            dataset to write (.cxds)                       [required]
      --count N             labelled positions to produce                  [10000]
      --threads N           worker threads                                 [cores]
      --rows R --cols C --connect K   board                                [6 7 4]
      --min-stones N        skip positions with fewer stones               [8]
      --player-depth D      alpha-beta depth of the game generator (0 = random) [4]
      --epsilon P           probability of a random move                   [0.2]
      --sample P            probability of labelling a visited position    [0.35]
      --exact               exact scores (default: win/draw/loss only, much faster)
      --solve-time MS       skip positions the solver cannot label in time [4000]
      --seed S

  supervised     Train a network to imitate the solver
      --data FILE[,FILE]    datasets                                       [required]
      --out FILE            model to write (.cxnn)                         [required]
      --hidden A,B,...      hidden layer widths                            [128,128]
      --encoding E          network input: raw | tactical | windows        [raw]
      --epochs N [10]  --batch N [256]  --lr X [0.001]  --lr-final F [0.05]  --weight-decay X [0.0001]
      --target outcome|best|soft   what the policy learns                 [outcome]
      --temperature T       for --target soft                              [1]
      --value-weight W      weight of the value loss                       [1]
      --validation F        fraction of positions kept for validation      [0.05]
      --no-mirror           disable the left-right mirror augmentation
      --init FILE           start from an existing model
      --seed S

  selfplay       AlphaZero-style reinforcement learning: the network plays itself with tree search
      --out-dir DIR         checkpoints and logs                           [required]
      --init FILE           start from an existing model (e.g. a supervised one)
      --hidden A,B,...      hidden layer widths (when starting from scratch) [128,128]
      --encoding E          network input: raw | tactical | windows (from scratch) [raw]
      --iterations N [40]  --games N [100]  --sims N [100]  --threads N [cores]
      --steps N             training steps per iteration                   [300]
      --batch N [256]  --lr X [0.001]  --buffer N [120000]
      --teacher FILE        labelled dataset mixed into every batch
      --teacher-fraction F  share of each batch coming from it             [0.25]
      --eval-every N [5]  --eval-games N [40]  --eval-sims N [100]  --eval-opponents A;B (agent specs, ';' separated)
      --rows R --cols C --connect K   board                                [6 7 4]
      --seed S

  inspect        Print the architecture and the metadata of a model        --model FILE
  dataset-info   Statistics of a dataset                                   --data FILE
  merge-data     Merge datasets, dropping duplicated positions             --data FILE[,FILE...] --out FILE
  eval           Accuracy of a model on a labelled dataset                 --model FILE --data FILE [--target outcome]
  agent-accuracy How often any agent keeps the game-theoretic result      --agent SPEC --data FILE [--limit N] [--threads N]
  export-vectors Test vectors for checking the TensorFlow / TFLite export  --model FILE --out FILE.json [--count 64] [--seed 1]
)";
    }

    int threadsDefault()
    {
        return static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    }

    PolicyTarget parseTarget(const std::string &name)
    {
        if (name == "outcome")
            return PolicyTarget::OUTCOME;
        if (name == "best")
            return PolicyTarget::BEST;
        if (name == "soft")
            return PolicyTarget::SOFT;
        throw std::invalid_argument("--target must be outcome, best or soft");
    }

    // ------------------------------------------------------------------------
    // Commands
    // ------------------------------------------------------------------------

    int genData(const Args &args)
    {
        GenerateOptions options;
        options.rows = static_cast<int>(args.integer("rows", 6));
        options.cols = static_cast<int>(args.integer("cols", 7));
        options.connect = static_cast<int>(args.integer("connect", 4));
        options.count = static_cast<size_t>(args.integer("count", 10000));
        options.threads = static_cast<int>(args.integer("threads", threadsDefault()));
        options.seed = static_cast<uint64_t>(args.integer("seed", 1));
        options.min_stones = static_cast<int>(args.integer("min-stones", 8));
        options.player_depth = static_cast<int>(args.integer("player-depth", 4));
        options.epsilon = args.real("epsilon", 0.2);
        options.sample_probability = args.real("sample", 0.35);
        options.weak = !args.flag("exact");
        options.solve_time_ms = static_cast<int>(args.integer("solve-time", 4000));
        const std::string out = args.str("out");
        args.rejectUnused();

        if (out.empty())
            throw std::invalid_argument("gen-data needs --out FILE");

        std::cout << "Generating " << options.count << " labelled positions (" << (options.weak ? "win/draw/loss" : "exact scores")
                  << ") on a " << options.rows << "x" << options.cols << " board, " << options.threads << " thread(s)...\n";

        const auto start = std::chrono::steady_clock::now();
        size_t last_report = 0;
        const Dataset dataset = generateDataset(options, [&](size_t done, size_t total, size_t skipped)
                                                {
            if (done - last_report >= std::max<size_t>(1000, total / 20) || done == total)
            {
                last_report = done;
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::printf("  %zu / %zu positions  (%zu skipped: solver timeout)  %.0fs elapsed, ~%.0fs left\n", done, total, skipped, seconds,
                            seconds * static_cast<double>(total - done) / static_cast<double>(std::max<size_t>(done, 1)));
                std::fflush(stdout);
            } });

        dataset.save(out);
        std::cout << "Saved " << dataset.size() << " positions to " << out << " in "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s\n";
        return EXIT_SUCCESS;
    }

    int supervised(const Args &args)
    {
        SupervisedOptions options;
        options.hidden = args.sizes("hidden", options.hidden);
        options.encoding = parseEncoding(args.str("encoding", "raw"));
        options.epochs = static_cast<int>(args.integer("epochs", options.epochs));
        options.batch_size = static_cast<size_t>(args.integer("batch", static_cast<long>(options.batch_size)));
        options.learning_rate = args.real("lr", options.learning_rate);
        options.final_learning_rate_fraction = args.real("lr-final", options.final_learning_rate_fraction);
        options.weight_decay = args.real("weight-decay", options.weight_decay);
        options.value_weight = args.real("value-weight", options.value_weight);
        options.target = parseTarget(args.str("target", "outcome"));
        options.soft_temperature = args.real("temperature", options.soft_temperature);
        options.validation_fraction = args.real("validation", options.validation_fraction);
        options.augment_mirror = !args.flag("no-mirror");
        options.init_model = args.str("init");
        options.seed = static_cast<uint64_t>(args.integer("seed", 1));
        options.output_model = args.str("out");
        const std::vector<std::string> data_files = args.list("data");
        args.rejectUnused();

        if (data_files.empty() || options.output_model.empty())
            throw std::invalid_argument("supervised needs --data FILE and --out FILE");

        Dataset dataset = Dataset::load(data_files[0]);
        for (size_t i = 1; i < data_files.size(); ++i)
            dataset.append(Dataset::load(data_files[i]));
        std::cout << "Loaded " << dataset.size() << " labelled positions (" << dataset.rows() << "x" << dataset.cols()
                  << ", connect " << dataset.connect() << ")\n";

        std::ostringstream metadata;
        metadata << "supervised from the exact solver: " << dataset.size() << " positions, " << options.epochs << " epochs, batch "
                 << options.batch_size << ", lr " << options.learning_rate << ", target "
                 << args.str("target", "outcome") << (options.augment_mirror ? ", mirror augmentation" : "");
        options.metadata = metadata.str();

        SupervisedReport report;
        const auto network = trainSupervised(dataset, options, report, [](const std::string &line)
                                             { std::cout << line << std::endl; });

        std::cout << "\nBest epoch " << report.best_epoch << ": plays a result-preserving move in "
                  << 100.0 * report.best_validation.optimal_move_rate << "% of the validation positions, value outcome accuracy "
                  << 100.0 * report.best_validation.outcome_accuracy << "%\n"
                  << "Model saved to " << options.output_model << " (" << report.parameters << " parameters)\n";
        return EXIT_SUCCESS;
    }

    int selfPlay(const Args &args)
    {
        SelfPlayOptions options;
        options.rows = static_cast<int>(args.integer("rows", 6));
        options.cols = static_cast<int>(args.integer("cols", 7));
        options.connect = static_cast<int>(args.integer("connect", 4));
        options.hidden = args.sizes("hidden", options.hidden);
        options.encoding = parseEncoding(args.str("encoding", "raw"));
        options.init_model = args.str("init");
        options.iterations = static_cast<int>(args.integer("iterations", options.iterations));
        options.games_per_iteration = static_cast<int>(args.integer("games", options.games_per_iteration));
        options.simulations = static_cast<int>(args.integer("sims", options.simulations));
        options.threads = static_cast<int>(args.integer("threads", threadsDefault()));
        options.train_steps_per_iteration = static_cast<int>(args.integer("steps", options.train_steps_per_iteration));
        options.batch_size = static_cast<size_t>(args.integer("batch", static_cast<long>(options.batch_size)));
        options.learning_rate = args.real("lr", options.learning_rate);
        options.final_learning_rate_fraction = args.real("lr-final", options.final_learning_rate_fraction);
        options.buffer_capacity = static_cast<size_t>(args.integer("buffer", static_cast<long>(options.buffer_capacity)));
        options.teacher_fraction = args.real("teacher-fraction", options.teacher_fraction);
        options.dirichlet_fraction = args.real("noise", options.dirichlet_fraction);
        options.temperature_plies = static_cast<int>(args.integer("temperature-plies", options.temperature_plies));
        options.eval_every = static_cast<int>(args.integer("eval-every", options.eval_every));
        options.eval_games = static_cast<int>(args.integer("eval-games", options.eval_games));
        options.eval_simulations = static_cast<int>(args.integer("eval-sims", options.eval_simulations));
        options.seed = static_cast<uint64_t>(args.integer("seed", 1));
        options.output_dir = args.str("out-dir");
        const std::string teacher_file = args.str("teacher");

        if (args.has("eval-opponents"))
        {
            options.eval_opponents.clear();
            std::stringstream stream(args.str("eval-opponents"));
            std::string item;
            while (std::getline(stream, item, ';'))
            {
                if (!item.empty())
                    options.eval_opponents.push_back(item);
            }
        }
        args.rejectUnused();

        if (options.output_dir.empty())
            throw std::invalid_argument("selfplay needs --out-dir DIR");

        Dataset teacher;
        if (!teacher_file.empty())
        {
            teacher = Dataset::load(teacher_file);
            options.teacher = &teacher;
            std::cout << "Teacher: " << teacher.size() << " solver-labelled positions mixed into every batch\n";
        }

        // Recorded in the model file: how it was made
        std::ostringstream metadata;
        metadata << "AlphaZero-style self-play";
        if (!options.init_model.empty())
            metadata << " starting from " << std::filesystem::path(options.init_model).filename().string();
        metadata << ", encoding " << encodingName(options.encoding) << ", " << options.iterations << " iterations of "
                 << options.games_per_iteration << " games with " << options.simulations << " simulations per move";
        if (options.teacher != nullptr)
            metadata << ", " << static_cast<int>(100 * options.teacher_fraction) << "% of every batch from " << teacher.size()
                     << " solver-labelled positions";
        options.metadata = metadata.str();

        std::ofstream log_file(options.output_dir + "/selfplay.log", std::ios::app);
        const auto network = runSelfPlay(options, [&](const std::string &line)
                                         {
            std::cout << line << std::endl;
            if (log_file)
                log_file << line << std::endl; });
        (void)network;

        std::cout << "Final model: " << options.output_dir << "/latest.cxnn\n";
        return EXIT_SUCCESS;
    }

    int inspect(const Args &args)
    {
        const std::string path = args.str("model");
        args.rejectUnused();
        if (path.empty())
            throw std::invalid_argument("inspect needs --model FILE");

        std::string metadata;
        const nn::PolicyValueNet net = nn::PolicyValueNet::load(resolveModelPath(path), &metadata);
        const nn::NetworkConfig &config = net.config();

        std::cout << "Model " << path << "\n"
                  << "  board:       " << config.board_rows << "x" << config.board_cols << ", connect " << config.connect << "\n"
                  << "  input:       " << config.input_size << " numbers, encoding '"
                  << (config.encoding <= static_cast<uint32_t>(Encoding::WINDOWS) ? encodingName(static_cast<Encoding>(config.encoding)) : "?") << "'\n"
                  << "  trunk:       ";
        for (size_t hidden : config.hidden)
            std::cout << "Dense(" << hidden << ")+ReLU  ";
        std::cout << "\n  policy head: Dense(" << config.policy_size << ")\n"
                  << "  value head:  Dense(1)+tanh\n"
                  << "  parameters:  " << net.parameterCount() << " (" << net.parameterCount() * 4 / 1024 << " KiB as float32, ~"
                  << net.parameterCount() / 1024 << " KiB as int8)\n"
                  << "  metadata:    " << (metadata.empty() ? "(none)" : metadata) << "\n";
        return EXIT_SUCCESS;
    }

    int mergeData(const Args &args)
    {
        const std::vector<std::string> files = args.list("data");
        const std::string out = args.str("out");
        args.rejectUnused();
        if (files.empty() || out.empty())
            throw std::invalid_argument("merge-data needs --data FILE[,FILE...] and --out FILE");

        Dataset merged = Dataset::load(files[0]);
        const size_t first = merged.size();
        for (size_t i = 1; i < files.size(); ++i)
            merged.append(Dataset::load(files[i]));

        // Drop duplicated positions (different runs can reach the same position)
        Dataset unique(merged.rows(), merged.cols(), merged.connect());
        std::map<uint64_t, bool> seen;
        for (const LabeledPosition &item : merged.items())
        {
            if (seen.emplace(item.current + item.mask, true).second)
                unique.add(item);
        }

        unique.save(out);
        std::cout << "Merged " << files.size() << " files (first: " << first << ", total " << merged.size() << " positions) into "
                  << unique.size() << " unique positions: " << out << "\n";
        return EXIT_SUCCESS;
    }

    int datasetInfo(const Args &args)
    {
        const std::string path = args.str("data");
        args.rejectUnused();
        if (path.empty())
            throw std::invalid_argument("dataset-info needs --data FILE");

        const Dataset dataset = Dataset::load(path);
        std::vector<size_t> by_stones(static_cast<size_t>(dataset.rows() * dataset.cols() + 1), 0);
        size_t wins = 0, draws = 0, losses = 0;
        for (const LabeledPosition &item : dataset.items())
        {
            ++by_stones[static_cast<size_t>(popcount(item.mask))];
            (item.value > 0 ? wins : (item.value < 0 ? losses : draws))++;
        }

        std::cout << dataset.size() << " positions, board " << dataset.rows() << "x" << dataset.cols() << ", connect " << dataset.connect() << "\n"
                  << "  value for the player to move: win " << 100.0 * wins / dataset.size() << "%, draw " << 100.0 * draws / dataset.size()
                  << "%, loss " << 100.0 * losses / dataset.size() << "%\n  stones on the board:\n";
        for (size_t s = 0; s < by_stones.size(); ++s)
        {
            if (by_stones[s] > 0)
                std::cout << "    " << s << ": " << by_stones[s] << "\n";
        }
        return EXIT_SUCCESS;
    }

    int evalModel(const Args &args)
    {
        const std::string model_path = args.str("model");
        const std::string data_path = args.str("data");
        const PolicyTarget target = parseTarget(args.str("target", "outcome"));
        args.rejectUnused();
        if (model_path.empty() || data_path.empty())
            throw std::invalid_argument("eval needs --model FILE and --data FILE");

        const nn::PolicyValueNet net = nn::PolicyValueNet::load(resolveModelPath(model_path));
        const Dataset dataset = Dataset::load(data_path);
        const std::vector<Example> examples = makeExamples(dataset, target);
        const EvalStats stats = evaluate(net, examples, dataset.rows(), dataset.cols(), dataset.connect());

        std::cout << "On " << stats.samples << " positions: result-preserving move chosen in " << 100.0 * stats.optimal_move_rate
                  << "%, value outcome accuracy " << 100.0 * stats.outcome_accuracy << "%, value MSE " << stats.value_mse
                  << ", policy cross-entropy " << stats.policy_loss << "\n";
        return EXIT_SUCCESS;
    }

    /**
     * How often an agent plays a move that keeps the game-theoretic result, measured on positions
     * labelled by the exact solver. Reported separately for winning, drawn and lost positions
     * (in a lost position every move "keeps" the result, so only the first two are informative).
     */
    int agentAccuracy(const Args &args)
    {
        const std::string spec = args.str("agent");
        const std::string data_path = args.str("data");
        const size_t limit = static_cast<size_t>(args.integer("limit", 0));
        const int threads = static_cast<int>(args.integer("threads", threadsDefault()));
        args.rejectUnused();
        if (spec.empty() || data_path.empty())
            throw std::invalid_argument("agent-accuracy needs --agent SPEC and --data FILE");

        const Dataset dataset = Dataset::load(data_path);
        const size_t total = limit > 0 ? std::min(limit, dataset.size()) : dataset.size();

        struct Counts
        {
            size_t hits[3] = {0, 0, 0};  // by outcome: win, draw, loss
            size_t total[3] = {0, 0, 0};
            double seconds = 0;
        };
        std::vector<Counts> counts(static_cast<size_t>(std::max(1, threads)));

        auto worker = [&](size_t index)
        {
            AgentPtr agent = createAgent(spec);
            const size_t stride = counts.size();
            for (size_t i = index; i < total; i += stride)
            {
                const LabeledPosition &item = dataset[i];
                const Position position = Position::fromBitboards(dataset.rows(), dataset.cols(), dataset.connect(), item.current, item.mask);
                const Targets targets = makeTargets(item, dataset.cols(), PolicyTarget::OUTCOME);

                const MoveChoice choice = agent->chooseMove(position);
                const size_t kind = item.value > 0 ? 0 : (item.value == 0 ? 1 : 2);
                ++counts[index].total[kind];
                if ((targets.optimal_mask >> choice.column) & 1u)
                    ++counts[index].hits[kind];
                counts[index].seconds += choice.seconds;
            }
        };

        std::vector<std::thread> pool;
        for (size_t t = 0; t < counts.size(); ++t)
            pool.emplace_back(worker, t);
        for (auto &thread : pool)
            thread.join();

        Counts sum;
        for (const Counts &c : counts)
        {
            for (int k = 0; k < 3; ++k)
            {
                sum.hits[k] += c.hits[k];
                sum.total[k] += c.total[k];
            }
            sum.seconds += c.seconds;
        }

        auto percent = [](size_t hits, size_t n)
        { return n > 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(n) : 0.0; };

        const size_t informative_total = sum.total[0] + sum.total[1];
        const size_t informative_hits = sum.hits[0] + sum.hits[1];
        std::printf("%-46s  winning positions: %5.1f%% (%zu)   drawn: %5.1f%% (%zu)   win+draw: %5.1f%%   avg %.2f ms/move\n", spec.c_str(),
                    percent(sum.hits[0], sum.total[0]), sum.total[0], percent(sum.hits[1], sum.total[1]), sum.total[1],
                    percent(informative_hits, informative_total), 1000.0 * sum.seconds / static_cast<double>(std::max<size_t>(total, 1)));
        return EXIT_SUCCESS;
    }

    /** Test vectors: inputs and the exact outputs of the C++ network, to validate the TensorFlow / TFLite export. */
    int exportVectors(const Args &args)
    {
        const std::string model_path = args.str("model");
        const std::string out_path = args.str("out");
        const size_t count = static_cast<size_t>(args.integer("count", 64));
        const uint64_t seed = static_cast<uint64_t>(args.integer("seed", 1));
        args.rejectUnused();
        if (model_path.empty() || out_path.empty())
            throw std::invalid_argument("export-vectors needs --model FILE and --out FILE");

        const nn::PolicyValueNet net = nn::PolicyValueNet::load(resolveModelPath(model_path));
        const nn::NetworkConfig &config = net.config();
        const int rows = static_cast<int>(config.board_rows), cols = static_cast<int>(config.board_cols), connect = static_cast<int>(config.connect);
        if (rows == 0 || cols == 0 || connect == 0)
            throw std::invalid_argument("The model does not record its board: cannot generate positions");

        Rng rng(seed);
        std::ofstream out(out_path);
        if (!out)
            throw std::runtime_error("Cannot write " + out_path);

        auto number = [](double v)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.9g", v);
            return std::string(buffer);
        };

        out << "{\n  \"format\": \"connectx-test-vectors-1\",\n"
            << "  \"rows\": " << rows << ", \"cols\": " << cols << ", \"connect\": " << connect << ",\n"
            << "  \"encoding\": \"" << encodingName(static_cast<Encoding>(config.encoding)) << "\",\n"
            << "  \"input_size\": " << config.input_size << ", \"policy_size\": " << config.policy_size << ",\n"
            << "  \"samples\": [\n";

        std::vector<nn::Real> input(config.input_size), logits(config.policy_size);
        for (size_t i = 0; i < count; ++i)
        {
            // A random, non-terminal position (random play for a random number of moves)
            Position position(rows, cols, connect);
            const int target_stones = static_cast<int>(rng.below(static_cast<uint32_t>(rows * cols - 1)));
            while (position.moves() < target_stones && !position.isTerminal())
            {
                const std::vector<int> moves = position.legalMoves();
                position.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }
            if (position.isTerminal())
            {
                --i;
                continue;
            }

            encodePosition(position, static_cast<Encoding>(config.encoding), input.data());
            nn::Real value = 0;
            net.predict(input.data(), logits.data(), &value);

            out << "    {\"cells\": [";
            for (int r = 0; r < rows; ++r)
            {
                out << (r ? "," : "") << "[";
                for (int c = 0; c < cols; ++c)
                    out << (c ? "," : "") << position.cell(r, c);
                out << "]";
            }
            out << "], \"input\": [";
            for (size_t k = 0; k < input.size(); ++k)
                out << (k ? "," : "") << number(input[k]);
            out << "], \"logits\": [";
            for (size_t k = 0; k < logits.size(); ++k)
                out << (k ? "," : "") << number(logits[k]);
            out << "], \"value\": " << number(value) << "}" << (i + 1 < count ? "," : "") << "\n";
        }

        out << "  ]\n}\n";
        std::cout << "Wrote " << count << " test vectors to " << out_path << "\n";
        return EXIT_SUCCESS;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help")
    {
        printHelp();
        return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    try
    {
        const std::string command = argv[1];
        const Args args(argc, argv, 2);

        if (command == "gen-data")
            return genData(args);
        if (command == "supervised")
            return supervised(args);
        if (command == "selfplay")
            return selfPlay(args);
        if (command == "inspect")
            return inspect(args);
        if (command == "dataset-info")
            return datasetInfo(args);
        if (command == "merge-data")
            return mergeData(args);
        if (command == "eval")
            return evalModel(args);
        if (command == "agent-accuracy")
            return agentAccuracy(args);
        if (command == "export-vectors")
            return exportVectors(args);

        std::cerr << "Unknown command '" << command << "'\n\n";
        printHelp();
        return EXIT_FAILURE;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
