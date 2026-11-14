#pragma once

#include <cstddef>

#include "nn/matrix.hpp"

namespace nn
{
    /**
     * Softmax of one row of logits: p_j = exp(l_j - max) / sum_k exp(l_k - max).
     * Subtracting the maximum does not change the result and prevents overflows.
     *
     * @param legal optional array of n flags; classes whose flag is 0 get probability 0
     *              (used to exclude the columns that are full). At least one must be legal.
     */
    void softmax(const Real *logits, size_t n, Real *probabilities, const unsigned char *legal = nullptr);

    /**
     * Cross-entropy between the softmax of the logits and a target distribution:
     *
     *     loss_i = - sum_j target_ij * log( softmax(logits_i)_j )
     *
     * It is the loss of the *policy*: the network is pushed to put its probability
     * mass where the target (a solver's verdict, or the visit counts of a tree
     * search) puts it.
     *
     * The gradient of the softmax + cross-entropy pair is the famously simple
     * dLoss/dLogit_j = softmax_j * sum(target) - target_j  (= p_j - t_j for a proper distribution).
     *
     * @param mask   optional (batch x classes) matrix: classes whose entry is 0 are excluded
     *               from the softmax and get no gradient (illegal moves)
     * @param dlogits receives the gradient of the *mean* loss over the batch
     * @return the mean loss over the batch
     */
    Real softmaxCrossEntropy(const Matrix &logits, const Matrix &targets, Matrix &dlogits, const Matrix *mask = nullptr);

    /**
     * Mean squared error summed over the columns and averaged over the batch:
     *
     *     loss = (1 / N) sum_i sum_j (prediction_ij - target_ij)^2
     *
     * It is the loss of the *value*. `dpredictions` receives its gradient.
     */
    Real meanSquaredError(const Matrix &predictions, const Matrix &targets, Matrix &dpredictions);
}
