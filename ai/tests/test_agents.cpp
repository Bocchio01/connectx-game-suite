// Behavioural tests of the agents and of the agent factory

#include <memory>
#include <stdexcept>
#include <vector>

#include "ai/agents/greedy_agent.hpp"
#include "ai/agents/mcts_agent.hpp"
#include "ai/agents/random_agent.hpp"
#include "ai/agents/solver_agent.hpp"
#include "ai/arena.hpp"
#include "ai/factory.hpp"
#include "test_util.hpp"

using namespace ai;

namespace
{
    // x has columns 1-3 of the bottom row: x to move wins in column 4 (index 3)
    Position canWin() { return Position::fromMoves(6, 7, 4, "112233"); }
    // x threatens column 4 on the bottom row and o (to move) cannot win: o must play column 4 (index 3)
    Position mustBlock() { return Position::fromMoves(6, 7, 4, "17273"); }
    // o owns columns 1-3 of the second row: x must not play column 4 (index 3), o would win above it
    Position poisoned() { return Position::fromMoves(6, 7, 4, "13217273"); }

    /** A deterministic mid-game position: nobody has won, nobody can win at once. */
    Position midGame(int stones, uint64_t seed)
    {
        Rng rng(seed);
        for (;;)
        {
            Position p(6, 7, 4);
            while (p.moves() < stones && !p.isTerminal())
            {
                const std::vector<int> moves = p.legalMoves();
                p.play(moves[rng.below(static_cast<uint32_t>(moves.size()))]);
            }
            if (!p.isTerminal() && !p.canWinNext())
                return p;
        }
    }

    bool throwsInvalidArgument(const std::string &spec)
    {
        try
        {
            createAgent(spec);
        }
        catch (const std::invalid_argument &)
        {
            return true;
        }
        catch (...)
        {
        }
        return false;
    }
}

void test_spec_parsing()
{
    const AgentSpec plain = AgentSpec::parse("random");
    CHECK_EQ(plain.type, std::string("random"));
    CHECK(plain.params.empty());

    const AgentSpec spec = AgentSpec::parse("  AlphaBeta : depth=8 , time = 500,tt=0 ");
    CHECK_EQ(spec.type, std::string("alphabeta"));
    CHECK_EQ(spec.getInt("depth", 0), 8);
    CHECK_EQ(spec.getInt("time", 0), 500);
    CHECK(!spec.getBool("tt", true));
    CHECK_EQ(spec.getInt("missing", 42), 42);

    // Values keep their case (they can be file names)
    const AgentSpec model = AgentSpec::parse("nn:model=Models/Net.cxnn");
    CHECK_EQ(model.getString("model"), std::string("Models/Net.cxnn"));

    CHECK(throwsInvalidArgument(""));
    CHECK(throwsInvalidArgument("nonexistent"));
    CHECK(throwsInvalidArgument("minimax:depth=abc"));
    CHECK(throwsInvalidArgument("minimax:depht=4")); // typo in a parameter name
    CHECK(throwsInvalidArgument("greedy:x"));
    CHECK(throwsInvalidArgument("mcts:rollout=smart"));

    for (const AgentInfo &info : availableAgents())
    {
        if (info.example.find("model=") != std::string::npos)
            continue; // needs a model file: covered by test_neural

        const AgentPtr agent = createAgent(info.example);
        CHECK(agent != nullptr);
        CHECK(!agent->name().empty());
    }
}

void test_agents_always_play_legal_moves()
{
    struct Shape
    {
        int rows, cols, connect;
    };

    const std::vector<std::string> specs = {
        "random", "greedy", "minimax:depth=3", "alphabeta:depth=5", "alphabeta:depth=8,time=50",
        "mcts:sims=200", "mcts:sims=200,rollout=tactical", "solver:time=50"};

    for (const Shape &shape : {Shape{6, 7, 4}, Shape{5, 5, 4}, Shape{4, 4, 3}, Shape{7, 7, 5}, Shape{4, 7, 4}})
    {
        for (const std::string &spec : specs)
        {
            AgentPtr agent = createAgent(spec + (spec.find(':') == std::string::npos ? ":seed=7" : ",seed=7"));
            RandomAgent random(11);

            for (int game = 0; game < 3; ++game)
            {
                const bool agent_first = (game % 2 == 0);
                Position start(shape.rows, shape.cols, shape.connect);
                const GameResult result = agent_first ? playGame(*agent, random, start) : playGame(random, *agent, start);
                CHECK_MSG(!result.illegal_move, spec << " played an illegal move on " << shape.rows << "x" << shape.cols);

                // Nothing should lose to a random player on these tiny searches... except by bad luck on tiny boards,
                // so only check that a game really finished
                CHECK(result.plies > 0);
            }
        }
    }
}

