#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "ai/agents/alphabeta_agent.hpp"
#include "ai/agents/greedy_agent.hpp"
#include "ai/agents/mcts_agent.hpp"
#include "ai/agents/minimax_agent.hpp"
#include "ai/agents/neural_agent.hpp"
#include "ai/agents/neural_search_agent.hpp"
#include "ai/agents/random_agent.hpp"
#include "ai/agents/solver_agent.hpp"
#include "ai/alphazero.hpp"
#include "ai/factory.hpp"
#include "ai/model_cache.hpp"

namespace ai
{
    namespace
    {
        std::string trim(const std::string &text)
        {
            size_t begin = 0;
            size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    }

    // ========================================================================
    // AgentSpec
    // ========================================================================

    AgentSpec AgentSpec::parse(const std::string &text)
    {
        AgentSpec spec;

        const size_t colon = text.find(':');
        spec.type = lower(trim(text.substr(0, colon)));
        if (spec.type.empty())
            throw std::invalid_argument("Empty agent specification");

        if (colon == std::string::npos)
            return spec;

        std::stringstream stream(text.substr(colon + 1));
        std::string item;
        while (std::getline(stream, item, ','))
        {
            item = trim(item);
            if (item.empty())
                continue;

            const size_t equals = item.find('=');
            if (equals == std::string::npos)
                throw std::invalid_argument("Expected key=value, got '" + item + "' in agent '" + spec.type + "'");

            // (values keep their case: they can be file names)
            spec.params[lower(trim(item.substr(0, equals)))] = trim(item.substr(equals + 1));
        }

        return spec;
    }

    int AgentSpec::getInt(const std::string &key, int fallback) const
    {
        auto it = params.find(key);
        if (it == params.end())
            return fallback;

        try
        {
            size_t used = 0;
            const int value = std::stoi(it->second, &used);
            if (used != it->second.size())
                throw std::invalid_argument("trailing characters");
            return value;
        }
        catch (const std::exception &)
        {
            throw std::invalid_argument("Parameter '" + key + "' of agent '" + type + "' must be an integer, got '" + it->second + "'");
        }
    }

    double AgentSpec::getDouble(const std::string &key, double fallback) const
    {
        auto it = params.find(key);
        if (it == params.end())
            return fallback;

        try
        {
            size_t used = 0;
            const double value = std::stod(it->second, &used);
            if (used != it->second.size())
                throw std::invalid_argument("trailing characters");
            return value;
        }
        catch (const std::exception &)
        {
            throw std::invalid_argument("Parameter '" + key + "' of agent '" + type + "' must be a number, got '" + it->second + "'");
        }
    }

    bool AgentSpec::getBool(const std::string &key, bool fallback) const
    {
        auto it = params.find(key);
        if (it == params.end())
            return fallback;

        const std::string value = lower(it->second);
        if (value == "1" || value == "true" || value == "yes" || value == "on")
            return true;
        if (value == "0" || value == "false" || value == "no" || value == "off")
            return false;
        throw std::invalid_argument("Parameter '" + key + "' of agent '" + type + "' must be 0 or 1, got '" + it->second + "'");
    }

    std::string AgentSpec::getString(const std::string &key, const std::string &fallback) const
    {
        auto it = params.find(key);
        return it == params.end() ? fallback : it->second;
    }

    void AgentSpec::requireKnown(std::initializer_list<const char *> known) const
    {
        for (const auto &entry : params)
        {
            bool found = entry.first == "seed";
            for (const char *name : known)
                found = found || entry.first == name;

            if (!found)
            {
                std::string list = "seed";
                for (const char *name : known)
                    list += std::string(", ") + name;
                throw std::invalid_argument("Unknown parameter '" + entry.first + "' for agent '" + type + "' (known: " + list + ")");
            }
        }
    }

    // ========================================================================
    // Factory
    // ========================================================================

