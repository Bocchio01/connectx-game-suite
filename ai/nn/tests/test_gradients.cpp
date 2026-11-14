// Gradient checking of the hand-written backpropagation.
//
// This file is compiled with NN_REAL_TYPE=double (see ai/nn/CMakeLists.txt). For every
// parameter w of a network we compare the gradient computed by backward() with the
// numerical derivative of the loss, estimated with central differences:
//
//     dL/dw  ~=  ( L(w + h) - L(w - h) ) / (2 h)
//
// If the chain rule is implemented correctly the two agree to many digits.

#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include "nn/layers.hpp"
#include "nn/loss.hpp"
#include "nn/network.hpp"
#include "test_util.hpp"

using namespace nn;

static_assert(sizeof(Real) == sizeof(double), "this test must be built with NN_REAL_TYPE=double");

namespace
{
    /** Largest relative error between the analytic and numerical gradients of `parameters`. */
    template <typename LossFunction>
    double checkGradients(const std::vector<Parameter *> &parameters, LossFunction loss, const char *what)
    {
        const double h = 1e-6;
        double worst = 0;
        size_t checked = 0;

        for (Parameter *p : parameters)
        {
            for (size_t i = 0; i < p->value.size(); ++i)
            {
                const Real original = p->value.data()[i];

                p->value.data()[i] = original + h;
                const double plus = loss();
                p->value.data()[i] = original - h;
                const double minus = loss();
                p->value.data()[i] = original;

                const double numeric = (plus - minus) / (2 * h);
                const double analytic = p->grad.data()[i];
                const double error = std::fabs(numeric - analytic) / std::max(1e-7, std::fabs(numeric) + std::fabs(analytic));

                worst = std::max(worst, error);
                ++checked;

                CHECK_MSG(error < 1e-5, what << ": parameter " << p->name << "[" << i << "] analytic " << analytic << " numeric " << numeric);
            }
        }

        std::cout << "    " << what << ": " << checked << " gradients checked, worst relative error " << worst << std::endl;
        return worst;
    }

    Matrix randomMatrix(size_t rows, size_t cols, std::mt19937_64 &rng, double scale = 1.0)
    {
        std::normal_distribution<double> normal(0.0, scale);
        Matrix m(rows, cols);
        for (size_t i = 0; i < m.size(); ++i)
            m.data()[i] = normal(rng);
        return m;
    }
}

void test_dense_and_activations()
{
    std::mt19937_64 rng(1);

    // Dense -> Tanh -> Dense -> ReLU -> MSE against a random target
    Sequential net;
    net.add<Dense>(5, 7, rng, Init::XAVIER_NORMAL);
    net.add<Tanh>();
    net.add<Dense>(7, 3, rng, Init::HE_NORMAL);
    net.add<ReLU>();

    const Matrix x = randomMatrix(6, 5, rng);
    const Matrix target = randomMatrix(6, 3, rng);

    std::vector<Parameter *> params;
    net.collectParameters(params);

    auto lossValue = [&]()
    {
        Matrix unused;
        return static_cast<double>(meanSquaredError(net.forward(x), target, unused));
    };

    for (Parameter *p : params)
        p->zeroGrad();
    Matrix d;
    meanSquaredError(net.forward(x), target, d);
    net.backward(d);

    CHECK(checkGradients(params, lossValue, "Dense/Tanh/ReLU/MSE") < 1e-5);

    // The gradient with respect to the input must be right too (it is what the previous layer receives)
    {
        Matrix input = x;
        Matrix grad_input = net.backward(d); // (accumulates parameter gradients again, irrelevant here)
        const double h = 1e-6;
        double worst = 0;
        for (size_t i = 0; i < input.size(); ++i)
        {
            const Real original = input.data()[i];
            Matrix unused;
            input.data()[i] = original + h;
            const double plus = meanSquaredError(net.forward(input), target, unused);
            input.data()[i] = original - h;
            const double minus = meanSquaredError(net.forward(input), target, unused);
            input.data()[i] = original;
            const double numeric = (plus - minus) / (2 * h);
            const double analytic = grad_input.data()[i];
            worst = std::max(worst, std::fabs(numeric - analytic) / std::max(1e-7, std::fabs(numeric) + std::fabs(analytic)));
        }
        CHECK(worst < 1e-5);
        std::cout << "    input gradient: worst relative error " << worst << std::endl;
    }
}

