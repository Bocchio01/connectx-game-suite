#pragma once

#include <cstdint>
#include <string>

#include "ai/position.hpp"

namespace ai
{
    /**
     * How a position is presented to a neural network. Every encoding is relative to the player
     * to move ("mine" = the player who has to play) and is flattened as NHWC:
     * out[(row * cols + col) * planes + plane], with row 0 = top.
     *
     *   RAW      2 planes: 0 my stones, 1 the opponent's stones.
     *            The network must discover everything by itself, e.g. what a "three in a row" is.
     *
     *   TACTICAL 5 planes: the two above, plus 2 the empty cells where I would complete a line,
     *            3 the empty cells where the opponent would complete a line, 4 the cell where a
     *            stone dropped in each column lands (the playable cells). The 1-move tactics (win
     *            now, block now, do not play under a threat) become trivially visible; the network
     *            can spend its capacity on strategy.
     *
     *   WINDOWS  the 2 raw planes followed by 2 numbers for every line of `connect` cells
     *            ("window") of the board: the share of the line that I hold if the opponent holds
     *            nothing there (else 0), and the same for the opponent. This is the information
     *            used by the classic heuristic evaluation, as network inputs.
     *
     * The encoding is stored in the model file, so every program that evaluates a network (C++,
     * Python, the Arduino sketch) knows how to build its input.
     */
    enum class Encoding : uint32_t
    {
        RAW = 0,
        TACTICAL = 1,
        WINDOWS = 2
    };

    /** "raw", "tactical", "windows" */
    const char *encodingName(Encoding encoding);

    /** @throws std::invalid_argument for unknown names */
    Encoding parseEncoding(const std::string &name);

    /** Number of floats of an encoded position of the given board. */
    int encodedSize(const Geometry &geometry, Encoding encoding);

    /** Writes encodedSize() floats. */
    void encodePosition(const Position &position, Encoding encoding, float *out);
}
