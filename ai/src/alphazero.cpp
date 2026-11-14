#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include "ai/agents/neural_agent.hpp"
#include "ai/alphazero.hpp"
#include "ai/encoding.hpp"
#include "nn/loss.hpp"

namespace ai
{
    // ========================================================================
    // AlphaZeroSearch
    // ========================================================================

    AlphaZeroSearch::AlphaZeroSearch(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed)
        : network_(std::move(network)),
          options_(options),
          evaluations_(0),
          rng_(seed)
    {
        options_.simulations = std::max(1, options_.simulations);
    }

    double AlphaZeroSearch::expand(int32_t node, const Position &position)
    {
        // Evaluate the position with the network
        const size_t columns = static_cast<size_t>(position.cols());
        input_.resize(network_->config().input_size);
        logits_.resize(columns);
        nn::Real value = 0;

        encodePosition(position, static_cast<Encoding>(network_->config().encoding), input_.data());
        network_->predict(input_.data(), logits_.data(), &value);
        ++evaluations_;

        // Priors: softmax of the logits restricted to the legal moves
        std::vector<unsigned char> legal(columns, 0);
        for (size_t col = 0; col < columns; ++col)
            legal[col] = position.canPlay(static_cast<int>(col)) ? 1 : 0;

        std::vector<nn::Real> priors(columns);
        nn::softmax(logits_.data(), columns, priors.data(), legal.data());

        const int32_t first = static_cast<int32_t>(nodes_.size());
        int count = 0;
        for (size_t col = 0; col < columns; ++col)
        {
            if (!legal[col])
                continue;

            Node child;
            child.move = static_cast<uint8_t>(col);
            child.prior = static_cast<float>(priors[col]);
            nodes_.push_back(child);
            ++count;
        }

        nodes_[node].first_child = first; // (nodes_ may have been reallocated above)
        nodes_[node].child_count = static_cast<uint8_t>(count);
        return static_cast<double>(value);
    }

    int32_t AlphaZeroSearch::selectChild(const Node &parent) const
    {
        const double sqrt_visits = std::sqrt(static_cast<double>(parent.visits));

        int32_t best = parent.first_child;
        double best_score = -1e30;

        for (int i = 0; i < parent.child_count; ++i)
        {
            const int32_t index = parent.first_child + i;
            const Node &child = nodes_[index];

            // Q: average outcome for the player choosing among the children (0 when never visited);
            // U: exploration bonus, large for moves with a high prior that were rarely tried
            const double q = child.visits > 0 ? child.value_sum / child.visits : 0.0;
            const double u = options_.c_puct * static_cast<double>(child.prior) * sqrt_visits / (1.0 + child.visits);
            const double score = q + u;

            if (score > best_score)
            {
                best_score = score;
                best = index;
            }
        }

        return best;
    }

    void AlphaZeroSearch::iterate(const Position &root, bool add_noise)
    {
        Position position = root;
        path_.clear();
        path_.push_back(0);
        int32_t node = 0;

        // Value of the last node of the path, for the player to move there
        double value = 0.0;

        for (;;)
        {
            Node &current = nodes_[node];

            if (current.terminal != 0)
            {
                value = current.terminal == 1 ? -1.0 : 0.0; // the player to move has just lost (or drawn)
                break;
            }

            if (current.first_child < 0)
            {
                // A leaf: ask the network instead of playing the game out
                value = expand(node, position);

                if (node == 0 && add_noise && options_.dirichlet_fraction > 0)
                {
                    // Mix Dirichlet noise into the root priors, so that moves the network
                    // currently dislikes still get tried now and then
                    Node &root_node = nodes_[0];
                    std::gamma_distribution<double> gamma(options_.dirichlet_alpha, 1.0);
                    std::vector<double> noise(root_node.child_count);
                    double total = 0;
                    for (double &n : noise)
                    {
                        n = gamma(rng_);
                        total += n;
                    }
                    for (int i = 0; i < root_node.child_count; ++i)
                    {
                        Node &child = nodes_[root_node.first_child + i];
                        child.prior = static_cast<float>((1.0 - options_.dirichlet_fraction) * child.prior +
                                                         options_.dirichlet_fraction * (total > 0 ? noise[i] / total : 1.0 / root_node.child_count));
                    }
                }
                break;
            }

            const int32_t child = selectChild(current);
            position.play(nodes_[child].move);
            path_.push_back(child);
            node = child;

            if (position.isTerminal())
                nodes_[node].terminal = position.lastMoveWon() ? 1 : 2;
        }

        // Backpropagation: the node's statistics are for the player who moved into it, i.e. the
        // opponent of the player to move there, so the sign flips at every level
        double reward = -value;
        for (auto it = path_.rbegin(); it != path_.rend(); ++it)
        {
            Node &n = nodes_[*it];
            ++n.visits;
            n.value_sum += reward;
            reward = -reward;
        }
    }

