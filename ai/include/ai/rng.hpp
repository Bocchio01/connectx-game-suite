#pragma once

#include <cstdint>
#include <limits>

namespace ai
{
    /**
     * xoshiro256** pseudo random number generator.
     *
     * Fast, tiny state (cheap to copy and to seed), good statistical quality.
     * It satisfies UniformRandomBitGenerator so it can also drive the
     * distributions of <random> (e.g. std::gamma_distribution).
     */
    class Rng
    {
    public:
        using result_type = uint64_t;

        static constexpr result_type min() { return 0; }
        static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

        explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ULL)
        {
            reseed(seed);
        }

        /**
         * Re-initialise the generator. The 256-bit state is expanded from the
         * seed with splitmix64, as recommended by the authors of xoshiro.
         */
        void reseed(uint64_t seed)
        {
            for (auto &word : state_)
            {
                seed += 0x9E3779B97F4A7C15ULL;
                uint64_t z = seed;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                word = z ^ (z >> 31);
            }
        }

        result_type operator()()
        {
            const uint64_t result = rotl(state_[1] * 5, 7) * 9;
            const uint64_t t = state_[1] << 17;

            state_[2] ^= state_[0];
            state_[3] ^= state_[1];
            state_[1] ^= state_[2];
            state_[0] ^= state_[3];
            state_[2] ^= t;
            state_[3] = rotl(state_[3], 45);

            return result;
        }

        /**
         * Uniform integer in [0, n). Multiply-shift reduction: the bias is
         * below n / 2^32, irrelevant for the tiny ranges used here.
         */
        uint32_t below(uint32_t n)
        {
            const uint64_t x = (*this)() >> 32;
            return static_cast<uint32_t>((x * n) >> 32);
        }

        /**
         * Uniform real number in [0, 1).
         */
        double uniform()
        {
            return static_cast<double>((*this)() >> 11) * (1.0 / 9007199254740992.0);
        }

    private:
        static uint64_t rotl(uint64_t x, int k)
        {
            return (x << k) | (x >> (64 - k));
        }

        uint64_t state_[4];
    };

    /**
     * A non-deterministic seed (std::random_device mixed with the clock).
     */
    uint64_t randomSeed();
}
