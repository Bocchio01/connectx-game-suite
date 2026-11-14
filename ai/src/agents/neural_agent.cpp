#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include "ai/agents/neural_agent.hpp"
#include "ai/encoding.hpp"
#include "nn/loss.hpp"

namespace ai
{
    void checkNetworkMatches(const nn::NetworkConfig &config, const Position &position)
    {
        const bool shape_ok = config.encoding <= static_cast<uint32_t>(Encoding::WINDOWS) &&
                              config.input_size == static_cast<size_t>(encodedSize(position.geometry(), static_cast<Encoding>(config.encoding))) &&
                              config.policy_size == static_cast<size_t>(position.cols()) &&
                              (config.board_rows == 0 || config.board_rows == static_cast<uint32_t>(position.rows())) &&
                              (config.board_cols == 0 || config.board_cols == static_cast<uint32_t>(position.cols())) &&
                              (config.connect == 0 || config.connect == static_cast<uint32_t>(position.connect()));

        if (!shape_ok)
        {
            throw std::invalid_argument("This model was trained for a " + std::to_string(config.board_rows) + "x" +
                                        std::to_string(config.board_cols) + " board with connect " + std::to_string(config.connect) +
                                        ", but the game is " + std::to_string(position.rows()) + "x" + std::to_string(position.cols()) +
                                        " with connect " + std::to_string(position.connect()));
        }
    }

    NeuralAgent::NeuralAgent(std::shared_ptr<const nn::PolicyValueNet> network, uint64_t seed)
        : NeuralAgent(std::move(network), Options(), seed)
    {
    }

    NeuralAgent::NeuralAgent(std::shared_ptr<const nn::PolicyValueNet> network, const Options &options, uint64_t seed)
        : network_(std::move(network)),
          options_(options),
          rng_(seed)
    {
        if (!network_)
            throw std::invalid_argument("NeuralAgent needs a network");
    }

    std::string NeuralAgent::name() const
    {
        std::string text = "nn(";
        text += std::to_string(network_->parameterCount()) + " params";
        if (options_.temperature > 0)
            text += ",temp=" + std::to_string(options_.temperature).substr(0, 4);
        if (options_.mirror)
            text += ",mirror";
        if (options_.tactics)
            text += ",tactics";
        return text + ")";
    }

    MoveChoice NeuralAgent::chooseMove(const Position &position)
    {
        const auto start = std::chrono::steady_clock::now();
        checkNetworkMatches(network_->config(), position);

        MoveChoice choice;
        choice.scores = emptyScores(position);

        const size_t columns = static_cast<size_t>(position.cols());
        const uint64_t legal = position.legalMoveMask();

        // Optional tactical override: take a win, block a loss
        if (options_.tactics)
        {
            uint64_t forced = position.currentThreats() & legal;
            if (forced == 0)
                forced = position.opponentThreats() & legal;
            if (forced != 0)
            {
                choice.column = columnOfCell(position, forced & (~forced + 1));
                choice.info = "tactics";
                choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                return choice;
            }
        }

        // Network evaluation
        const Encoding encoding = static_cast<Encoding>(network_->config().encoding);
        input_.resize(network_->config().input_size);
        std::vector<nn::Real> logits(columns);
        nn::Real value = 0;

        encodePosition(position, encoding, input_.data());
        network_->predict(input_.data(), logits.data(), &value);
        uint64_t evaluations = 1;

        if (options_.mirror)
        {
            // The mirrored board must get the mirrored answer: average the two estimates
            const Position mirrored = position.mirrored();
            std::vector<nn::Real> mirrored_logits(columns);
            nn::Real mirrored_value = 0;
            encodePosition(mirrored, encoding, input_.data());
            network_->predict(input_.data(), mirrored_logits.data(), &mirrored_value);

            for (size_t col = 0; col < columns; ++col)
                logits[col] = (logits[col] + mirrored_logits[columns - 1 - col]) / 2;
            value = (value + mirrored_value) / 2;
            ++evaluations;
        }

        // Masked softmax over the legal columns
        std::vector<unsigned char> legal_flags(columns, 0);
        for (size_t col = 0; col < columns; ++col)
            legal_flags[col] = position.canPlay(static_cast<int>(col)) ? 1 : 0;

        std::vector<nn::Real> probabilities(columns);
        if (options_.temperature > 0)
        {
            std::vector<nn::Real> scaled(logits);
            for (nn::Real &l : scaled)
                l = static_cast<nn::Real>(l / options_.temperature);
            nn::softmax(scaled.data(), columns, probabilities.data(), legal_flags.data());
        }
        else
        {
            nn::softmax(logits.data(), columns, probabilities.data(), legal_flags.data());
        }

        int best = -1;
        for (size_t col = 0; col < columns; ++col)
        {
            if (!legal_flags[col])
                continue;
            choice.scores[col] = probabilities[col];
            if (best < 0 || logits[col] > logits[static_cast<size_t>(best)])
                best = static_cast<int>(col);
        }

        if (options_.temperature > 0)
        {
            // Sample from the distribution
            double ticket = rng_.uniform();
            for (size_t col = 0; col < columns; ++col)
            {
                if (!legal_flags[col])
                    continue;
                ticket -= probabilities[col];
                if (ticket <= 0)
                {
                    best = static_cast<int>(col);
                    break;
                }
            }
        }

        choice.column = best;
        choice.value = value;
        choice.nodes = evaluations;
        choice.info = "policy network";
        choice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return choice;
    }
}
