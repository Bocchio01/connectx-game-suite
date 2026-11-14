#pragma once

#include <vector>

#include "ai/agent.hpp"

namespace ai
{
    /**
     * Level 5: Monte Carlo Tree Search with UCT (Kocsis & Szepesvari).
     *
     * No evaluation function and no game knowledge beyond the rules: the value
     * of a move is estimated by playing many games to the end. Every
     * simulation has four steps:
     *   1. selection   walk down the tree choosing the child with the best UCT
     *                  score (average result + an exploration bonus for rarely
     *                  visited moves);
     *   2. expansion   when a leaf that was already visited is reached, create
     *                  its children;
     *   3. simulation  play the game to the end from the new node with a cheap
     *                  rollout policy;
     *   4. backprop    add the result to every node of the path.
     * After the simulations the most visited move at the root is played.
     *
     * Two rollout policies are available: purely random moves (the textbook
     * version) or "tactical" moves (take an immediate win, avoid moves that
     * lose at once), which makes every simulation far more informative.
     */
    class MctsAgent : public Agent
    {
    public:
        enum class Rollout
        {
            RANDOM,
            TACTICAL
        };

        struct Options
        {
            /** Simulations per move (0 = limited only by `time_limit_ms`). */
            int simulations = 2000;

            /** If > 0, stop after this many milliseconds even if the simulations are not finished. */
            int time_limit_ms = 0;

            /** UCT exploration constant (sqrt(2) is the theoretical value for rewards in [0, 1]). */
            double exploration = 1.41421356237;

            Rollout rollout = Rollout::RANDOM;
        };

        MctsAgent();
        explicit MctsAgent(const Options &options, uint64_t seed = randomSeed());

        std::string name() const override;
        MoveChoice chooseMove(const Position &position) override;
        void seed(uint64_t value) override { rng_.reseed(value); }

    private:
        struct Node
        {
            int32_t first_child = -1; // children are stored contiguously
            int32_t visits = 0;
            double reward = 0.0;      // sum of rewards for the player who made the move leading here
            uint8_t move = 0;         // column played to reach this node
            uint8_t child_count = 0;
            uint8_t terminal = 0;     // 0 = not (known to be) terminal, 1 = won by the last mover, 2 = draw
        };

        void iterate(const Position &root);
        void expand(int32_t node, const Position &position);
        int32_t selectChild(const Node &parent) const;
        int rollout(Position position);

        Options options_;
        std::vector<Node> nodes_;
        std::vector<int32_t> path_;
        Rng rng_;
    };
}