    AlphaZeroSearch::Result AlphaZeroSearch::search(const Position &root, double limit_seconds)
    {
        checkNetworkMatches(network_->config(), root);

        const auto start = std::chrono::steady_clock::now();
        nodes_.clear();
        nodes_.reserve(static_cast<size_t>(options_.simulations) * 3 + 16);
        nodes_.push_back(Node());
        evaluations_ = 0;

        for (int simulation = 0; simulation < options_.simulations; ++simulation)
        {
            if (limit_seconds > 0 && (simulation & 15) == 0 && simulation > 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= limit_seconds)
                break;

            iterate(root, simulation == 0);
        }

        Result result;
        result.evaluations = evaluations_;
        result.visits.assign(static_cast<size_t>(root.cols()), 0);
        result.values.assign(static_cast<size_t>(root.cols()), std::numeric_limits<double>::quiet_NaN());

        const Node &root_node = nodes_[0];
        for (int i = 0; i < root_node.child_count; ++i)
        {
            const Node &child = nodes_[root_node.first_child + i];
            result.visits[child.move] = child.visits;
            if (child.visits > 0)
                result.values[child.move] = child.value_sum / child.visits; // for the root player
        }

        result.value = root_node.visits > 0 ? -root_node.value_sum / root_node.visits : 0.0;
        return result;
    }

    // ========================================================================
    // AlphaZeroAgent
    // ========================================================================

    namespace
    {
        AlphaZeroSearch::Options searchOptions(const AlphaZeroAgent::Options &options)
        {
            AlphaZeroSearch::Options search;
            search.simulations = options.simulations;
            search.c_puct = options.c_puct;
            search.dirichlet_fraction = 0.0;
            return search;
        }
    }

    AlphaZeroAgent::AlphaZeroAgent(std::shared_ptr<const nn::PolicyValueNet> network, uint64_t seed)
        : AlphaZeroAgent(std::move(network), Options(), seed)
    {
    }

    AlphaZeroAgent::AlphaZeroAgent(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed)
        : options_(options),
          search_(std::move(network), searchOptions(options), seed),
          rng_(seed ^ 0xA5A5A5A5ULL)
    {
    }

    std::string AlphaZeroAgent::name() const
    {
        std::string text = "az(sims=" + std::to_string(options_.simulations);
        if (options_.time_limit_ms > 0)
            text += ",time=" + std::to_string(options_.time_limit_ms) + "ms";
        if (options_.temperature > 0)
            text += ",temp=" + std::to_string(options_.temperature).substr(0, 4);
        return text + ")";
    }

    void AlphaZeroAgent::seed(uint64_t value)
    {
        search_.seed(value);
        rng_.reseed(value ^ 0xA5A5A5A5ULL);
    }

    MoveChoice AlphaZeroAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();

        const AlphaZeroSearch::Result result = search_.search(position, options_.time_limit_ms / 1000.0);

        MoveChoice choice;
        choice.scores = emptyScores(position);
        int total = 0;
        for (int visits : result.visits)
            total += visits;

        int best = -1;
        for (size_t col = 0; col < result.visits.size(); ++col)
        {
            if (!position.canPlay(static_cast<int>(col)))
                continue;
            choice.scores[col] = total > 0 ? static_cast<double>(result.visits[col]) / total : 0.0;
            if (best < 0 || result.visits[col] > result.visits[static_cast<size_t>(best)])
                best = static_cast<int>(col);
        }

        if (options_.temperature > 0 && total > 0)
        {
            // Sample proportionally to visits^(1/temperature)
            std::vector<double> weights(result.visits.size(), 0.0);
            double sum = 0;
            for (size_t col = 0; col < weights.size(); ++col)
            {
                if (result.visits[col] > 0)
                {
                    weights[col] = std::pow(static_cast<double>(result.visits[col]), 1.0 / options_.temperature);
                    sum += weights[col];
                }
            }
            double ticket = rng_.uniform() * sum;
            for (size_t col = 0; col < weights.size(); ++col)
            {
                ticket -= weights[col];
                if (weights[col] > 0 && ticket <= 0)
                {
                    best = static_cast<int>(col);
                    break;
                }
            }
        }

        choice.column = best;
        choice.value = result.value;
        choice.nodes = result.evaluations;
        choice.info = "network + tree search";
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }
}
