#pragma once

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

// Minimal test helpers. Unlike <cassert>, these checks stay active in Release builds
// (CI runs the tests with -DCMAKE_BUILD_TYPE=Release, where assert() compiles to nothing).

namespace testutil
{
    inline int &failures()
    {
        static int count = 0;
        return count;
    }

    inline int &checks()
    {
        static int count = 0;
        return count;
    }
}

#define CHECK(condition)                                                                       \
    do                                                                                         \
    {                                                                                          \
        ++testutil::checks();                                                                  \
        if (!(condition))                                                                      \
        {                                                                                      \
            ++testutil::failures();                                                            \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition << "\n"; \
        }                                                                                      \
    } while (0)

#define CHECK_MSG(condition, message)                                                          \
    do                                                                                         \
    {                                                                                          \
        ++testutil::checks();                                                                  \
        if (!(condition))                                                                      \
        {                                                                                      \
            ++testutil::failures();                                                            \
            std::ostringstream oss_;                                                           \
            oss_ << message;                                                                   \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition " -- "   \
                      << oss_.str() << "\n";                                                   \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b) CHECK_MSG((a) == (b), "got " << (a) << " expected " << (b))

#define CHECK_NEAR(a, b, tolerance) \
    CHECK_MSG(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (tolerance), "got " << (a) << " expected " << (b) << " (+/- " << (tolerance) << ")")

#define RUN_TEST(function)                                                                        \
    do                                                                                            \
    {                                                                                             \
        const int before_ = testutil::failures();                                                 \
        const auto start_ = std::chrono::steady_clock::now();                                     \
        std::cout << "[ RUN  ] " #function << std::endl;                                          \
        function();                                                                               \
        const auto ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(                   \
                             std::chrono::steady_clock::now() - start_)                           \
                             .count();                                                            \
        std::cout << (testutil::failures() == before_ ? "[  OK  ] " : "[ FAIL ] ") << #function \
                  << " (" << ms_ << " ms)" << std::endl;                                          \
    } while (0)

/** Prints the summary and returns the process exit code. */
inline int testSummary(const char *suite)
{
    std::cout << "\n"
              << suite << ": " << testutil::checks() << " checks, "
              << testutil::failures() << " failures" << std::endl;
    return testutil::failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
