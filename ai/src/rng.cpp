#include <chrono>
#include <random>

#include "ai/rng.hpp"

namespace ai
{
    uint64_t randomSeed()
    {
        std::random_device device;
        const uint64_t entropy = (static_cast<uint64_t>(device()) << 32) ^ device();
        const uint64_t clock = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return entropy ^ (clock * 0x9E3779B97F4A7C15ULL);
    }
}
