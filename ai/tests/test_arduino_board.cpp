// The Arduino sketch's portable engine (ai/arduino/ConnectXNet/connectx_board.h) must behave exactly like the
// reference engine and like the encoders used for training: otherwise the network on the microcontroller
// would receive inputs it was never trained on.

#include <vector>

#include "ai/agents/neural_agent.hpp"
#include "ai/agents/neural_search_agent.hpp"
#include "ai/encoding.hpp"
#include "ai/rng.hpp"
#include "ai/training/trainer.hpp"
#include "connectx_board.h"
#include "test_util.hpp"

using namespace ai;

namespace
{
    static_assert(connectx::ROWS == 6 && connectx::COLS == 7 && connectx::CONNECT == 4, "the sketch defaults to the standard board");
    static_assert(connectx::encodedSize(connectx::ENCODING_RAW) == 84, "raw size");
    static_assert(connectx::encodedSize(connectx::ENCODING_TACTICAL) == 210, "tactical size");
    static_assert(connectx::encodedSize(connectx::ENCODING_WINDOWS) == 84 + 2 * 69, "windows size");

    /** Plays the same random game on both engines, calling `check` on every position. */
    template <typename Check>
    void forRandomPositions(int games, uint64_t seed, Check check)
    {
        Rng rng(seed);
        for (int game = 0; game < games; ++game)
        {
            connectx::Board board;
            Position position(6, 7, 4);

            while (!position.isTerminal())
            {
                check(board, position);

                const std::vector<int> moves = position.legalMoves();
                const int column = moves[rng.below(static_cast<uint32_t>(moves.size()))];
                const bool predicted = board.winsByPlaying(column);
                const bool won_board = board.play(column);
                const bool won_position = position.play(column);
                CHECK_EQ(won_board, won_position);
                CHECK_EQ(predicted, won_position);
            }
            check(board, position);
        }
    }
}

void test_game_logic_matches_the_reference_engine()
{
    forRandomPositions(300, 11, [](const connectx::Board &board, const Position &position)
                       {
        CHECK_EQ(static_cast<int>(board.moves), position.moves());
        CHECK_EQ(board.isOver(), position.isTerminal());
        CHECK_EQ(board.winner != 0, position.lastMoveWon());
        CHECK_EQ(board.isFull(), position.moves() == 42);

        for (int c = 0; c < 7; ++c)
        {
            CHECK_EQ(board.canPlay(c), position.canPlay(c));
            if (!board.isOver())
                CHECK_EQ(board.winsByPlaying(c), position.canPlay(c) && position.isWinningMove(c));
        }

        // The relative view agrees cell by cell
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 7; ++c)
                CHECK_EQ(static_cast<int>(board.relative(r, c)), position.cell(r, c)); });
}

void test_encoders_match_the_training_encoders()
{
    const struct
    {
        int sketch;
        Encoding training;
    } encodings[] = {{connectx::ENCODING_RAW, Encoding::RAW},
                     {connectx::ENCODING_TACTICAL, Encoding::TACTICAL},
                     {connectx::ENCODING_WINDOWS, Encoding::WINDOWS}};

    for (const auto &pair : encodings)
    {
        const int size = connectx::encodedSize(pair.sketch);
        CHECK_EQ(size, encodedSize(Geometry::get(6, 7, 4), pair.training));

        std::vector<float> from_sketch(static_cast<size_t>(size)), from_training(static_cast<size_t>(size));
        forRandomPositions(60, 21 + pair.sketch, [&](const connectx::Board &board, const Position &position)
                           {
            if (position.isTerminal())
                return;
            connectx::encodeBoard(board, pair.sketch, from_sketch.data());
            encodePosition(position, pair.training, from_training.data());
            CHECK(from_sketch == from_training); });
    }
}

void test_move_choice_matches_the_neural_agent()
{
    auto network = std::make_shared<nn::PolicyValueNet>(training::makeNetworkConfig(6, 7, 4, {24, 24}, Encoding::TACTICAL), 5);

    NeuralAgent plain(network, 1);
    NeuralAgent::Options shield_options;
    shield_options.tactics = true;
    NeuralAgent shielded(network, shield_options, 1);

    std::vector<float> input(210), logits(7);
    forRandomPositions(150, 33, [&](const connectx::Board &board, const Position &position)
                       {
        if (position.isTerminal())
            return;

        connectx::encodeBoard(board, connectx::ENCODING_TACTICAL, input.data());
        nn::Real value;
        network->predict(input.data(), logits.data(), &value);

        CHECK_EQ(connectx::chooseColumn(board, logits.data(), false), plain.chooseMove(position).column);
        CHECK_EQ(connectx::chooseColumn(board, logits.data(), true), shielded.chooseMove(position).column); });

    // The shield on hand-made positions: a win, then a block
    connectx::Board win;
    for (int c : {0, 0, 1, 1, 2, 2})
        win.play(c);
    CHECK_EQ(connectx::tacticalColumn(win), 3);

    connectx::Board block;
    for (int c : {0, 6, 1, 6, 2})
        block.play(c);
    CHECK_EQ(connectx::tacticalColumn(block), 3);

    CHECK_EQ(connectx::tacticalColumn(connectx::Board()), -1);
}


void test_value_search_matches_the_neural_search_agent()
{
    auto network = std::make_shared<nn::PolicyValueNet>(training::makeNetworkConfig(6, 7, 4, {24, 24}, Encoding::TACTICAL), 8);

    // The evaluation function of the sketch: the value head of the (float) network
    struct Evaluator
    {
        const nn::PolicyValueNet &network;
        std::vector<float> input = std::vector<float>(210);
        std::vector<float> logits = std::vector<float>(7);
        int calls = 0;

        float operator()(const connectx::Board &board)
        {
            connectx::encodeBoard(board, connectx::ENCODING_TACTICAL, input.data());
            nn::Real value;
            network.predict(input.data(), logits.data(), &value);
            ++calls;
            return value;
        }
    };

    for (int depth = 1; depth <= 3; ++depth)
    {
        NeuralSearchAgent::Options options;
        options.depth = depth;
        NeuralSearchAgent agent(network, options, 1);

        int compared = 0;
        forRandomPositions(depth == 3 ? 12 : 25, 40 + depth, [&](const connectx::Board &board, const Position &position)
                           {
            if (position.isTerminal() || (position.moves() % 3) != 0)
                return; // (a sample of the positions of every game)

            Evaluator evaluator{*network};
            float scores[7];
            const int column = connectx::searchColumn(board, depth, evaluator, scores);

            const MoveChoice choice = agent.chooseMove(position);
            CHECK_EQ(column, choice.column);
            for (int c = 0; c < 7; ++c)
            {
                if (position.canPlay(c))
                    CHECK_NEAR(scores[c], choice.scores[static_cast<size_t>(c)], 1e-5);
            }
            ++compared; });
        CHECK(compared > 20);
    }
}

int main()
{
    RUN_TEST(test_game_logic_matches_the_reference_engine);
    RUN_TEST(test_encoders_match_the_training_encoders);
    RUN_TEST(test_move_choice_matches_the_neural_agent);
    RUN_TEST(test_value_search_matches_the_neural_search_agent);
    return testSummary("test_arduino_board");
}
