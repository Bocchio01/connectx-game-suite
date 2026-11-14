// Tests of the neural network library: kernels, losses, optimisers, learning, persistence

#include <cmath>
#include <random>
#include <sstream>
#include <vector>

#include "nn/layers.hpp"
#include "nn/loss.hpp"
#include "nn/network.hpp"
#include "nn/optimizer.hpp"
#include "test_util.hpp"

using namespace nn;

namespace
{
    Matrix randomMatrix(size_t rows, size_t cols, std::mt19937_64 &rng, double sparsity = 0.0)
    {
        std::normal_distribution<double> normal(0.0, 1.0);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        Matrix m(rows, cols);
        for (size_t i = 0; i < m.size(); ++i)
            m.data()[i] = uniform(rng) < sparsity ? 0 : static_cast<Real>(normal(rng));
        return m;
    }

    double maxDifference(const Matrix &a, const Matrix &b)
    {
        CHECK_EQ(a.rows(), b.rows());
        CHECK_EQ(a.cols(), b.cols());
        double worst = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(a.data()[i]) - static_cast<double>(b.data()[i])));
        return worst;
    }
}

void test_matrix_kernels()
{
    std::mt19937_64 rng(1);

    // Odd sizes exercise the unrolled loops and their remainders
    for (size_t m : {1u, 2u, 3u, 5u, 8u, 13u})
    {
        for (size_t k : {1u, 3u, 4u, 7u, 16u})
        {
            for (size_t n : {1u, 2u, 6u, 9u})
            {
                const Matrix a = randomMatrix(m, k, rng, 0.3); // sparse: the kernels skip zeros
                const Matrix b = randomMatrix(k, n, rng, 0.3);

                // a * b
                Matrix expected(m, n);
                for (size_t i = 0; i < m; ++i)
                    for (size_t j = 0; j < n; ++j)
                    {
                        double sum = 0;
                        for (size_t p = 0; p < k; ++p)
                            sum += static_cast<double>(a(i, p)) * b(p, j);
                        expected(i, j) = static_cast<Real>(sum);
                    }

                Matrix product;
                matmul(a, b, product);
                CHECK(maxDifference(product, expected) < 1e-4);

                // a^T * c, with c of shape m x n  (a^T: k x m) -> k x n
                const Matrix c = randomMatrix(m, n, rng, 0.3);
                Matrix expected_at(k, n);
                for (size_t i = 0; i < k; ++i)
                    for (size_t j = 0; j < n; ++j)
                    {
                        double sum = 0;
                        for (size_t p = 0; p < m; ++p)
                            sum += static_cast<double>(a(p, i)) * c(p, j);
                        expected_at(i, j) = static_cast<Real>(sum);
                    }

                Matrix at_c;
                matmulAtB(a, c, at_c, false);
                CHECK(maxDifference(at_c, expected_at) < 1e-4);

                // accumulate = true adds to what is there
                matmulAtB(a, c, at_c, true);
                Matrix doubled = expected_at;
                for (size_t i = 0; i < doubled.size(); ++i)
                    doubled.data()[i] *= 2;
                CHECK(maxDifference(at_c, doubled) < 2e-4);

                // c * d^T with d: k x n  -> m x k
                const Matrix d = randomMatrix(k, n, rng);
                Matrix expected_bt(m, k);
                for (size_t i = 0; i < m; ++i)
                    for (size_t j = 0; j < k; ++j)
                    {
                        double sum = 0;
                        for (size_t p = 0; p < n; ++p)
                            sum += static_cast<double>(c(i, p)) * d(j, p);
                        expected_bt(i, j) = static_cast<Real>(sum);
                    }

                Matrix c_dt;
                matmulABt(c, d, c_dt);
                CHECK(maxDifference(c_dt, expected_bt) < 1e-4);
            }
        }
    }

    // addRowVector / columnSums
    Matrix m(3, 2);
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 2; ++j)
            m(i, j) = static_cast<Real>(i * 2 + j);
    Matrix bias(1, 2);
    bias(0, 0) = 10;
    bias(0, 1) = 20;
    addRowVector(m, bias);
    CHECK_EQ(m(2, 1), static_cast<Real>(25));

    // m is now [[10, 21], [12, 23], [14, 25]]
    Matrix sums;
    columnSums(m, sums, false);
    CHECK_EQ(sums(0, 0), static_cast<Real>(10 + 12 + 14));
    CHECK_EQ(sums(0, 1), static_cast<Real>(21 + 23 + 25));
    columnSums(m, sums, true); // accumulates: twice the sums
    CHECK_EQ(sums(0, 0), static_cast<Real>(2 * (10 + 12 + 14)));
    CHECK_EQ(sums(0, 1), static_cast<Real>(2 * (21 + 23 + 25)));
}

