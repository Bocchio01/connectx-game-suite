#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include "ai/agents/neural_agent.hpp"
#include "ai/agents/neural_search_agent.hpp"
#include "ai/encoding.hpp"

namespace ai
{
    namespace
    {
        // Proven wins score 1 minus a small penalty per ply (faster wins first); the network's opinion
        // is shrunk to +-0.9 so that it can never outweigh a proven result.
        constexpr double kWin = 1.0;
        constexpr double kPlyPenalty = 0.001;
        constexpr double kNetworkScale = 0.9;
        constexpr double kInfinity = 10.0;
    }

    NeuralSearchAgent::NeuralSearchAgent(std::shared_ptr<const nn::PolicyValueNet> network, uint64_t seed)
        : NeuralSearchAgent(std::move(network), Options(), seed)
    {
    }

    NeuralSearchAgent::NeuralSearchAgent(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed)
        : network_(std::move(network)),
          options_(options),
          evaluations_(0),
          rng_(seed)
    {
        if (!network_)
            throw std::invalid_argument("NeuralSearchAgent needs a network");
        options_.depth = std::max(1, std::min(options_.depth, 12));
    }

    std::string NeuralSearchAgent::name() const
    {
        return "nnsearch(depth=" + std::to_string(options_.depth) + ", " + std::to_string(network_->parameterCount()) + " params)";
    }

    double NeuralSearchAgent::networkValue(const Position &position)
    {
        input_.resize(network_->config().input_size);
        logits_.resize(network_->config().policy_size);
        encodePosition(position, static_cast<Encoding>(network_->config().encoding), input_.data());

        nn::Real value = 0;
        network_->predict(input_.data(), logits_.data(), &value);
        ++evaluations_;
        return kNetworkScale * static_cast<double>(value);
    }

    double NeuralSearchAgent::negamax(const Position &position, int depth, double alpha, double beta, int ply)
    {
        if (position.isDraw())
            return 0.0;

        // Exact tactics, as in AlphaBetaAgent: an immediate win ends the search of the node; moves that
        // let the opponent win at once are never searched (if all of them do, the position is lost)
        if ((position.currentThreats() & position.legalMoveMask()) != 0)
            return kWin - kPlyPenalty * (ply + 1);

        const uint64_t candidates = position.nonLosingMoves();
        if (candidates == 0)
            return -(kWin - kPlyPenalty * (ply + 2));

        if (depth == 0)
            return networkValue(position);

        const Geometry &geometry = position.geometry();
        double best = -kInfinity;

        for (int i = 0; i < geometry.cols; ++i)
        {
            const uint64_t cell = position.playableCell(geometry.move_order[i]);
            if ((cell & candidates) == 0)
                continue;

            Position child = position;
            child.playCell(cell);

            const double score = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
            best = std::max(best, score);
            alpha = std::max(alpha, score);
            if (alpha >= beta)
                break;
        }

        return best;
    }

    MoveChoice NeuralSearchAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();
        checkNetworkMatches(network_->config(), position);
        evaluations_ = 0;

        const Geometry &geometry = position.geometry();
        MoveChoice choice;
        choice.scores = emptyScores(position);

        double best_score = -kInfinity;
        std::vector<int> best_columns;

        for (int i = 0; i < geometry.cols; ++i)
        {
            const int col = geometry.move_order[i];
            if (!position.canPlay(col))
                continue;

            Position child = position;
            child.play(col);

            double score;
            if (child.lastMoveWon())
                score = kWin - kPlyPenalty;
            else
                // Every root move is searched with a full window: the scores are exact, which is what makes
                // them meaningful as MoveChoice::scores (and lets ties be detected)
                score = -negamax(child, options_.depth - 1, -kInfinity, kInfinity, 1);

            choice.scores[static_cast<size_t>(col)] = score;
            if (score > best_score + 1e-12)
            {
                best_score = score;
                best_columns.assign(1, col);
            }
            else if (std::fabs(score - best_score) <= 1e-12)
            {
                best_columns.push_back(col);
            }
        }

        choice.column = options_.randomize ? best_columns[rng_.below(static_cast<uint32_t>(best_columns.size()))] : best_columns.front();
        choice.value = std::max(-1.0, std::min(1.0, best_score));
        choice.nodes = evaluations_;
        choice.depth = options_.depth;
        choice.info = "alpha-beta with the network as evaluation";
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }
}
