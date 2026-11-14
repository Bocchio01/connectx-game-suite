#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

/**
 * Scalar type used by the whole library. It is `float` (what TensorFlow Lite uses too);
 * the gradient-checking test rebuilds the library with -DNN_REAL_TYPE=double to
 * compare analytic and numerical gradients without float rounding noise.
 */
#ifndef NN_REAL_TYPE
#define NN_REAL_TYPE float
#endif

namespace nn
{
    using Real = NN_REAL_TYPE;

    /**
     * A dense row-major matrix: element (r, c) lives at data[r * cols + c].
     *
     * A batch of samples is a matrix with one sample per row, so a layer maps a
     * (batch x inputs) matrix to a (batch x outputs) one.
     */
    class Matrix
    {
    public:
        Matrix() : rows_(0), cols_(0) {}

        Matrix(size_t rows, size_t cols, Real value = 0)
            : rows_(rows), cols_(cols), data_(rows * cols, value) {}

        size_t rows() const { return rows_; }
        size_t cols() const { return cols_; }
        size_t size() const { return data_.size(); }
        bool empty() const { return data_.empty(); }

        Real *data() { return data_.data(); }
        const Real *data() const { return data_.data(); }

        Real *row(size_t r) { return data_.data() + r * cols_; }
        const Real *row(size_t r) const { return data_.data() + r * cols_; }

        Real &operator()(size_t r, size_t c) { return data_[r * cols_ + c]; }
        const Real &operator()(size_t r, size_t c) const { return data_[r * cols_ + c]; }

        /** Changes the shape. The memory is only touched if the number of elements changes. */
        void resize(size_t rows, size_t cols)
        {
            rows_ = rows;
            cols_ = cols;
            data_.resize(rows * cols);
        }

        void fill(Real value) { std::fill(data_.begin(), data_.end(), value); }
        void zero() { fill(0); }

    private:
        size_t rows_;
        size_t cols_;
        std::vector<Real> data_;
    };

    // ------------------------------------------------------------------------
    // Kernels. They are the only place where the arithmetic cost of a network is
    // spent, so the loops are arranged to touch memory sequentially (and to be
    // auto-vectorised by the compiler). Rows of zeros inputs are skipped: the
    // board encodings are sparse and so are ReLU activations.
    // ------------------------------------------------------------------------

    /** out = a * b            (a: m x k, b: k x n, out: m x n) */
    void matmul(const Matrix &a, const Matrix &b, Matrix &out);

    /** out (+)= a^T * b       (a: k x m, b: k x n, out: m x n); with `accumulate` the result is added to `out` */
    void matmulAtB(const Matrix &a, const Matrix &b, Matrix &out, bool accumulate);

    /** out = a * b^T          (a: m x k, b: n x k, out: m x n) */
    void matmulABt(const Matrix &a, const Matrix &b, Matrix &out);

    /** For every row r of `m`: m[r][j] += row[0][j]. */
    void addRowVector(Matrix &m, const Matrix &row);

    /** out[0][j] (+)= sum over the rows of m[r][j]. */
    void columnSums(const Matrix &m, Matrix &out, bool accumulate);
}
