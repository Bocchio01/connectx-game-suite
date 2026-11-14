#include <cassert>
#include <cmath>

#include "nn/layers.hpp"

namespace nn
{
    // ========================================================================
    // Dense
    // ========================================================================

    Dense::Dense(size_t inputs, size_t outputs, std::mt19937_64 &rng, Init init, Real scale)
        : weight_(inputs, outputs, true, "weight"),
          bias_(1, outputs, false, "bias")
    {
        const double fan_in = static_cast<double>(inputs);
        const double fan_out = static_cast<double>(outputs);
        const double variance = (init == Init::HE_NORMAL) ? 2.0 / fan_in : 2.0 / (fan_in + fan_out);
        const double stddev = std::sqrt(variance) * static_cast<double>(scale);

        std::normal_distribution<double> normal(0.0, stddev);
        for (size_t i = 0; i < weight_.value.size(); ++i)
            weight_.value.data()[i] = static_cast<Real>(normal(rng));

        bias_.value.zero();
        weight_.grad.zero();
        bias_.grad.zero();
    }

    const Matrix &Dense::forward(const Matrix &input)
    {
        assert(input.cols() == inputSize());

        input_ = input; // the backward pass needs it
        matmul(input_, weight_.value, output_);
        addRowVector(output_, bias_.value);
        return output_;
    }

    const Matrix &Dense::backward(const Matrix &grad_output)
    {
        assert(grad_output.cols() == outputSize() && grad_output.rows() == input_.rows());

        matmulAtB(input_, grad_output, weight_.grad, true); // dW += X^T dY
        columnSums(grad_output, bias_.grad, true);          // db += sum of the rows of dY
        matmulABt(grad_output, weight_.value, grad_input_); // dX  = dY W^T
        return grad_input_;
    }

    void Dense::collectParameters(std::vector<Parameter *> &parameters)
    {
        parameters.push_back(&weight_);
        parameters.push_back(&bias_);
    }

    // ========================================================================
    // Activations
    // ========================================================================

    const Matrix &ReLU::forward(const Matrix &input)
    {
        output_.resize(input.rows(), input.cols());
        const Real *in = input.data();
        Real *out = output_.data();
        for (size_t i = 0; i < input.size(); ++i)
            out[i] = in[i] > 0 ? in[i] : 0;
        return output_;
    }

    const Matrix &ReLU::backward(const Matrix &grad_output)
    {
        assert(grad_output.size() == output_.size());

        grad_input_.resize(grad_output.rows(), grad_output.cols());
        const Real *g = grad_output.data();
        const Real *out = output_.data();
        Real *gi = grad_input_.data();
        for (size_t i = 0; i < grad_output.size(); ++i)
            gi[i] = out[i] > 0 ? g[i] : 0; // the derivative of max(0, x) is 1 for x > 0, else 0
        return grad_input_;
    }

    const Matrix &Tanh::forward(const Matrix &input)
    {
        output_.resize(input.rows(), input.cols());
        const Real *in = input.data();
        Real *out = output_.data();
        for (size_t i = 0; i < input.size(); ++i)
            out[i] = std::tanh(in[i]);
        return output_;
    }

    const Matrix &Tanh::backward(const Matrix &grad_output)
    {
        assert(grad_output.size() == output_.size());

        grad_input_.resize(grad_output.rows(), grad_output.cols());
        const Real *g = grad_output.data();
        const Real *out = output_.data();
        Real *gi = grad_input_.data();
        for (size_t i = 0; i < grad_output.size(); ++i)
            gi[i] = g[i] * (1 - out[i] * out[i]); // d tanh(x) / dx = 1 - tanh(x)^2
        return grad_input_;
    }

    // ========================================================================
    // Sequential
    // ========================================================================

    const Matrix &Sequential::forward(const Matrix &input)
    {
        const Matrix *current = &input;
        for (auto &layer : layers_)
            current = &layer->forward(*current);
        return *current;
    }

    const Matrix &Sequential::backward(const Matrix &grad_output)
    {
        const Matrix *current = &grad_output;
        for (auto it = layers_.rbegin(); it != layers_.rend(); ++it)
            current = &(*it)->backward(*current);
        return *current;
    }

    void Sequential::collectParameters(std::vector<Parameter *> &parameters)
    {
        for (auto &layer : layers_)
            layer->collectParameters(parameters);
    }
}