void test_softmax_and_losses()
{
    // softmax: probabilities, stability with huge logits, masking
    const Real logits[4] = {1, 2, 3, 4};
    Real p[4];
    softmax(logits, 4, p);
    Real sum = 0;
    for (Real v : p)
        sum += v;
    CHECK_NEAR(sum, 1.0, 1e-6);
    CHECK(p[3] > p[2] && p[2] > p[1] && p[1] > p[0]);

    const Real huge[3] = {1000, 1001, 1002};
    softmax(huge, 3, p);
    CHECK_NEAR(p[0] + p[1] + p[2], 1.0, 1e-6);
    CHECK(std::isfinite(p[0]) && std::isfinite(p[2]));

    const unsigned char legal[4] = {1, 0, 1, 0};
    softmax(logits, 4, p, legal);
    CHECK_EQ(p[1], Real(0));
    CHECK_EQ(p[3], Real(0));
    CHECK_NEAR(p[0] + p[2], 1.0, 1e-6);

    // Cross-entropy of uniform logits against any distribution is log(n); gradient is p - t
    Matrix uniform_logits(1, 4, 0.0);
    Matrix target(1, 4);
    target(0, 0) = 0.5;
    target(0, 1) = 0.25;
    target(0, 2) = 0.25;
    target(0, 3) = 0;
    Matrix dlogits;
    const Real loss = softmaxCrossEntropy(uniform_logits, target, dlogits);
    CHECK_NEAR(loss, std::log(4.0), 1e-5);
    CHECK_NEAR(dlogits(0, 0), 0.25 - 0.5, 1e-6);
    CHECK_NEAR(dlogits(0, 3), 0.25 - 0.0, 1e-6);

    // Masked classes get no gradient
    Matrix mask(1, 4, 1.0);
    mask(0, 3) = 0;
    softmaxCrossEntropy(uniform_logits, target, dlogits, &mask);
    CHECK_EQ(dlogits(0, 3), Real(0));
    CHECK_NEAR(dlogits(0, 0), 1.0 / 3.0 - 0.5, 1e-6);

    // MSE
    Matrix prediction(2, 1), expected(2, 1), dpred;
    prediction(0, 0) = 1;
    prediction(1, 0) = -1;
    expected(0, 0) = 0;
    expected(1, 0) = 1;
    const Real mse = meanSquaredError(prediction, expected, dpred);
    CHECK_NEAR(mse, (1.0 + 4.0) / 2.0, 1e-6);
    CHECK_NEAR(dpred(0, 0), 2.0 * 1.0 / 2.0, 1e-6);
    CHECK_NEAR(dpred(1, 0), 2.0 * -2.0 / 2.0, 1e-6);
}

void test_optimizers_fit_a_line()
{
    // y = 2 a - 3 b + 1 with a single Dense layer: both optimisers must find the coefficients
    for (int which = 0; which < 2; ++which)
    {
        std::mt19937_64 rng(10 + which);
        Dense layer(2, 1, rng, Init::XAVIER_NORMAL);
        std::vector<Parameter *> params;
        layer.collectParameters(params);

        std::unique_ptr<Optimizer> optimizer;
        if (which == 0)
            optimizer.reset(new SGD(Real(0.05), Real(0.9)));
        else
            optimizer.reset(new Adam(Real(0.05)));

        Matrix x(64, 2), y(64, 1);
        std::uniform_real_distribution<double> uniform(-1.0, 1.0);
        for (size_t i = 0; i < 64; ++i)
        {
            x(i, 0) = static_cast<Real>(uniform(rng));
            x(i, 1) = static_cast<Real>(uniform(rng));
            y(i, 0) = 2 * x(i, 0) - 3 * x(i, 1) + 1;
        }

        for (int step = 0; step < 600; ++step)
        {
            for (Parameter *p : params)
                p->zeroGrad();
            Matrix d;
            meanSquaredError(layer.forward(x), y, d);
            layer.backward(d);
            optimizer->step(params);
        }

        CHECK_NEAR(layer.weight().value(0, 0), 2.0, 0.02);
        CHECK_NEAR(layer.weight().value(1, 0), -3.0, 0.02);
        CHECK_NEAR(layer.bias().value(0, 0), 1.0, 0.02);
    }
}