void test_greedy_tactics()
{
    GreedyAgent greedy(5);
    for (int round = 0; round < 20; ++round)
    {
        CHECK_EQ(greedy.chooseMove(canWin()).column, 3);
        CHECK_EQ(greedy.chooseMove(mustBlock()).column, 3);
        CHECK(greedy.chooseMove(poisoned()).column != 3);
    }

    // On an empty board it plays the centre
    CHECK_EQ(greedy.chooseMove(Position(6, 7, 4)).column, 3);

    // On an even-width board it plays one of the two central columns
    const int col = greedy.chooseMove(Position(6, 8, 4)).column;
    CHECK(col == 3 || col == 4);
}

void test_mcts_tactics()
{
    for (MctsAgent::Rollout rollout : {MctsAgent::Rollout::RANDOM, MctsAgent::Rollout::TACTICAL})
    {
        MctsAgent::Options options;
        options.simulations = 3000;
        options.rollout = rollout;
        MctsAgent mcts(options, 17);

        CHECK_EQ(mcts.chooseMove(canWin()).column, 3);
        CHECK_EQ(mcts.chooseMove(mustBlock()).column, 3);
        CHECK(mcts.chooseMove(poisoned()).column != 3);
    }

    // The time limit is honoured
    MctsAgent::Options timed;
    timed.simulations = 0;
    timed.time_limit_ms = 50;
    MctsAgent mcts(timed, 3);
    const MoveChoice choice = mcts.chooseMove(Position(6, 7, 4));
    CHECK(choice.seconds < 0.5);
    CHECK(choice.nodes > 100);
}

void test_solver_agent_plays_the_proven_best_move()
{
    // A late-middle-game position: the solver agent must choose a move whose exact score is the best one
    Solver reference(1 << 20);
    SolverAgent agent(SolverAgent::Options(), 9);

    const Position p = midGame(20, 1234);
    CHECK(!p.isTerminal());

    std::array<int, kMaxCols> scores;
    CHECK(reference.scoreMoves(p, scores));
    int best = Solver::kIllegal;
    for (int col = 0; col < p.cols(); ++col)
        best = std::max(best, scores[col]);

    const MoveChoice choice = agent.chooseMove(p);
    CHECK_EQ(scores[choice.column], best);
    CHECK(choice.value == (best > 0 ? 1.0 : (best < 0 ? -1.0 : 0.0)));

    // Weak mode never plays a move that gives away the game-theoretic result
    SolverAgent::Options weak_options;
    weak_options.weak = true;
    SolverAgent weak(weak_options, 9);
    const MoveChoice weak_choice = weak.chooseMove(p);
    CHECK_EQ((scores[weak_choice.column] > 0) - (scores[weak_choice.column] < 0), (best > 0) - (best < 0));
}

void test_solver_agent_never_throws_away_the_result()
{
    // From a position with a known game-theoretic value, the perfect player must obtain at least
    // that value against any opponent (here a random one), whichever side it plays.
    for (uint64_t seed : {11, 12, 13, 14, 15, 16})
    {
        const Position start = midGame(18, seed);
        Solver reference(1 << 20);
        int score = 0;
        CHECK(reference.solve(start, score));

        SolverAgent winner(SolverAgent::Options(), 1);
        RandomAgent random(2);

        for (int game = 0; game < 5; ++game)
        {
            winner.seed(100 + game);
            random.seed(200 + game);

            // GameResult::outcome is +1 when the agent that moves first in `start` wins
            if (score > 0)
            {
                CHECK_EQ(playGame(winner, random, start).outcome, 1); // the side to move wins: solver plays it
            }
            else if (score < 0)
            {
                CHECK_EQ(playGame(random, winner, start).outcome, -1); // the side to move loses: solver is the other side
            }
            else
            {
                CHECK(playGame(winner, random, start).outcome >= 0); // a draw: the solver must not lose
                CHECK(playGame(random, winner, start).outcome <= 0);
            }
        }
    }
}

int main()
{
    RUN_TEST(test_spec_parsing);
    RUN_TEST(test_agents_always_play_legal_moves);
    RUN_TEST(test_greedy_tactics);
    RUN_TEST(test_mcts_tactics);
    RUN_TEST(test_solver_agent_plays_the_proven_best_move);
    RUN_TEST(test_solver_agent_never_throws_away_the_result);
    return testSummary("test_agents");
}
