#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "nn/layers.hpp"

namespace nn
{
    /** Shape of a PolicyValueNet (and, informatively, of the game it was built for). */
    struct NetworkConfig
    {
        /** Number of inputs (for Connect-X: rows * cols * 2 board planes). */
        size_t input_size = 0;

        /** Widths of the shared hidden layers (each followed by a ReLU). */
        std::vector<size_t> hidden = {128, 128};

        /** Number of policy outputs (for Connect-X: the number of columns). */
        size_t policy_size = 0;

        /** Board the network was trained for; stored in the model file (0 = unknown). */
        uint32_t board_rows = 0;
        uint32_t board_cols = 0;
        uint32_t connect = 0;

        /**
         * How the board is turned into the input vector (an id defined by the application, see
         * ai/encoding.hpp). The library only stores it in the model file.
         */
        uint32_t encoding = 0;
    };

    /**
     * A two-headed multilayer perceptron in the style of AlphaZero:
     *
     *                          +--> Dense(policy_size) ----------------> policy logits
     *   input --> trunk -------+
     *   (Dense + ReLU)*        +--> Dense(1) --> tanh -----------------> value in (-1, 1)
     *
     * The *trunk* is shared: its features are used both to say which move looks best
     * (policy) and who is winning (value). Training both tasks at once makes the
     * features richer than training each task alone.
     *
     * Only standard operations are used (fully connected + ReLU + tanh), so the
     * very same network can be rebuilt in Keras and converted to TensorFlow Lite.
     */
    class PolicyValueNet
    {
    public:
        explicit PolicyValueNet(const NetworkConfig &config, uint64_t seed = 1);

        const NetworkConfig &config() const { return config_; }

        // ----- training (batches) ---------------------------------------------

        /**
         * Forward pass over a batch (one sample per row of `input`).
         * @param logits receives (batch x policy_size) unnormalised policy scores
         * @param values receives (batch x 1) values in (-1, 1)
         * Keeps the intermediate results needed by backward().
         */
        void forward(const Matrix &input, Matrix &logits, Matrix &values);

        /**
         * Backward pass: dLoss/dLogits and dLoss/dValues in, gradients of all parameters
         * accumulated in the parameters. Must follow a forward() on the same batch.
         */
        void backward(const Matrix &dlogits, const Matrix &dvalues);

        /** Every trainable parameter, in a fixed order. */
        std::vector<Parameter *> parameters();

        void zeroGrad();

        /** Total number of trainable numbers (weights and biases). */
        size_t parameterCount() const;

        // ----- inference (one sample) -----------------------------------------

        /**
         * Evaluates a single sample without allocating memory. Thread-safe: the weights
         * are only read and the scratch buffers are thread-local.
         * @param input  config().input_size numbers
         * @param logits receives policy_size policy scores
         * @param value  receives the value in (-1, 1)
         */
        void predict(const Real *input, Real *logits, Real *value) const;

        // ----- persistence ----------------------------------------------------

        /**
         * Writes the model in the ".cxnn" format (documented in ai/nn/README.md):
         * little-endian, float32 weights in the same layout as Keras' Dense kernels.
         * @param metadata free text stored in the file (e.g. how the model was trained)
         */
        void save(std::ostream &out, const std::string &metadata = "") const;
        void save(const std::string &path, const std::string &metadata = "") const;

        /** @throws std::runtime_error if the file is missing, truncated or inconsistent */
        static PolicyValueNet load(std::istream &in, std::string *metadata = nullptr);
        static PolicyValueNet load(const std::string &path, std::string *metadata = nullptr);

        /** Copies the weights of another network with the same shape. */
        void copyWeightsFrom(const PolicyValueNet &other);

    private:
        NetworkConfig config_;
        Sequential trunk_;
        std::vector<Dense *> trunk_dense_; // the Dense layers inside trunk_
        std::unique_ptr<Dense> policy_head_;
        std::unique_ptr<Dense> value_head_;
        Tanh value_activation_;
        Matrix grad_features_;
    };
}
