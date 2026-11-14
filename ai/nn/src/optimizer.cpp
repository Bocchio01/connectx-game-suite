#include <cassert>
#include <cmath>

#include "nn/optimizer.hpp"

namespace nn
{
    void SGD::step(const std::vector<Parameter *> &parameters)
    {
        if (velocity_.size() != parameters.size())
        {
            velocity_.clear();
            for (const Parameter *p : parameters)
                velocity_.emplace_back(p->value.rows(), p->value.cols());
        }

        for (size_t k = 0; k < parameters.size(); ++k)
        {
            Parameter &p = *parameters[k];
            Real *w = p.value.data();
            const Real *g = p.grad.data();
            Real *v = velocity_[k].data();
            const Real decay = p.decay ? weight_decay_ : Real(0);

            for (size_t i = 0; i < p.value.size(); ++i)
            {
                const Real gradient = g[i] + decay * w[i];
                v[i] = momentum_ * v[i] + gradient;
                w[i] -= learning_rate_ * (nesterov_ ? gradient + momentum_ * v[i] : v[i]);
            }
        }
    }

    void Adam::step(const std::vector<Parameter *> &parameters)
    {
        if (first_moment_.size() != parameters.size())
        {
            first_moment_.clear();
            second_moment_.clear();
            for (const Parameter *p : parameters)
            {
                first_moment_.emplace_back(p->value.rows(), p->value.cols());
                second_moment_.emplace_back(p->value.rows(), p->value.cols());
            }
            steps_ = 0;
        }

        ++steps_;
        const double correction1 = 1.0 - std::pow(static_cast<double>(beta1_), static_cast<double>(steps_));
        const double correction2 = 1.0 - std::pow(static_cast<double>(beta2_), static_cast<double>(steps_));
        const Real step_size = static_cast<Real>(static_cast<double>(learning_rate_) * std::sqrt(correction2) / correction1);

        for (size_t k = 0; k < parameters.size(); ++k)
        {
            Parameter &p = *parameters[k];
            Real *w = p.value.data();
            const Real *g = p.grad.data();
            Real *m = first_moment_[k].data();
            Real *v = second_moment_[k].data();
            const bool decay = p.decay && weight_decay_ != 0;

            for (size_t i = 0; i < p.value.size(); ++i)
            {
                m[i] = beta1_ * m[i] + (1 - beta1_) * g[i];
                v[i] = beta2_ * v[i] + (1 - beta2_) * g[i] * g[i];
                w[i] -= step_size * m[i] / (std::sqrt(v[i]) + epsilon_);

                if (decay)
                    w[i] -= learning_rate_ * weight_decay_ * w[i]; // decoupled from the gradient statistics
            }
        }
    }

    Real clipGradientNorm(const std::vector<Parameter *> &parameters, Real max_norm)
    {
        double sum = 0;
        for (const Parameter *p : parameters)
        {
            const Real *g = p->grad.data();
            for (size_t i = 0; i < p->grad.size(); ++i)
                sum += static_cast<double>(g[i]) * static_cast<double>(g[i]);
        }

        const double norm = std::sqrt(sum);
        if (norm > static_cast<double>(max_norm) && norm > 0)
        {
            const Real scale = static_cast<Real>(static_cast<double>(max_norm) / norm);
            for (Parameter *p : parameters)
            {
                Real *g = p->grad.data();
                for (size_t i = 0; i < p->grad.size(); ++i)
                    g[i] *= scale;
            }
        }

        return static_cast<Real>(norm);
    }
}
