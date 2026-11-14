#include <cassert>

#include "nn/matrix.hpp"

namespace nn
{
    void matmul(const Matrix &a, const Matrix &b, Matrix &out)
    {
        assert(a.cols() == b.rows());
        const size_t m = a.rows();
        const size_t k = a.cols();
        const size_t n = b.cols();

        out.resize(m, n);
        out.zero();

        for (size_t i = 0; i < m; ++i)
        {
            Real *out_row = out.row(i);
            const Real *a_row = a.row(i);

            for (size_t p = 0; p < k; ++p)
            {
                const Real a_ip = a_row[p];
                if (a_ip == 0)
                    continue;

                const Real *b_row = b.row(p);
                for (size_t j = 0; j < n; ++j)
                    out_row[j] += a_ip * b_row[j];
            }
        }
    }

    void matmulAtB(const Matrix &a, const Matrix &b, Matrix &out, bool accumulate)
    {
        assert(a.rows() == b.rows());
        const size_t k = a.rows();
        const size_t m = a.cols();
        const size_t n = b.cols();

        if (accumulate)
        {
            assert(out.rows() == m && out.cols() == n);
        }
        else
        {
            out.resize(m, n);
            out.zero();
        }

        for (size_t p = 0; p < k; ++p)
        {
            const Real *a_row = a.row(p);
            const Real *b_row = b.row(p);

            for (size_t i = 0; i < m; ++i)
            {
                const Real a_pi = a_row[i];
                if (a_pi == 0)
                    continue;

                Real *out_row = out.row(i);
                for (size_t j = 0; j < n; ++j)
                    out_row[j] += a_pi * b_row[j];
            }
        }
    }

    void matmulABt(const Matrix &a, const Matrix &b, Matrix &out)
    {
        assert(a.cols() == b.cols());
        const size_t m = a.rows();
        const size_t k = a.cols();
        const size_t n = b.rows();

        out.resize(m, n);

        for (size_t i = 0; i < m; ++i)
        {
            const Real *a_row = a.row(i);
            Real *out_row = out.row(i);

            for (size_t j = 0; j < n; ++j)
            {
                const Real *b_row = b.row(j);

                // Four independent accumulators: breaks the dependency chain of the additions
                Real s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                size_t p = 0;
                for (; p + 4 <= k; p += 4)
                {
                    s0 += a_row[p] * b_row[p];
                    s1 += a_row[p + 1] * b_row[p + 1];
                    s2 += a_row[p + 2] * b_row[p + 2];
                    s3 += a_row[p + 3] * b_row[p + 3];
                }
                for (; p < k; ++p)
                    s0 += a_row[p] * b_row[p];

                out_row[j] = (s0 + s1) + (s2 + s3);
            }
        }
    }

    void addRowVector(Matrix &m, const Matrix &row)
    {
        assert(row.rows() == 1 && row.cols() == m.cols());
        const size_t cols = m.cols();
        const Real *bias = row.data();

        for (size_t r = 0; r < m.rows(); ++r)
        {
            Real *m_row = m.row(r);
            for (size_t j = 0; j < cols; ++j)
                m_row[j] += bias[j];
        }
    }

    void columnSums(const Matrix &m, Matrix &out, bool accumulate)
    {
        if (accumulate)
        {
            assert(out.rows() == 1 && out.cols() == m.cols());
        }
        else
        {
            out.resize(1, m.cols());
            out.zero();
        }

        const size_t cols = m.cols();
        Real *sums = out.data();

        for (size_t r = 0; r < m.rows(); ++r)
        {
            const Real *m_row = m.row(r);
            for (size_t j = 0; j < cols; ++j)
                sums[j] += m_row[j];
        }
    }
}
