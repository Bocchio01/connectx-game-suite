#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ai
{
    /**
     * Number of bits set in a 64-bit word.
     */
    inline int popcount(uint64_t x)
    {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_popcountll(x);
#elif defined(_MSC_VER) && defined(_M_X64)
        return static_cast<int>(__popcnt64(x));
#else
        x -= (x >> 1) & 0x5555555555555555ULL;
        x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
        x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
        return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
#endif
    }

    /**
     * Index of the lowest set bit. @p x must be non-zero.
     */
    inline int lowestBit(uint64_t x)
    {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_ctzll(x);
#elif defined(_MSC_VER) && defined(_M_X64)
        unsigned long index = 0;
        _BitScanForward64(&index, x);
        return static_cast<int>(index);
#else
        int index = 0;
        while ((x & 1ULL) == 0)
        {
            x >>= 1;
            ++index;
        }
        return index;
#endif
    }

    /**
     * Clears the lowest set bit of @p x.
     */
    inline uint64_t clearLowestBit(uint64_t x)
    {
        return x & (x - 1);
    }
}
