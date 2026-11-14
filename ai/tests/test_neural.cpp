// Tests of the neural agents, the encodings, the datasets and the training code

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ai/agents/neural_agent.hpp"
#include "ai/agents/random_agent.hpp"
#include "ai/alphazero.hpp"
#include "ai/encoding.hpp"
#include "ai/factory.hpp"
#include "ai/model_cache.hpp"
#include "ai/solver.hpp"
#include "ai/training/trainer.hpp"
#include "test_util.hpp"

using namespace ai;
using namespace ai::training;

namespace
{
    std::string tempPath(const std::string &name)
    {
        return (std::filesystem::temp_directory_path() / ("connectx_test_" + name)).string();
    }

    Position randomPosition(int rows, int cols, int connect, int stones, Rng &rng)
    {
        for (;;)
        {
            Position p(rows, cols, connect);
            while (p.moves() < stones && !p.isTerminal())
            {
                const std::vector<int> moves = p.legalMoves();
                p.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }
            if (!p.isTerminal())
                return p;
        }
    }

    std::shared_ptr<nn::PolicyValueNet> randomNetwork(Encoding encoding, std::vector<size_t> hidden = {32, 32}, uint64_t seed = 3)
    {
        return std::make_shared<nn::PolicyValueNet>(makeNetworkConfig(6, 7, 4, hidden, encoding), seed);
    }

    // x has columns 1-3 of the bottom row: x to move wins in column 4 (index 3)
    Position canWin() { return Position::fromMoves(6, 7, 4, "112233"); }
    // x threatens column 4 on the bottom row and o (to move) cannot win: o must play column 4 (index 3)
    Position mustBlock() { return Position::fromMoves(6, 7, 4, "17273"); }
}

