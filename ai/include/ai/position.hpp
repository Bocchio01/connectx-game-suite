#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ai/bitops.hpp"

namespace ai
{
    /** Hard limits of the bitboard engine (see Geometry::isSupported). */
    constexpr int kMaxCols = 16;
    constexpr int kMaxConnect = 16;

    /**
     * Immutable description of a board shape plus every constant derived from it.
     *
     * Cells are packed in a 64-bit word, one column after the other. Every
     * column uses (rows + 1) bits: the extra, always empty, "sentinel" bit on
     * top of each column stops vertical, horizontal and diagonal bit-shifts
     * from wrapping around into the neighbouring column.
     *
     *     .  .  .  .  .  .  .      <- sentinel row (never occupied)
     *     5 12 19 26 33 40 47      bit index = col * (rows + 1) + row_from_bottom
     *     4 11 18 25 32 39 46
     *     3 10 17 24 31 38 45
     *     2  9 16 23 30 37 44
     *     1  8 15 22 29 36 43
     *     0  7 14 21 28 35 42
     *
     * Instances are cached per (rows, cols, connect) and live for the whole
     * process, so positions simply keep a pointer to their geometry.
     */
    struct Geometry
    {
        int rows;
        int cols;
        int connect;
        int stride; // rows + 1: bit distance between a cell and the one above it in the next column
        int cells;  // rows * cols

        uint64_t board_mask;  // every playable cell
        uint64_t bottom_mask; // the bottom cell of every column
        uint64_t center_mask; // the cells of the central column(s)
        std::array<uint64_t, kMaxCols> column_mask;
        std::array<uint64_t, kMaxCols> bottom_cell;
        std::array<uint64_t, kMaxCols> top_cell;

        /** Columns sorted from the centre outwards (best columns first). */
        std::array<int, kMaxCols> move_order;

        /** Bit distance between two neighbouring cells of a line: vertical, horizontal, "/" and "\". */
        std::array<int, 4> directions;

        /** Every line of `connect` adjacent cells on the board (used by the heuristic evaluation). */
        std::vector<uint64_t> windows;

        /**
         * Returns the (cached) geometry of a board.
         * @throws std::invalid_argument if the shape is not supported
         */
        static const Geometry &get(int rows, int cols, int connect);

        /**
         * Whether the bitboard engine can handle a game configuration.
         * Requirements: 2 players, rows >= 4, cols >= 4, 3 <= connect <= max(rows, cols)
         * and cols * (rows + 1) <= 64 (the whole board must fit in one machine word:
         * 6x7, 7x7, 7x8, 5x9, 4x12 ... are fine, 8x8 is not).
         */
        static bool isSupported(int rows, int cols, int connect, std::string *reason = nullptr);

    private:
        Geometry(int rows, int cols, int connect);
    };

    /**
     * A game position for two players, optimised for tree search.
     *
     * The position is stored *relative to the player to move*: `current()`
     * always holds the stones of the side that has to play and `opponent()` the
     * others. After play() the roles swap with a single XOR, so no side-to-move
     * flag and no player ids are needed. The whole object is 4 machine words
     * and is trivially copyable, which makes "copy, play, recurse" search
     * cheap (no undo logic needed).
     *
     * This is the engine used by every AI agent. The network/GUI layers keep
     * using the reference implementation in `core`; the unit tests check that
     * both implement exactly the same rules.
     */
    class Position
    {
    public:
        /** Creates the empty position of a rows x cols board. */
        explicit Position(int rows = 6, int cols = 7, int connect = 4);

        /**
         * Builds a position from a grid as sent by the server.
         * @param grid            grid[0] is the TOP row; 0 = empty, otherwise a player id
         * @param current_player  id of the player to move
         * @param opponent_player id of the other player
         * @throws std::invalid_argument for inconsistent grids (unknown ids, floating stones, ...)
         */
        static Position fromGrid(const std::vector<std::vector<uint8_t>> &grid,
                                 uint8_t current_player,
                                 uint8_t opponent_player,
                                 int connect);

        /**
         * Builds a position from its two bitboards (see Geometry for the bit layout).
         * Used to store positions compactly (16 bytes) in replay buffers.
         */
        static Position fromBitboards(int rows, int cols, int connect, uint64_t current, uint64_t mask);

        /**
         * Builds a position by replaying a sequence of moves written as columns
         * numbered from 1 ('1'..'9', then 'a', 'b', ... for larger boards).
         * Handy for tests: "4453" means: column 4, column 4, column 5, column 3.
         * @throws std::invalid_argument on illegal moves or moves after the game is over
         */
        static Position fromMoves(int rows, int cols, int connect, const std::string &columns);

        // ----- geometry ------------------------------------------------------

        const Geometry &geometry() const { return *geo_; }
        int rows() const { return geo_->rows; }
        int cols() const { return geo_->cols; }
        int connect() const { return geo_->connect; }

