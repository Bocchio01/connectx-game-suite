#include <algorithm>
#include <chrono>
#include <cmath>

#include "ai/agents/mcts_agent.hpp"

namespace ai
{
    MctsAgent::MctsAgent() : MctsAgent(Options())
    {
    }

    MctsAgent::MctsAgent(const Options &options, uint64_t seed)
        : options_(options),
          rng_(seed)
    {
        if (options_.simulations <= 0 && options_.time_limit_ms <= 0)
            options_.simulations = 2000;
    }

    std::string MctsAgent::name() const
    {
        std::string text = "mcts(";
        if (options_.simulations > 0)
            text += "sims=" + std::to_string(options_.simulations);
        if (options_.time_limit_ms > 0)
            text += std::string(options_.simulations > 0 ? "," : "") + "time=" + std::to_string(options_.time_limit_ms) + "ms";
        text += options_.rollout == Rollout::TACTICAL ? ",tactical" : ",random";
        return text + ")";
    }

    void MctsAgent::expand(int32_t node, const Position &position)
    {
        const Geometry &geometry = position.geometry();
        const int32_t first = static_cast<int32_t>(nodes_.size());
        int count = 0;

        for (int i = 0; i < geometry.cols; ++i)
        {
            const int col = geometry.move_order[i];
            if (!position.canPlay(col))
                continue;

            Node child;
            child.move = static_cast<uint8_t>(col);
            nodes_.push_back(child);
            ++count;
        }

        // (nodes_ may have been reallocated: access the parent by index only now)
        nodes_[node].first_child = first;
        nodes_[node].child_count = static_cast<uint8_t>(count);
    }

    int32_t MctsAgent::selectChild(const Node &parent) const
    {
        const double log_parent = std::log(static_cast<double>(parent.visits));

        int32_t best = parent.first_child;
        double best_value = -1.0;

        for (int i = 0; i < parent.child_count; ++i)
        {
            const int32_t index = parent.first_child + i;
            const Node &child = nodes_[index];

            // Every child is tried once before the statistics start to matter
            if (child.visits == 0)
                return index;

            // `reward` is measured for the player who moved into the child, i.e. the player
            // choosing among the children: he wants the highest average reward.
            const double exploitation = child.reward / child.visits;
            const double exploration = options_.exploration * std::sqrt(log_parent / child.visits);
            const double value = exploitation + exploration;

            if (value > best_value)
            {
                best_value = value;
                best = index;
            }
        }

        return best;
    }

    int MctsAgent::rollout(Position position)
    {
        // Result from the point of view of the player who is to move at the start: +1 / 0 / -1.
        // `sign` is +1 while the player to move is that player, -1 while it is the opponent.
        int sign = 1;

        for (;;)
        {
            const uint64_t legal = position.legalMoveMask();
            uint64_t cell;

            if (options_.rollout == Rollout::TACTICAL)
            {
                // Win if possible, otherwise play a move that does not lose at once (if any)
                const uint64_t wins = position.currentThreats() & legal;
                if (wins != 0)
                {
                    cell = wins & (~wins + 1);
                }
                else
                {
                    const uint64_t safe = position.nonLosingMoves();
                    cell = randomCell(safe != 0 ? safe : legal, rng_);
                }
            }
            else
            {
                cell = randomCell(legal, rng_);
            }

            position.playCell(cell);

            if (position.lastMoveWon())
                return sign; // the player who has just moved has won
            if (position.isDraw())
                return 0;

            sign = -sign;
        }
    }

    void MctsAgent::iterate(const Position &root)
    {
        Position position = root;
        path_.clear();
        path_.push_back(0);
        int32_t node = 0;

        // Outcome (+1 / 0 / -1) for the player to move in the position of the last visited node
        int result;

        for (;;)
        {
            if (nodes_[node].terminal != 0)
            {
                result = nodes_[node].terminal == 1 ? -1 : 0;
                break;
            }

            // 3. simulation: a node seen for the first time is evaluated with a rollout
            if (nodes_[node].visits == 0 && node != 0)
            {
                result = rollout(position);
                break;
            }

            // 2. expansion
            if (nodes_[node].first_child < 0)
                expand(node, position);

            // 1. selection
            const int32_t child = selectChild(nodes_[node]);
            position.play(nodes_[child].move);
            path_.push_back(child);
            node = child;

            if (position.isTerminal())
            {
                nodes_[node].terminal = position.lastMoveWon() ? 1 : 2;
                result = position.lastMoveWon() ? -1 : 0;
                break;
            }
        }

        // 4. backpropagation. `result` is for the player to move in the leaf, so the player who
        //    moved into the leaf gets the opposite; the reward then flips at every level.
        double reward = (1.0 - result) / 2.0;
        for (auto it = path_.rbegin(); it != path_.rend(); ++it)
        {
            Node &n = nodes_[*it];
            ++n.visits;
            n.reward += reward;
            reward = 1.0 - reward;
        }
    }

    MoveChoice MctsAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(options_.time_limit_ms);

        MoveChoice choice;
        choice.scores = emptyScores(position);

        // Nothing to think about when there is a single legal move
        if (position.legalMoveCount() == 1)
        {
            choice.column = columnOfCell(position, position.legalMoveMask());
            choice.info = "only move";
            return choice;
        }

        nodes_.clear();
        nodes_.reserve(static_cast<size_t>(std::max(options_.simulations, 1000)) * 2);
        nodes_.push_back(Node());

        int simulations = 0;
        while (options_.simulations <= 0 || simulations < options_.simulations)
        {
            if (options_.time_limit_ms > 0 && (simulations & 63) == 0 && std::chrono::steady_clock::now() >= deadline)
                break;

            iterate(position);
            ++simulations;
        }

        // Play the most visited move (the "robust child"): less noisy than the best average
        const Node &root = nodes_[0];
        int32_t best = root.first_child;
        for (int i = 0; i < root.child_count; ++i)
        {
            const Node &child = nodes_[root.first_child + i];
            const Node &current_best = nodes_[best];
            if (child.visits > current_best.visits ||
                (child.visits == current_best.visits && child.reward > current_best.reward))
                best = root.first_child + i;

            choice.scores[child.move] = root.visits > 0 ? static_cast<double>(child.visits) / root.visits : 0.0;
        }

        const Node &best_node = nodes_[best];
        choice.column = best_node.move;
        choice.nodes = static_cast<uint64_t>(simulations);
        choice.value = best_node.visits > 0 ? 2.0 * best_node.reward / best_node.visits - 1.0 : 0.0;
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }
}