void test_policy_value_network()
{
    std::mt19937_64 rng(2);

    NetworkConfig config;
    config.input_size = 9;
    config.hidden = {7, 6, 5};
    config.policy_size = 4;
    PolicyValueNet net(config, 77);

    const size_t batch = 8;
    const Matrix x = randomMatrix(batch, config.input_size, rng);

    // Target policy: random distributions; target value in [-1, 1]; some policy entries masked out
    Matrix policy_target(batch, config.policy_size);
    Matrix mask(batch, config.policy_size, 1.0);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (size_t i = 0; i < batch; ++i)
    {
        double sum = 0;
        for (size_t j = 0; j < config.policy_size; ++j)
        {
            if (i % 3 == 0 && j == i % config.policy_size)
                mask(i, j) = 0; // an illegal move
            policy_target(i, j) = mask(i, j) != 0 ? uniform(rng) + 0.01 : 0.0;
            sum += policy_target(i, j);
        }
        for (size_t j = 0; j < config.policy_size; ++j)
            policy_target(i, j) /= sum;
    }
    Matrix value_target(batch, 1);
    for (size_t i = 0; i < batch; ++i)
        value_target(i, 0) = 2 * uniform(rng) - 1;

    const double value_weight = 0.7;

    auto lossValue = [&]()
    {
        Matrix logits, values, dl, dv;
        net.forward(x, logits, values);
        const double policy_loss = softmaxCrossEntropy(logits, policy_target, dl, &mask);
        const double value_loss = meanSquaredError(values, value_target, dv);
        return policy_loss + value_weight * value_loss;
    };

    // Analytic gradients
    net.zeroGrad();
    {
        Matrix logits, values, dlogits, dvalues;
        net.forward(x, logits, values);
        softmaxCrossEntropy(logits, policy_target, dlogits, &mask);
        meanSquaredError(values, value_target, dvalues);
        for (size_t i = 0; i < dvalues.size(); ++i)
            dvalues.data()[i] *= value_weight;
        net.backward(dlogits, dvalues);
    }

    CHECK(checkGradients(net.parameters(), lossValue, "PolicyValueNet (policy CE with mask + value MSE)") < 1e-5);
}

void test_policy_value_network_without_trunk()
{
    std::mt19937_64 rng(3);

    NetworkConfig config;
    config.input_size = 6;
    config.hidden = {}; // heads directly on the input
    config.policy_size = 3;
    PolicyValueNet net(config, 5);

    const Matrix x = randomMatrix(5, 6, rng);
    Matrix policy_target(5, 3, 1.0 / 3.0);
    Matrix value_target = randomMatrix(5, 1, rng, 0.3);

    auto lossValue = [&]()
    {
        Matrix logits, values, dl, dv;
        net.forward(x, logits, values);
        return static_cast<double>(softmaxCrossEntropy(logits, policy_target, dl)) + meanSquaredError(values, value_target, dv);
    };

    net.zeroGrad();
    Matrix logits, values, dlogits, dvalues;
    net.forward(x, logits, values);
    softmaxCrossEntropy(logits, policy_target, dlogits);
    meanSquaredError(values, value_target, dvalues);
    net.backward(dlogits, dvalues);

    CHECK(checkGradients(net.parameters(), lossValue, "PolicyValueNet without trunk") < 1e-5);
}

int main()
{
    RUN_TEST(test_dense_and_activations);
    RUN_TEST(test_policy_value_network);
    RUN_TEST(test_policy_value_network_without_trunk);
    return testSummary("nn_test_gradients");
}