void test_encodings()
{
    const Geometry &g = Geometry::get(6, 7, 4);
    CHECK_EQ(encodedSize(g, Encoding::RAW), 84);
    CHECK_EQ(encodedSize(g, Encoding::TACTICAL), 210);
    CHECK_EQ(encodedSize(g, Encoding::WINDOWS), 84 + 2 * 69);

    CHECK(parseEncoding("tactical") == Encoding::TACTICAL);
    bool threw = false;
    try
    {
        parseEncoding("nonsense");
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    CHECK(threw);

    Rng rng(5);
    for (int round = 0; round < 60; ++round)
    {
        const Position p = randomPosition(6, 7, 4, static_cast<int>(rng.below(30)), rng);

        // RAW: identical to Position::encode
        std::vector<float> raw(84), reference(84);
        encodePosition(p, Encoding::RAW, raw.data());
        p.encode(reference.data());
        CHECK(raw == reference);

        // TACTICAL: the first two planes are the raw ones; the others describe threats and playable cells
        std::vector<float> tactical(210);
        encodePosition(p, Encoding::TACTICAL, tactical.data());
        for (int r = 0; r < 6; ++r)
        {
            for (int c = 0; c < 7; ++c)
            {
                const float *cell = &tactical[(r * 7 + c) * 5];
                CHECK((cell[0] == 1.0f) == (p.cell(r, c) == 1));
                CHECK((cell[1] == 1.0f) == (p.cell(r, c) == 2));

                // A threat cell is an empty cell where that player would complete a line
                if (p.cell(r, c) != 0)
                {
                    CHECK(cell[2] == 0.0f && cell[3] == 0.0f && cell[4] == 0.0f);
                }
            }
        }

        // The playable plane has exactly one cell per non-full column
        int playable = 0, my_threats = 0, their_threats = 0;
        for (int i = 0; i < 42; ++i)
        {
            my_threats += tactical[i * 5 + 2] == 1.0f;
            their_threats += tactical[i * 5 + 3] == 1.0f;
            playable += tactical[i * 5 + 4] == 1.0f;
        }
        CHECK_EQ(playable, p.legalMoveCount());
        CHECK_EQ(my_threats, popcount(p.currentThreats()));
        CHECK_EQ(their_threats, popcount(p.opponentThreats()));

        // Mirroring the position mirrors every plane (RAW and TACTICAL are column-equivariant)
        for (Encoding encoding : {Encoding::RAW, Encoding::TACTICAL})
        {
            const int planes = encoding == Encoding::RAW ? 2 : 5;
            std::vector<float> a(planes * 42), b(planes * 42);
            encodePosition(p, encoding, a.data());
            encodePosition(p.mirrored(), encoding, b.data());
            for (int r = 0; r < 6; ++r)
                for (int c = 0; c < 7; ++c)
                    for (int plane = 0; plane < planes; ++plane)
                        CHECK_EQ(a[(r * 7 + c) * planes + plane], b[(r * 7 + (6 - c)) * planes + plane]);
        }
    }

    // WINDOWS: the line features are shares of K in [0, 1]; the opening position has none
    std::vector<float> windows(84 + 138);
    encodePosition(Position(6, 7, 4), Encoding::WINDOWS, windows.data());
    for (float v : windows)
        CHECK(v == 0.0f);

    Position p = Position::fromMoves(6, 7, 4, "4");
    encodePosition(p, Encoding::WINDOWS, windows.data());
    float max_share = 0;
    for (size_t i = 84; i < windows.size(); ++i)
        max_share = std::max(max_share, windows[i]);
    CHECK_NEAR(max_share, 0.25, 1e-6); // one stone in a window of four
}

void test_targets()
{
    LabeledPosition item;
    item.scores.fill(kIllegalScore);
    // columns 0..4 legal: 2 winning (scores 3 and 1), 1 drawing, 2 losing
    item.scores[0] = -2;
    item.scores[1] = 3;
    item.scores[2] = 0;
    item.scores[3] = 1;
    item.scores[4] = -1;
    item.value = 1;

    // BEST: only the fastest win
    Targets best = makeTargets(item, 7, PolicyTarget::BEST);
    CHECK_NEAR(best.policy[1], 1.0, 1e-6);
    CHECK_EQ(best.policy[3], 0.0f);

    // OUTCOME: every winning move
    Targets outcome = makeTargets(item, 7, PolicyTarget::OUTCOME);
    CHECK_NEAR(outcome.policy[1], 0.5, 1e-6);
    CHECK_NEAR(outcome.policy[3], 0.5, 1e-6);
    CHECK_EQ(outcome.policy[2], 0.0f);
    CHECK_EQ(outcome.optimal_mask, (1u << 1) | (1u << 3));
    CHECK_EQ(outcome.value, 1.0f);

    // SOFT: a proper distribution over the legal columns, ordered like the scores, nothing on illegal ones
    Targets soft = makeTargets(item, 7, PolicyTarget::SOFT, 1.0);
    float sum = 0;
    for (int c = 0; c < 7; ++c)
        sum += soft.policy[c];
    CHECK_NEAR(sum, 1.0, 1e-5);
    CHECK(soft.policy[1] > soft.policy[3] && soft.policy[3] > soft.policy[2] && soft.policy[2] > soft.policy[4] && soft.policy[4] > soft.policy[0]);
    CHECK_EQ(soft.policy[5], 0.0f);

    // A drawn position: the drawing moves are the optimal ones
    LabeledPosition draw;
    draw.scores.fill(kIllegalScore);
    draw.scores[2] = 0;
    draw.scores[3] = 0;
    draw.scores[4] = -1;
    draw.value = 0;
    Targets drawn = makeTargets(draw, 7, PolicyTarget::OUTCOME);
    CHECK_EQ(drawn.optimal_mask, (1u << 2) | (1u << 3));

    // A lost position: the optimal moves are the slowest defeats
    LabeledPosition lost;
    lost.scores.fill(kIllegalScore);
    lost.scores[0] = -3;
    lost.scores[1] = -1;
    lost.scores[2] = -1;
    lost.value = -1;
    Targets loss = makeTargets(lost, 7, PolicyTarget::OUTCOME);
    CHECK_EQ(loss.optimal_mask, (1u << 1) | (1u << 2));
}

void test_dataset_generation_and_io()
{
    GenerateOptions options;
    options.count = 60;
    options.threads = 2;
    options.seed = 7;
    options.min_stones = 26; // late positions only: the solver answers instantly
    options.player_depth = 2;
    options.weak = false;
    const Dataset dataset = generateDataset(options);
    CHECK_EQ(dataset.size(), static_cast<size_t>(60));

    Solver solver(1 << 18);
    std::vector<uint64_t> keys;
    for (const LabeledPosition &item : dataset.items())
    {
        const Position p = Position::fromBitboards(6, 7, 4, item.current, item.mask);
        CHECK(!p.isTerminal());
        CHECK(p.moves() >= 26);
        keys.push_back(p.key());

        // The labels are the solver's scores
        std::array<int, kMaxCols> scores;
        CHECK(solver.scoreMoves(p, scores));
        int best = -1000;
        for (int c = 0; c < 7; ++c)
        {
            if (scores[c] == Solver::kIllegal)
                CHECK_EQ(item.scores[c], kIllegalScore);
            else
            {
                CHECK_EQ(static_cast<int>(item.scores[c]), scores[c]);
                best = std::max(best, scores[c]);
            }
        }
        CHECK_EQ(static_cast<int>(item.value), (best > 0) - (best < 0));
    }

    // Positions are unique
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());

    // Save / load
    const std::string path = tempPath("dataset.cxds");
    dataset.save(path);
    const Dataset loaded = Dataset::load(path);
    CHECK_EQ(loaded.size(), dataset.size());
    CHECK_EQ(loaded.rows(), 6);
    CHECK_EQ(loaded.cols(), 7);
    CHECK_EQ(loaded.connect(), 4);
    for (size_t i = 0; i < dataset.size(); ++i)
    {
        CHECK(loaded[i].current == dataset[i].current);
        CHECK(loaded[i].mask == dataset[i].mask);
        CHECK(loaded[i].scores == dataset[i].scores);
        CHECK_EQ(loaded[i].value, dataset[i].value);
    }
    std::filesystem::remove(path);

    bool threw = false;
    try
    {
        Dataset::load(tempPath("does_not_exist.cxds"));
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    CHECK(threw);
}

void test_neural_agents()
{
    Rng rng(9);

    for (Encoding encoding : {Encoding::RAW, Encoding::TACTICAL, Encoding::WINDOWS})
    {
        const auto network = randomNetwork(encoding);

        for (bool mirror : {false, true})
        {
            NeuralAgent::Options options;
            options.mirror = mirror;
            NeuralAgent agent(network, options, 1);

            for (int round = 0; round < 30; ++round)
            {
                const Position p = randomPosition(6, 7, 4, static_cast<int>(rng.below(36)), rng);
                const MoveChoice choice = agent.chooseMove(p);
                CHECK(p.canPlay(choice.column));
                CHECK(choice.value >= -1.0 && choice.value <= 1.0);
            }
        }

        // With the tactics switch on, wins are taken and threats blocked whatever the network thinks
        NeuralAgent::Options tactical;
        tactical.tactics = true;
        NeuralAgent agent(network, tactical, 1);
        CHECK_EQ(agent.chooseMove(canWin()).column, 3);
        CHECK_EQ(agent.chooseMove(mustBlock()).column, 3);

        // Sampling with a temperature still gives legal moves
        NeuralAgent::Options sampled;
        sampled.temperature = 1.0;
        NeuralAgent sampler(network, sampled, 5);
        for (int round = 0; round < 30; ++round)
        {
            const Position p = randomPosition(6, 7, 4, static_cast<int>(rng.below(36)), rng);
            CHECK(p.canPlay(sampler.chooseMove(p).column));
        }
    }

    // A network for another board is refused with an explanation
    NeuralAgent agent(randomNetwork(Encoding::RAW), 1);
    bool threw = false;
    try
    {
        agent.chooseMove(Position(5, 6, 4));
    }
    catch (const std::invalid_argument &)
    {
        threw = true;
    }
    CHECK(threw);
}

void test_alphazero_search()
{
    const auto network = randomNetwork(Encoding::TACTICAL);

    // Even with an untrained network the tree search finds forced tactics: wins...
    {
        AlphaZeroSearch::Options options;
        options.simulations = 200;
        AlphaZeroSearch search(network, options, 1);
        const AlphaZeroSearch::Result result = search.search(canWin());

        int best = 0;
        for (int c = 0; c < 7; ++c)
            if (result.visits[c] > result.visits[best])
                best = c;
        CHECK_EQ(best, 3);
        CHECK(result.value > 0.5);
        CHECK(result.evaluations > 10);

        int total = 0;
        for (int v : result.visits)
            total += v;
        CHECK_EQ(total, 199); // every simulation but the first (which expands the root) visits a child
    }

    // ...and forced blocks
    {
        AlphaZeroAgent::Options options;
        options.simulations = 400;
        AlphaZeroAgent agent(network, options, 2);
        CHECK_EQ(agent.chooseMove(mustBlock()).column, 3);
    }

    // The root noise changes the priors but the answer stays legal; the time limit is honoured
    {
        AlphaZeroSearch::Options options;
        options.simulations = 100000;
        options.dirichlet_fraction = 0.25;
        AlphaZeroSearch search(network, options, 3);
        const auto start = std::chrono::steady_clock::now();
        const AlphaZeroSearch::Result result = search.search(Position(6, 7, 4), 0.05);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK(seconds < 1.0);
        CHECK(result.evaluations < 100000);
        int total = 0;
        for (int v : result.visits)
            total += v;
        CHECK(total > 10);
    }
}

void test_self_play_game()
{
    SelfPlayOptions options;
    options.simulations = 20;
    options.dirichlet_fraction = 0.25;
    options.temperature_plies = 6;

    const auto network = randomNetwork(Encoding::RAW);
    AlphaZeroSearch::Options search_options;
    search_options.simulations = options.simulations;
    search_options.dirichlet_fraction = options.dirichlet_fraction;
    AlphaZeroSearch search(network, search_options, 11);
    Rng rng(12);

    int decisive = 0;
    for (int game_index = 0; game_index < 12; ++game_index)
    {
        const SelfPlayGame game = playSelfPlayGame(search, options, rng);
        CHECK_EQ(static_cast<int>(game.examples.size()), game.plies);
        CHECK(game.plies >= 7 && game.plies <= 42);

        for (size_t i = 0; i < game.examples.size(); ++i)
        {
            const Example &example = game.examples[i];

            // The policy target is a probability distribution over the legal columns
            const Position p = Position::fromBitboards(6, 7, 4, example.current, example.mask);
            CHECK_EQ(p.moves(), static_cast<int>(i));
            float sum = 0;
            for (int c = 0; c < 7; ++c)
            {
                sum += example.policy[c];
                if (!p.canPlay(c))
                    CHECK_EQ(example.policy[c], 0.0f);
            }
            CHECK_NEAR(sum, 1.0, 1e-4);

            // Values alternate between the two players, the last position's player to move wins
            if (game.result != 0 && i + 1 < game.examples.size())
                CHECK_EQ(example.value, -game.examples[i + 1].value);
        }

        if (game.result != 0)
        {
            ++decisive;
            CHECK_EQ(game.examples.back().value, 1.0f);
            // result +1 = the first player won: he moved at the even indices, so the last position (the winning
            // move) is at an even index exactly when the number of positions is odd
            CHECK_EQ(game.result, game.plies % 2 == 1 ? 1 : -1);
        }
        else
        {
            for (const Example &example : game.examples)
                CHECK_EQ(example.value, 0.0f);
        }
    }
    CHECK(decisive > 0);
}

void test_supervised_learning_and_self_play_run()
{
    // Late-game positions (instant to label): a small network must beat random guessing clearly
    GenerateOptions generation;
    generation.count = 1500;
    generation.threads = 2;
    generation.seed = 21;
    generation.min_stones = 24;
    generation.player_depth = 2;
    const Dataset dataset = generateDataset(generation);

    SupervisedOptions options;
    options.hidden = {48, 48};
    options.encoding = Encoding::TACTICAL;
    options.epochs = 12;
    options.batch_size = 64;
    options.learning_rate = 0.003;
    options.validation_fraction = 0.2;
    options.output_model = tempPath("sup.cxnn");
    options.metadata = "unit test";

    SupervisedReport report;
    std::vector<std::string> lines;
    const auto network = trainSupervised(dataset, options, report, [&](const std::string &line)
                                         { lines.push_back(line); });
    CHECK(!lines.empty());
    CHECK(report.best_epoch >= 1);

    // Learning happened: against the untrained network (same initial weights) both losses went down a lot
    const std::vector<Example> examples = makeExamples(dataset, PolicyTarget::OUTCOME);
    const nn::PolicyValueNet untrained(makeNetworkConfig(6, 7, 4, options.hidden, options.encoding), options.seed);
    const EvalStats before = evaluate(untrained, examples, 6, 7, 4);
    const EvalStats after = evaluate(*network, examples, 6, 7, 4);
    CHECK_MSG(after.value_mse < 0.7 * before.value_mse, "value mse " << before.value_mse << " -> " << after.value_mse);
    CHECK_MSG(after.policy_loss < before.policy_loss, "policy loss " << before.policy_loss << " -> " << after.policy_loss);
    CHECK(after.outcome_accuracy > before.outcome_accuracy);

    // The saved model can be loaded back and used through the agent factory
    const auto loaded = loadSharedModel(options.output_model);
    CHECK_EQ(loaded->config().encoding, static_cast<uint32_t>(Encoding::TACTICAL));
    const AgentPtr agent = createAgent("nn:model=" + options.output_model + ",tactics=1");
    CHECK(agent->chooseMove(canWin()).column == 3);
    const AgentPtr az = createAgent("az:model=" + options.output_model + ",sims=600,seed=1");
    CHECK(az->chooseMove(mustBlock()).column == 3);
    const AgentPtr search = createAgent("nnsearch:model=" + options.output_model + ",depth=2");
    CHECK(search->chooseMove(canWin()).column == 3);
    CHECK(search->chooseMove(mustBlock()).column == 3);

    // A tiny self-play run completes, writes a checkpoint and keeps the board metadata
    SelfPlayOptions self_play;
    self_play.hidden = {16, 16};
    self_play.iterations = 2;
    self_play.games_per_iteration = 4;
    self_play.simulations = 10;
    self_play.threads = 2;
    self_play.train_steps_per_iteration = 5;
    self_play.batch_size = 16;
    self_play.eval_every = 2;
    self_play.eval_games = 2;
    self_play.eval_simulations = 10;
    self_play.eval_opponents = {"random"};
    self_play.output_dir = tempPath("selfplay");
    self_play.teacher = &dataset;
    std::vector<std::string> log;
    const auto trained = runSelfPlay(self_play, [&](const std::string &line)
                                     { log.push_back(line); });
    CHECK(trained != nullptr);
    CHECK(log.size() >= 3);
    CHECK(std::filesystem::exists(self_play.output_dir + "/latest.cxnn"));
    CHECK_EQ(nn::PolicyValueNet::load(self_play.output_dir + "/latest.cxnn").config().board_cols, 7u);

    std::filesystem::remove(options.output_model);
    std::filesystem::remove_all(self_play.output_dir);
}

int main()
{
    RUN_TEST(test_encodings);
    RUN_TEST(test_targets);
    RUN_TEST(test_dataset_generation_and_io);
    RUN_TEST(test_neural_agents);
    RUN_TEST(test_alphazero_search);
    RUN_TEST(test_self_play_game);
    RUN_TEST(test_supervised_learning_and_self_play_run);
    return testSummary("test_neural");
}
