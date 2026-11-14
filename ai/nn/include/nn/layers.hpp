#pragma once

#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "nn/matrix.hpp"

namespace nn
{
    /**
     * A trainable tensor together with the gradient of the loss with respect to it.
     * Gradients are *accumulated* by the backward passes and cleared with zeroGrad().
     */
    struct Parameter
    {
        Matrix value;
        Matrix grad;
        bool decay = true; // weights are subject to weight decay, biases are not
        std::string name;

        Parameter() = default;
        Parameter(size_t rows, size_t cols, bool weight_decay, std::string parameter_name)
            : value(rows, cols), grad(rows, cols), decay(weight_decay), name(std::move(parameter_name)) {}

        void zeroGrad() { grad.zero(); }
    };

    /**
     * One stage of a network.
     *
     * Training a network is: forward pass (compute the outputs, remembering what
     * the backward pass will need), then backward pass (apply the chain rule from
     * the end of the network to the beginning, producing for every layer the
     * gradient of the loss with respect to its parameters and to its input).
     */
    class Layer
    {
    public:
        virtual ~Layer() = default;

        /**
         * @param input one sample per row
         * @return the output (a buffer owned by the layer, valid until the next forward call)
         */
        virtual const Matrix &forward(const Matrix &input) = 0;

        /**
         * @param grad_output dLoss/dOutput of the last forward call
         * @return dLoss/dInput. As a side effect dLoss/dParameters are added to the gradients of the parameters.
         */
        virtual const Matrix &backward(const Matrix &grad_output) = 0;

        virtual void collectParameters(std::vector<Parameter *> &parameters) { (void)parameters; }

        virtual std::string name() const = 0;
    };

    /** How the weights of a Dense layer are drawn. */
    enum class Init
    {
        HE_NORMAL,    // N(0, 2 / fan_in): the right scale in front of a ReLU
        XAVIER_NORMAL // N(0, 2 / (fan_in + fan_out)): for linear / tanh outputs
    };

    /**
     * Fully connected layer: y = x W + b, with W of shape (inputs x outputs).
     * (Keras' Dense uses the very same layout for its kernel, which makes the
     * export to TensorFlow a plain copy.)
     *
     * Backward pass, with dY = dLoss/dY and a batch of N samples:
     *   dW = X^T dY          (sum over the batch of the outer products x_n dy_n)
     *   db = sum_n dy_n
     *   dX = dY W^T
     */
    class Dense : public Layer
    {
    public:
        /** @param scale multiplies the standard deviation of the initial weights (small values give small initial outputs) */
        Dense(size_t inputs, size_t outputs, std::mt19937_64 &rng, Init init = Init::HE_NORMAL, Real scale = 1);

        const Matrix &forward(const Matrix &input) override;
        const Matrix &backward(const Matrix &grad_output) override;
        void collectParameters(std::vector<Parameter *> &parameters) override;
        std::string name() const override { return "Dense"; }

        size_t inputSize() const { return weight_.value.rows(); }
        size_t outputSize() const { return weight_.value.cols(); }

        Parameter &weight() { return weight_; }
        const Parameter &weight() const { return weight_; }
        Parameter &bias() { return bias_; }
        const Parameter &bias() const { return bias_; }

    private:
        Parameter weight_;
        Parameter bias_;
        Matrix input_;
        Matrix output_;
        Matrix grad_input_;
    };

    /** y = max(0, x) */
    class ReLU : public Layer
    {
    public:
        const Matrix &forward(const Matrix &input) override;
        const Matrix &backward(const Matrix &grad_output) override;
        std::string name() const override { return "ReLU"; }

    private:
        Matrix output_;
        Matrix grad_input_;
    };

    /** y = tanh(x), squashes to (-1, 1). */
    class Tanh : public Layer
    {
    public:
        const Matrix &forward(const Matrix &input) override;
        const Matrix &backward(const Matrix &grad_output) override;
        std::string name() const override { return "Tanh"; }

    private:
        Matrix output_;
        Matrix grad_input_;
    };

    /** A chain of layers: the output of each one is the input of the next. */
    class Sequential
    {
    public:
        /** Appends a layer built in place and returns a reference to it. */
        template <typename LayerType, typename... Args>
        LayerType &add(Args &&...args)
        {
            auto layer = std::make_unique<LayerType>(std::forward<Args>(args)...);
            LayerType &reference = *layer;
            layers_.push_back(std::move(layer));
            return reference;
        }

        const Matrix &forward(const Matrix &input);
        const Matrix &backward(const Matrix &grad_output);
        void collectParameters(std::vector<Parameter *> &parameters);

        size_t size() const { return layers_.size(); }
        Layer &operator[](size_t index) { return *layers_[index]; }

    private:
        std::vector<std::unique_ptr<Layer>> layers_;
    };
}
