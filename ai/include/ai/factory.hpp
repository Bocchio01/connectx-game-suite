#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "ai/agent.hpp"

namespace ai
{
    /**
     * A textual description of an agent: "type" or "type:key=value,key=value".
     * Examples: "random", "alphabeta:depth=8,time=500", "mcts:sims=5000,rollout=tactical",
     * "az:model=models/connectx_6x7.cxnn,sims=400".
     */
    struct AgentSpec
    {
        std::string type;
        std::map<std::string, std::string> params;

        /** @throws std::invalid_argument on malformed text */
        static AgentSpec parse(const std::string &text);

        bool has(const std::string &key) const { return params.count(key) != 0; }
        int getInt(const std::string &key, int fallback) const;
        double getDouble(const std::string &key, double fallback) const;
        bool getBool(const std::string &key, bool fallback) const;
        std::string getString(const std::string &key, const std::string &fallback = "") const;

        /** @throws std::invalid_argument if a parameter is not in `known` */
        void requireKnown(std::initializer_list<const char *> known) const;
    };

    /** Description of an agent type, for help messages and GUIs. */
    struct AgentInfo
    {
        std::string type;       // e.g. "alphabeta"
        std::string summary;    // one line
        std::string parameters; // e.g. "depth=8 time=0 ..."
        std::string example;    // a ready-to-use spec
    };

    /**
     * Creates an agent from its textual description.
     * Every agent also accepts "seed=<n>" to make its random choices reproducible.
     * @throws std::invalid_argument for unknown types or parameters, std::runtime_error if a model cannot be loaded
     */
    AgentPtr createAgent(const std::string &spec);

    /** The agent types createAgent() knows, from the simplest to the strongest. */
    std::vector<AgentInfo> availableAgents();
}
