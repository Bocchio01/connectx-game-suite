#pragma once

#include <vector>

#include "nn/layers.hpp"

namespace nn
{
    /**
     * Updates the parameters of a network using the gradients computed by the
     * backward pass. step() does not clear the gradients: call zeroGrad() on
     * the network before the next forward/backward pass.
     */
    class Optimizer
    {
    public:
        explicit Optimizer(Real learning_rate) : learning_rate_(learning_rate) {}
        virtual ~Optimizer() = default;

        virtual void step(const std::vector<Parameter *> &parameters) = 0;

        void setLearningRate(Real learning_rate) { learning_rate_ = learning_rate; }
        Real learningRate() const { return learning_rate_; }

    protected:
        Real learning_rate_;
    };

    /**
     * Stochastic gradient descent with momentum.
     *
     *     v <- momentum * v + gradient (+ weight_decay * w)
     *     w <- w - learning_rate * v
     *
     * The velocity v is a running average of the recent gradients: it smooths the
     * noise of the mini-batches and speeds up movement along consistent directions.
     * With `nesterov` the gradient is "looked ahead": w <- w - lr * (gradient + momentum * v).
     */
    class SGD : public Optimizer
    {
    public:
        SGD(Real learning_rate, Real momentum = Real(0.9), Real weight_decay = 0, bool nesterov = false)
            : Optimizer(learning_rate), momentum_(momentum), weight_decay_(weight_decay), nesterov_(nesterov) {}

        void step(const std::vector<Parameter *> &parameters) override;

    private:
        Real momentum_;
        Real weight_decay_;
        bool nesterov_;
        std::vector<Matrix> velocity_;
    };

    /**
     * Adam (Kingma & Ba) with decoupled weight decay (AdamW).
     *
     * Keeps a running average of the gradient (m) and of its square (v) and moves each
     * weight by  lr * m / (sqrt(v) + eps): parameters with consistently large gradients
     * get smaller steps, rarely updated ones get larger steps. The bias correction
     * compensates for m and v starting at zero.
     */
    class Adam : public Optimizer
    {
    public:
        explicit Adam(Real learning_rate = Real(1e-3), Real beta1 = Real(0.9), Real beta2 = Real(0.999),
                      Real epsilon = Real(1e-8), Real weight_decay = 0)
            : Optimizer(learning_rate), beta1_(beta1), beta2_(beta2), epsilon_(epsilon), weight_decay_(weight_decay), steps_(0) {}

        void step(const std::vector<Parameter *> &parameters) override;

    private:
        Real beta1_;
        Real beta2_;
        Real epsilon_;
        Real weight_decay_;
        long steps_;
        std::vector<Matrix> first_moment_;
        std::vector<Matrix> second_moment_;
    };

    /**
     * Rescales all the gradients so that their global L2 norm does not exceed `max_norm`
     * (protects against the occasional exploding gradient).
     * @return the norm before clipping
     */
    Real clipGradientNorm(const std::vector<Parameter *> &parameters, Real max_norm);
}
