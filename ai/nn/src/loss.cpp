#include <cassert>
#include <cmath>
#include <limits>

#include "nn/loss.hpp"

namespace nn
{
    void softmax(const Real *logits, size_t n, Real *probabilities, const unsigned char *legal)
    {
        Real maximum = -std::numeric_limits<Real>::infinity();
        for (size_t j = 0; j < n; ++j)
        {
            if (legal == nullptr || legal[j])
                maximum = std::max(maximum, logits[j]);
        }

        Real sum = 0;
        for (size_t j = 0; j < n; ++j)
        {
            if (legal == nullptr || legal[j])
            {
                probabilities[j] = std::exp(logits[j] - maximum);
                sum += probabilities[j];
            }
            else
            {
                probabilities[j] = 0;
            }
        }

        for (size_t j = 0; j < n; ++j)
            probabilities[j] /= sum;
    }

    Real softmaxCrossEntropy(const Matrix &logits, const Matrix &targets, Matrix &dlogits, const Matrix *mask)
    {
        assert(logits.rows() == targets.rows() && logits.cols() == targets.cols());

        const size_t batch = logits.rows();
        const size_t classes = logits.cols();
        dlogits.resize(batch, classes);

        std::vector<Real> probabilities(classes);
        std::vector<unsigned char> legal(classes, 1);

        double total = 0;
        for (size_t i = 0; i < batch; ++i)
        {
            if (mask != nullptr)
            {
                for (size_t j = 0; j < classes; ++j)
                    legal[j] = (*mask)(i, j) != 0 ? 1 : 0;
            }

            softmax(logits.row(i), classes, probabilities.data(), legal.data());

            double target_sum = 0;
            for (size_t j = 0; j < classes; ++j)
            {
                if (!legal[j])
                    continue;
                target_sum += static_cast<double>(targets(i, j));
                total -= static_cast<double>(targets(i, j)) * std::log(static_cast<double>(probabilities[j]) + 1e-12);
            }

            // dLoss/dLogit_j = p_j * sum(t) - t_j, averaged over the batch
            for (size_t j = 0; j < classes; ++j)
            {
                dlogits(i, j) = legal[j]
                                    ? static_cast<Real>((static_cast<double>(probabilities[j]) * target_sum - static_cast<double>(targets(i, j))) / static_cast<double>(batch))
                                    : Real(0);
            }
        }

        return static_cast<Real>(total / static_cast<double>(batch));
    }

    Real meanSquaredError(const Matrix &predictions, const Matrix &targets, Matrix &dpredictions)
    {
        assert(predictions.rows() == targets.rows() && predictions.cols() == targets.cols());

        const size_t batch = predictions.rows();
        dpredictions.resize(batch, predictions.cols());

        double total = 0;
        const Real *p = predictions.data();
        const Real *t = targets.data();
        Real *d = dpredictions.data();
        const double scale = 1.0 / static_cast<double>(batch);

        for (size_t i = 0; i < predictions.size(); ++i)
        {
            const double error = static_cast<double>(p[i]) - static_cast<double>(t[i]);
            total += error * error;
            d[i] = static_cast<Real>(2.0 * error * scale);
        }

        return static_cast<Real>(total * scale);
    }
}