void test_xor_is_learned()
{
    // The classic problem a linear model cannot solve: needs a hidden layer
    std::mt19937_64 rng(5);
    Sequential net;
    net.add<Dense>(2, 8, rng, Init::XAVIER_NORMAL);
    net.add<Tanh>();
    net.add<Dense>(8, 1, rng, Init::XAVIER_NORMAL);
    net.add<Tanh>();

    std::vector<Parameter *> params;
    net.collectParameters(params);
    Adam optimizer(Real(0.03));

    Matrix x(4, 2), y(4, 1);
    const Real inputs[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    const Real targets[4] = {-1, 1, 1, -1};
    for (size_t i = 0; i < 4; ++i)
    {
        x(i, 0) = inputs[i][0];
        x(i, 1) = inputs[i][1];
        y(i, 0) = targets[i];
    }

    for (int step = 0; step < 1500; ++step)
    {
        for (Parameter *p : params)
            p->zeroGrad();
        Matrix d;
        meanSquaredError(net.forward(x), y, d);
        net.backward(d);
        optimizer.step(params);
    }

    const Matrix &out = net.forward(x);
    for (size_t i = 0; i < 4; ++i)
        CHECK_MSG(out(i, 0) * targets[i] > 0.9, "xor sample " << i << " -> " << out(i, 0));
}

void test_policy_value_network_learns()
{
    // Memorise a few random boards: the policy must pick the labelled column, the value the labelled sign
    std::mt19937_64 rng(8);
    std::uniform_int_distribution<int> bit(0, 1);

    NetworkConfig config;
    config.input_size = 30;
    config.hidden = {32, 32};
    config.policy_size = 5;
    PolicyValueNet net(config, 3);

    const size_t samples = 24;
    Matrix x(samples, config.input_size);
    Matrix policy_target(samples, config.policy_size, 0.0);
    Matrix value_target(samples, 1);
    std::vector<size_t> best(samples);
    for (size_t i = 0; i < samples; ++i)
    {
        for (size_t j = 0; j < config.input_size; ++j)
            x(i, j) = static_cast<Real>(bit(rng));
        best[i] = static_cast<size_t>(rng() % config.policy_size);
        policy_target(i, best[i]) = 1;
        value_target(i, 0) = (i % 2 == 0) ? Real(1) : Real(-1);
    }

    Adam optimizer(Real(0.01));
    auto params = net.parameters();
    Real last_loss = 0;
    for (int step = 0; step < 400; ++step)
    {
        net.zeroGrad();
        Matrix logits, values, dlogits, dvalues;
        net.forward(x, logits, values);
        last_loss = softmaxCrossEntropy(logits, policy_target, dlogits) + meanSquaredError(values, value_target, dvalues);
        net.backward(dlogits, dvalues);
        optimizer.step(params);
    }

    CHECK_MSG(last_loss < 0.05, "final loss " << last_loss);

    Matrix logits, values;
    net.forward(x, logits, values);
    for (size_t i = 0; i < samples; ++i)
    {
        size_t arg = 0;
        for (size_t j = 1; j < config.policy_size; ++j)
            if (logits(i, j) > logits(i, arg))
                arg = j;
        CHECK_EQ(arg, best[i]);
        CHECK(values(i, 0) * value_target(i, 0) > 0.8);
    }
}

void test_predict_matches_batch_forward()
{
    std::mt19937_64 rng(21);
    NetworkConfig config;
    config.input_size = 84;
    config.hidden = {40, 24};
    config.policy_size = 7;
    PolicyValueNet net(config, 11);

    const Matrix x = randomMatrix(16, config.input_size, rng, 0.5);
    Matrix logits, values;
    net.forward(x, logits, values);

    for (size_t i = 0; i < x.rows(); ++i)
    {
        Real single_logits[7];
        Real single_value;
        net.predict(x.row(i), single_logits, &single_value);

        for (size_t j = 0; j < 7; ++j)
            CHECK_NEAR(single_logits[j], logits(i, j), 1e-4);
        CHECK_NEAR(single_value, values(i, 0), 1e-5);
    }

    CHECK_EQ(net.parameterCount(), static_cast<size_t>(84 * 40 + 40 + 40 * 24 + 24 + 24 * 7 + 7 + 24 + 1));
}

void test_save_and_load()
{
    NetworkConfig config;
    config.input_size = 84;
    config.hidden = {32, 16, 8};
    config.policy_size = 7;
    config.board_rows = 6;
    config.board_cols = 7;
    config.connect = 4;
    PolicyValueNet net(config, 99);

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    net.save(buffer, "trained for the tests");

    std::string metadata;
    PolicyValueNet loaded = PolicyValueNet::load(buffer, &metadata);
    CHECK_EQ(metadata, std::string("trained for the tests"));
    CHECK_EQ(loaded.config().input_size, config.input_size);
    CHECK_EQ(loaded.config().policy_size, config.policy_size);
    CHECK(loaded.config().hidden == config.hidden);
    CHECK_EQ(loaded.config().board_rows, 6u);
    CHECK_EQ(loaded.config().board_cols, 7u);
    CHECK_EQ(loaded.config().connect, 4u);

    // Identical predictions (float32 weights survive the round trip exactly)
    std::mt19937_64 rng(4);
    const Matrix x = randomMatrix(5, config.input_size, rng, 0.5);
    for (size_t i = 0; i < x.rows(); ++i)
    {
        Real a[7], b[7], va, vb;
        net.predict(x.row(i), a, &va);
        loaded.predict(x.row(i), b, &vb);
        for (size_t j = 0; j < 7; ++j)
            CHECK_EQ(a[j], b[j]);
        CHECK_EQ(va, vb);
    }

    // Corrupted files are rejected
    std::string bytes = buffer.str();
    {
        std::string bad_magic = bytes;
        bad_magic[0] = 'X';
        std::istringstream in(bad_magic, std::ios::binary);
        bool threw = false;
        try
        {
            PolicyValueNet::load(in);
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        CHECK(threw);
    }
    {
        std::istringstream in(bytes.substr(0, bytes.size() / 2), std::ios::binary);
        bool threw = false;
        try
        {
            PolicyValueNet::load(in);
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        CHECK(threw);
    }

    // copyWeightsFrom
    PolicyValueNet other(config, 1234);
    other.copyWeightsFrom(net);
    Real a[7], b[7], va, vb;
    net.predict(x.row(0), a, &va);
    other.predict(x.row(0), b, &vb);
    CHECK_EQ(a[3], b[3]);
    CHECK_EQ(va, vb);
}

void test_gradient_clipping()
{
    std::mt19937_64 rng(2);
    Dense layer(3, 2, rng);
    std::vector<Parameter *> params;
    layer.collectParameters(params);

    for (Parameter *p : params)
        p->grad.fill(10);

    const Real norm_before = clipGradientNorm(params, 1);
    CHECK_NEAR(norm_before, std::sqrt(8.0 * 100.0), 1e-4); // 6 weights + 2 biases

    double sum = 0;
    for (Parameter *p : params)
        for (size_t i = 0; i < p->grad.size(); ++i)
            sum += static_cast<double>(p->grad.data()[i]) * p->grad.data()[i];
    CHECK_NEAR(std::sqrt(sum), 1.0, 1e-4);

    // Small gradients are left alone
    const Real norm_after = clipGradientNorm(params, 5);
    CHECK_NEAR(norm_after, 1.0, 1e-4);
}

int main()
{
    RUN_TEST(test_matrix_kernels);
    RUN_TEST(test_softmax_and_losses);
    RUN_TEST(test_optimizers_fit_a_line);
    RUN_TEST(test_xor_is_learned);
    RUN_TEST(test_policy_value_network_learns);
    RUN_TEST(test_predict_matches_batch_forward);
    RUN_TEST(test_save_and_load);
    RUN_TEST(test_gradient_clipping);
    return testSummary("nn_test_basics");
}
