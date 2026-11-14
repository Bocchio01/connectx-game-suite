#include <stdexcept>

#include "ai/encoding.hpp"

namespace ai
{
    const char *encodingName(Encoding encoding)
    {
        switch (encoding)
        {
        case Encoding::RAW:
            return "raw";
        case Encoding::TACTICAL:
            return "tactical";
        case Encoding::WINDOWS:
            return "windows";
        }
        return "?";
    }

    Encoding parseEncoding(const std::string &name)
    {
        if (name == "raw")
            return Encoding::RAW;
        if (name == "tactical")
            return Encoding::TACTICAL;
        if (name == "windows")
            return Encoding::WINDOWS;
        throw std::invalid_argument("Unknown encoding '" + name + "' (expected raw, tactical or windows)");
    }

    int encodedSize(const Geometry &geometry, Encoding encoding)
    {
        switch (encoding)
        {
        case Encoding::RAW:
            return geometry.cells * 2;
        case Encoding::TACTICAL:
            return geometry.cells * 5;
        case Encoding::WINDOWS:
            return geometry.cells * 2 + 2 * static_cast<int>(geometry.windows.size());
        }
        return 0;
    }

    void encodePosition(const Position &position, Encoding encoding, float *out)
    {
        const Geometry &geometry = position.geometry();
        const int rows = geometry.rows;
        const int cols = geometry.cols;

        const uint64_t mine = position.current();
        const uint64_t theirs = position.opponent();

        auto bit = [&](int row, int col)
        {
            return col * geometry.stride + (rows - 1 - row);
        };

        if (encoding == Encoding::RAW || encoding == Encoding::WINDOWS)
        {
            for (int row = 0; row < rows; ++row)
            {
                for (int col = 0; col < cols; ++col)
                {
                    float *cell = out + (row * cols + col) * 2;
                    cell[0] = static_cast<float>((mine >> bit(row, col)) & 1ULL);
                    cell[1] = static_cast<float>((theirs >> bit(row, col)) & 1ULL);
                }
            }

            if (encoding == Encoding::WINDOWS)
            {
                float *window_out = out + geometry.cells * 2;
                const float scale = 1.0f / static_cast<float>(geometry.connect);
                for (const uint64_t window : geometry.windows)
                {
                    const int my_stones = popcount(mine & window);
                    const int their_stones = popcount(theirs & window);
                    *window_out++ = their_stones == 0 ? static_cast<float>(my_stones) * scale : 0.0f;
                    *window_out++ = my_stones == 0 ? static_cast<float>(their_stones) * scale : 0.0f;
                }
            }
            return;
        }

        // TACTICAL
        const uint64_t my_threats = position.currentThreats();
        const uint64_t their_threats = position.opponentThreats();
        const uint64_t playable = position.legalMoveMask();

        for (int row = 0; row < rows; ++row)
        {
            for (int col = 0; col < cols; ++col)
            {
                float *cell = out + (row * cols + col) * 5;
                const int b = bit(row, col);
                cell[0] = static_cast<float>((mine >> b) & 1ULL);
                cell[1] = static_cast<float>((theirs >> b) & 1ULL);
                cell[2] = static_cast<float>((my_threats >> b) & 1ULL);
                cell[3] = static_cast<float>((their_threats >> b) & 1ULL);
                cell[4] = static_cast<float>((playable >> b) & 1ULL);
            }
        }
    }
}
