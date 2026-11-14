#pragma once

#include <cstdint>
#include <optional>
#include <vector>

/**
 * Pure game helpers used by the GUI: no Qt, no network, easy to unit-test.
 *
 * Boards follow the wire convention of the protocol: board[0] is the TOP row,
 * a cell holds 0 when empty and the player id (1..N) otherwise. A disc dropped
 * in a column lands in the lowest empty row of that column.
 */
namespace GameLogic
{
    using Board = std::vector<std::vector<uint8_t>>;

    /** A board position (row 0 is the top row). */
    struct Cell
    {
        int row = 0;
        int col = 0;
    };

    inline bool operator==(const Cell &a, const Cell &b) { return a.row == b.row && a.col == b.col; }
    inline bool operator!=(const Cell &a, const Cell &b) { return !(a == b); }
    inline bool operator<(const Cell &a, const Cell &b)
    {
        return a.row != b.row ? a.row < b.row : a.col < b.col;
    }

    /** A disc that was not on the board in the previous state. */
    struct DiscChange
    {
        Cell cell;
        uint8_t player = 0;
    };

    /** An empty rows x cols board. */
    Board makeEmptyBoard(int rows, int cols);

    /** True if the board has exactly `rows` rows of `cols` cells each. */
    bool hasShape(const Board &board, int rows, int cols);

    /** Lowest empty row of a column, or -1 if the column is full (or out of range). */
    int landingRow(const Board &board, int col);

    /** Number of discs on the board. */
    int countDiscs(const Board &board);

    /**
     * Finds every run of at least `connectLength` consecutive discs of `player`
     * along a row, a column or one of the two diagonals. Each run is returned as
     * the ordered list of its cells (left to right, or top to bottom for columns)
     * and includes ALL of the consecutive discs, even when the run is longer than
     * `connectLength`. Empty when the player has no such run.
     */
    std::vector<std::vector<Cell>> findWinningLines(const Board &board, uint8_t player, int connectLength);

    /** All the cells of all the winning runs (without duplicates, in row-major order). */
    std::vector<Cell> findWinningCells(const Board &board, uint8_t player, int connectLength);

    /**
     * Discs that appear in `next` compared to `prev`.
     * Returns std::nullopt when the transition is not an incremental one, i.e. the
     * board changed shape or a disc was removed/replaced (new game, reset...).
     * An empty vector means that the two boards are identical.
     */
    std::optional<std::vector<DiscChange>> findNewDiscs(const Board &prev, const Board &next);
}