    AgentPtr createAgent(const std::string &text)
    {
        const AgentSpec spec = AgentSpec::parse(text);
        const uint64_t seed = spec.has("seed") ? static_cast<uint64_t>(spec.getInt("seed", 0)) : randomSeed();

        if (spec.type == "random")
        {
            spec.requireKnown({});
            return AgentPtr(new RandomAgent(seed));
        }

        if (spec.type == "greedy")
        {
            spec.requireKnown({});
            return AgentPtr(new GreedyAgent(seed));
        }

        if (spec.type == "minimax")
        {
            spec.requireKnown({"depth"});
            return AgentPtr(new MinimaxAgent(spec.getInt("depth", 4), seed));
        }

        if (spec.type == "alphabeta" || spec.type == "ab")
        {
            spec.requireKnown({"depth", "time", "id", "tt", "tactics", "random"});
            AlphaBetaAgent::Options options;
            options.depth = spec.getInt("depth", 8);
            options.time_limit_ms = spec.getInt("time", 0);
            options.iterative = spec.getBool("id", false);
            options.use_table = spec.has("tt") ? (spec.getBool("tt", true) ? 1 : 0) : -1;
            options.tactics = spec.getBool("tactics", true);
            options.randomize = spec.getBool("random", true);
            return AgentPtr(new AlphaBetaAgent(options, seed));
        }

        if (spec.type == "solver" || spec.type == "perfect")
        {
            spec.requireKnown({"time", "weak", "table"});
            SolverAgent::Options options;
            options.time_limit_ms = spec.getInt("time", 0);
            options.weak = spec.getBool("weak", false);
            options.table_entries = static_cast<size_t>(spec.getInt("table", 1 << 22));
            return AgentPtr(new SolverAgent(options, seed));
        }

        if (spec.type == "mcts")
        {
            spec.requireKnown({"sims", "time", "c", "rollout"});
            MctsAgent::Options options;
            options.simulations = spec.getInt("sims", spec.has("time") ? 0 : 2000);
            options.time_limit_ms = spec.getInt("time", 0);
            options.exploration = spec.getDouble("c", options.exploration);

            const std::string rollout = lower(spec.getString("rollout", "random"));
            if (rollout == "random")
                options.rollout = MctsAgent::Rollout::RANDOM;
            else if (rollout == "tactical")
                options.rollout = MctsAgent::Rollout::TACTICAL;
            else
                throw std::invalid_argument("Parameter 'rollout' of agent 'mcts' must be 'random' or 'tactical'");

            return AgentPtr(new MctsAgent(options, seed));
        }

        if (spec.type == "nn")
        {
            spec.requireKnown({"model", "temp", "mirror", "tactics"});
            if (!spec.has("model"))
                throw std::invalid_argument("Agent 'nn' needs a model: nn:model=<file.cxnn>");

            NeuralAgent::Options options;
            options.temperature = spec.getDouble("temp", 0.0);
            options.mirror = spec.getBool("mirror", false);
            options.tactics = spec.getBool("tactics", false);
            return AgentPtr(new NeuralAgent(loadSharedModel(spec.getString("model")), options, seed));
        }

        if (spec.type == "nnsearch")
        {
            spec.requireKnown({"model", "depth", "random"});
            if (!spec.has("model"))
                throw std::invalid_argument("Agent 'nnsearch' needs a model: nnsearch:model=<file.cxnn>,depth=3");

            NeuralSearchAgent::Options options;
            options.depth = spec.getInt("depth", options.depth);
            options.randomize = spec.getBool("random", options.randomize);
            return AgentPtr(new NeuralSearchAgent(loadSharedModel(spec.getString("model")), options, seed));
        }

        if (spec.type == "az")
        {
            spec.requireKnown({"model", "sims", "c", "temp", "time"});
            if (!spec.has("model"))
                throw std::invalid_argument("Agent 'az' needs a model: az:model=<file.cxnn>,sims=200");

            AlphaZeroAgent::Options options;
            options.simulations = spec.getInt("sims", options.simulations);
            options.c_puct = spec.getDouble("c", options.c_puct);
            options.temperature = spec.getDouble("temp", 0.0);
            options.time_limit_ms = spec.getInt("time", 0);
            return AgentPtr(new AlphaZeroAgent(loadSharedModel(spec.getString("model")), options, seed));
        }

        throw std::invalid_argument("Unknown agent type '" + spec.type + "' (run with --list-agents to see the available ones)");
    }

    std::vector<AgentInfo> availableAgents()
    {
        return {
            {"random", "Plays a random legal move (the baseline)", "", "random"},
            {"greedy", "One-move lookahead: win, block, avoid blunders, prefer the centre", "", "greedy"},
            {"minimax", "Depth-limited minimax with a heuristic evaluation (no pruning)", "depth=4", "minimax:depth=5"},
            {"alphabeta", "Negamax + alpha-beta pruning; with time=<ms> iterative deepening and a transposition table",
             "depth=8 time=0 id=0 tt=auto tactics=1 random=1", "alphabeta:depth=10,time=1000"},
            {"solver", "Perfect play with the exact solver (optionally time-limited, then alpha-beta fallback)",
             "time=0 weak=0 table=4194304", "solver:time=3000"},
            {"mcts", "Monte Carlo tree search (UCT) with random or tactical rollouts",
             "sims=2000 time=0 c=1.414 rollout=random|tactical", "mcts:sims=5000,rollout=tactical"},
            {"nn", "The policy network alone, no search (one forward pass per move)",
             "model=<file.cxnn> temp=0 mirror=0 tactics=0", "nn:model=connectx_6x7.cxnn"},
            {"nnsearch", "Alpha-beta search that uses the value head of the network as its evaluation function",
             "model=<file.cxnn> depth=3 random=0", "nnsearch:model=connectx_6x7.cxnn,depth=3"},
            {"az", "AlphaZero style: policy/value network guiding a tree search (PUCT)",
             "model=<file.cxnn> sims=200 c=1.5 temp=0 time=0", "az:model=connectx_6x7.cxnn,sims=400"},
        };
    }
}