        // ----- state ---------------------------------------------------------

        /** Number of stones on the board. */
        int moves() const { return moves_; }

        /** Stones of the player to move. */
        uint64_t current() const { return cur_; }

        /** Stones of the other player. */
        uint64_t opponent() const { return cur_ ^ mask_; }

        /** All stones. */
        uint64_t mask() const { return mask_; }

        /**
         * Unique identifier of the position (usable as hash-table key): within
         * each column `cur + mask` encodes both the height and the owner of
         * every stone, and columns never carry into each other.
         */
        uint64_t key() const { return cur_ + mask_; }

        /** True when the player who just moved has connected enough stones. */
        bool lastMoveWon() const { return won_; }

        /** True when the board is full and nobody won. */
        bool isDraw() const { return !won_ && moves_ == geo_->cells; }

        /** True when the game is over (somebody won or the board is full). */
        bool isTerminal() const { return won_ || moves_ == geo_->cells; }

        /** Content of a cell seen from the player to move: 0 empty, 1 mine, 2 opponent's. Row 0 is the TOP row. */
        int cell(int row_from_top, int col) const;

        // ----- moves ---------------------------------------------------------

        /** True if `col` exists and is not full. */
        bool canPlay(int col) const
        {
            return col >= 0 && col < geo_->cols && (mask_ & geo_->top_cell[col]) == 0;
        }

        /** One bit per column: the lowest free cell of every non-full column. */
        uint64_t legalMoveMask() const
        {
            return (mask_ + geo_->bottom_mask) & geo_->board_mask;
        }

        /** The free cell where a stone dropped in `col` would land (0 if the column is full). */
        uint64_t playableCell(int col) const
        {
            return (mask_ + geo_->bottom_cell[col]) & geo_->column_mask[col];
        }

        int legalMoveCount() const { return popcount(legalMoveMask()); }

        /** Playable columns in ascending order. */
        std::vector<int> legalMoves() const;

        /**
         * Drops a stone of the player to move in `col` and passes the turn.
         * Precondition: canPlay(col) and the game is not over.
         * @return true if the move won the game
         */
        bool play(int col)
        {
            playCell(playableCell(col));
            return won_;
        }

        /** Like play(), but takes the target cell as a one-bit mask (see playableCell()). */
        void playCell(uint64_t cell)
        {
            cur_ ^= mask_;
            mask_ |= cell;
            ++moves_;
            won_ = hasConnect(cur_ ^ mask_); // stones of the player who has just moved
        }

        /** True if the player to move wins immediately by playing `col`. */
        bool isWinningMove(int col) const
        {
            return hasConnect(cur_ | playableCell(col));
        }

        // ----- tactics -------------------------------------------------------

        /** True if the given set of stones contains a line of `connect` stones. */
        bool hasConnect(uint64_t stones) const;

        /**
         * Empty cells where adding a stone to `stones` completes a line.
         * @param occupied every occupied cell (both players)
         */
        uint64_t winningCells(uint64_t stones, uint64_t occupied) const;

        /** Cells that would give the player to move a line (playable or not yet). */
        uint64_t currentThreats() const { return winningCells(cur_, mask_); }

        /** Cells that would give the opponent a line. */
        uint64_t opponentThreats() const { return winningCells(cur_ ^ mask_, mask_); }

        /** True if the player to move has an immediate winning move. */
        bool canWinNext() const { return (currentThreats() & legalMoveMask()) != 0; }

        /**
         * Playable cells that do not lose at once, as a one-bit-per-column mask.
         * Excludes moves that let the opponent win immediately: failing to block
         * an opponent's winning cell and playing right under it. Assumes the
         * player to move has no immediate win; returns 0 if every move loses.
         */
        uint64_t nonLosingMoves() const;

        // ----- utilities -----------------------------------------------------

        /** The same position reflected left-to-right (Connect-X is symmetric). */
        Position mirrored() const;

        /** Number of floats written by encode(): rows * cols * 2. */
        int encodedSize() const { return geo_->rows * geo_->cols * 2; }

        /**
         * Neural-network input encoding, relative to the player to move.
         * Layout "NHWC" flattened: out[(row * cols + col) * 2 + channel] with row 0 = top,
         * channel 0 = stones of the player to move, channel 1 = opponent's stones.
         */
        void encode(float *out) const;

        /** Multi-line ASCII picture (x = player who moved first, o = the other). */
        std::string toString() const;

        bool operator==(const Position &other) const
        {
            return geo_ == other.geo_ && cur_ == other.cur_ && mask_ == other.mask_;
        }

        bool operator!=(const Position &other) const { return !(*this == other); }

    private:
        const Geometry *geo_;
        uint64_t cur_;
        uint64_t mask_;
        int moves_;
        bool won_;
    };
}
