#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ai/encoding.hpp"
#include "ai/position.hpp"
#include "nn/matrix.hpp"

namespace ai
{
    namespace training
    {
        /** Marker stored in LabeledPosition::scores for columns that cannot be played. */
        constexpr int8_t kIllegalScore = -128;

        /**
         * A position labelled by the exact solver ("teacher").
         *
         * `scores[c]` is the game-theoretic score of playing column c (see Solver for the
         * convention). In a "weak" dataset the scores are only -1 / 0 / +1 (loss / draw / win).
         */
        struct LabeledPosition
        {
            uint64_t current = 0; // stones of the player to move (bitboard, see Geometry)
            uint64_t mask = 0;    // all stones
            std::array<int8_t, kMaxCols> scores;
            int8_t value = 0; // outcome for the player to move with perfect play: -1, 0, +1

            LabeledPosition() { scores.fill(kIllegalScore); }
        };

        /** How the label of a position is turned into a target distribution for the policy head. */
        enum class PolicyTarget
        {
            /** Uniform over the best moves (highest score: fastest win / slowest defeat). */
            BEST,

            /**
             * Uniform over every move that keeps the game-theoretic result (all winning moves
             * in a won position, all drawing moves in a drawn one). In lost positions: the best moves.
             * Easier to learn than BEST (does not ask the network to pick the *fastest* win) and
             * just as strong: a policy that never throws the result away never loses a won game.
             */
            OUTCOME,

            /** softmax(score / temperature) over the legal moves: also tells how bad the bad moves are. */
            SOFT
        };

        /** The label of a position in the form the training code uses. */
        struct Targets
        {
            std::array<float, kMaxCols> policy; // probabilities over the columns (0 for illegal ones)
            uint32_t optimal_mask = 0;          // bit c set: playing column c keeps the result (for accuracy metrics)
            float value = 0.0f;
        };

        /** Converts a label to training targets. */
        Targets makeTargets(const LabeledPosition &position, int cols, PolicyTarget mode, double soft_temperature = 1.0);

        /**
         * A collection of labelled positions for one board shape, with a simple binary
         * file format (".cxds"; documented in ai/README.md) that Python can read with numpy.
         */
        class Dataset
        {
        public:
            Dataset(int rows = 6, int cols = 7, int connect = 4) : rows_(rows), cols_(cols), connect_(connect) {}

            int rows() const { return rows_; }
            int cols() const { return cols_; }
            int connect() const { return connect_; }

            size_t size() const { return items_.size(); }
            bool empty() const { return items_.empty(); }
            const LabeledPosition &operator[](size_t index) const { return items_[index]; }
            const std::vector<LabeledPosition> &items() const { return items_; }

            void reserve(size_t count) { items_.reserve(count); }
            void add(const LabeledPosition &position) { items_.push_back(position); }
            void append(const Dataset &other);

            /** @throws std::runtime_error on I/O errors */
            void save(const std::string &path) const;

            /** @throws std::runtime_error if the file is missing or malformed */
            static Dataset load(const std::string &path);

        private:
            int rows_;
            int cols_;
            int connect_;
            std::vector<LabeledPosition> items_;
        };

        /** Parameters of dataset generation. */
        struct GenerateOptions
        {
            int rows = 6;
            int cols = 7;
            int connect = 4;

            /** Number of labelled positions to produce. */
            size_t count = 10000;

            int threads = 1;
            uint64_t seed = 1;

            /** Positions with fewer stones are not labelled (early positions are very expensive to solve). */
            int min_stones = 8;

            /** Games are played by an alpha-beta agent of this depth (0 = pure random play)... */
            int player_depth = 4;

            /** ...which plays a random move with this probability, to reach varied positions. */
            double epsilon = 0.2;

            /** Each position of a game is labelled with this probability (the rest of the game is skipped). */
            double sample_probability = 0.35;

            /** Only distinguish win / draw / loss (much faster than exact scores). */
            bool weak = true;

            /** Positions that the solver cannot label within this time are skipped. */
            int solve_time_ms = 4000;
        };

        /**
         * Plays many games (a noisy alpha-beta player against itself) and labels the positions met
         * with the exact solver. Positions are unique (a position already in the set is skipped).
         * @param progress optional callback (labelled so far, target, positions skipped for timeout)
         */
        Dataset generateDataset(const GenerateOptions &options,
                                const std::function<void(size_t, size_t, size_t)> &progress = nullptr);

        /**
         * Writes the network input of `position` into row `row` of `inputs` and its legal-move
         * mask into row `row` of `masks` (1 = playable column). Used to assemble mini-batches.
         */
        void encodeInto(const Position &position, Encoding encoding, size_t row, nn::Matrix &inputs, nn::Matrix &masks);
    }
}
