#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>

#include "ai/position.hpp"

namespace ai
{
    // ========================================================================
    // Geometry
    // ========================================================================

    bool Geometry::isSupported(int rows, int cols, int connect, std::string *reason)
    {
        auto fail = [reason](const char *why)
        {
            if (reason)
                *reason = why;
            return false;
        };

        if (rows < 4 || cols < 4)
            return fail("the board must be at least 4x4");
        if (connect < 3)
            return fail("the connect length must be at least 3");
        if (connect > kMaxConnect)
            return fail("the connect length is too large");
        if (connect > std::max(rows, cols))
            return fail("the connect length does not fit on the board");
        if (cols > kMaxCols || cols * (rows + 1) > 64)
            return fail("the board is too large for the bitboard engine (cols * (rows + 1) must be <= 64)");
        return true;
    }

    const Geometry &Geometry::get(int rows, int cols, int connect)
    {
        static std::mutex mutex;
        static std::map<std::tuple<int, int, int>, std::unique_ptr<Geometry>> cache;

        std::string reason;
        if (!isSupported(rows, cols, connect, &reason))
            throw std::invalid_argument("Unsupported board: " + reason);

        std::lock_guard<std::mutex> lock(mutex);
        auto key = std::make_tuple(rows, cols, connect);
        auto it = cache.find(key);
        if (it == cache.end())
            it = cache.emplace(key, std::unique_ptr<Geometry>(new Geometry(rows, cols, connect))).first;
        return *it->second;
    }

    Geometry::Geometry(int r, int c, int k)
        : rows(r),
          cols(c),
          connect(k),
          stride(r + 1),
          cells(r * c),
          board_mask(0),
          bottom_mask(0),
          center_mask(0),
          column_mask{},
          bottom_cell{},
          top_cell{},
          move_order{},
          directions{}
    {
        for (int col = 0; col < cols; ++col)
        {
            column_mask[col] = ((1ULL << rows) - 1) << (col * stride);
            bottom_cell[col] = 1ULL << (col * stride);
            top_cell[col] = 1ULL << (col * stride + rows - 1);
            board_mask |= column_mask[col];
            bottom_mask |= bottom_cell[col];
        }

        // Centre first: c, c-1, c+1, c-2, c+2 ... (central columns take part in more lines)
        for (int i = 0; i < cols; ++i)
            move_order[i] = cols / 2 + (1 - 2 * (i % 2)) * (i + 1) / 2;

        center_mask = (cols % 2 == 1) ? column_mask[cols / 2]
                                      : (column_mask[cols / 2 - 1] | column_mask[cols / 2]);

        // vertical, horizontal, diagonal "\" (down-right), diagonal "/" (up-right)
        directions = {1, stride, stride - 1, stride + 1};

        // Enumerate every line of `connect` cells
        const int steps[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}}; // (d_col, d_row)
        for (const auto &step : steps)
        {
            for (int col = 0; col < cols; ++col)
            {
                for (int row = 0; row < rows; ++row)
                {
                    const int end_col = col + step[0] * (connect - 1);
                    const int end_row = row + step[1] * (connect - 1);
                    if (end_col < 0 || end_col >= cols || end_row < 0 || end_row >= rows)
                        continue;

                    uint64_t window = 0;
                    for (int i = 0; i < connect; ++i)
                        window |= 1ULL << ((col + step[0] * i) * stride + (row + step[1] * i));
                    windows.push_back(window);
                }
            }
        }
    }

    // ========================================================================
    // Position
    // ========================================================================

    Position::Position(int rows, int cols, int connect)
        : geo_(&Geometry::get(rows, cols, connect)),
          cur_(0),
          mask_(0),
          moves_(0),
          won_(false)
    {
    }

    Position Position::fromGrid(const std::vector<std::vector<uint8_t>> &grid,
                                uint8_t current_player,
                                uint8_t opponent_player,
                                int connect)
    {
        if (grid.empty() || grid[0].empty())
            throw std::invalid_argument("Empty grid");
        if (current_player == opponent_player || current_player == 0 || opponent_player == 0)
            throw std::invalid_argument("Invalid player ids");

        const int rows = static_cast<int>(grid.size());
        const int cols = static_cast<int>(grid[0].size());

        Position position(rows, cols, connect);
        const Geometry &geo = *position.geo_;

        uint64_t current = 0;
        uint64_t opponent = 0;

        for (int row_from_top = 0; row_from_top < rows; ++row_from_top)
        {
            if (static_cast<int>(grid[row_from_top].size()) != cols)
                throw std::invalid_argument("Ragged grid");

            for (int col = 0; col < cols; ++col)
            {
                const uint8_t value = grid[row_from_top][col];
                if (value == 0)
                    continue;

                const uint64_t bit = 1ULL << (col * geo.stride + (rows - 1 - row_from_top));
                if (value == current_player)
                    current |= bit;
                else if (value == opponent_player)
                    opponent |= bit;
                else
                    throw std::invalid_argument("Unknown player id in grid");
            }
        }

        // Gravity: no empty cell below a stone
        const uint64_t occupied = current | opponent;
        for (int col = 0; col < cols; ++col)
        {
            const uint64_t column = (occupied >> (col * geo.stride)) & ((1ULL << rows) - 1);
            if ((column & (column + 1)) != 0) // column must look like 0..01..1
                throw std::invalid_argument("Floating stones in grid");
        }

        position.cur_ = current;
        position.mask_ = occupied;
        position.moves_ = popcount(occupied);
        position.won_ = position.hasConnect(opponent);
        return position;
    }

    Position Position::fromBitboards(int rows, int cols, int connect, uint64_t current, uint64_t mask)
    {
        Position position(rows, cols, connect);
        position.cur_ = current;
        position.mask_ = mask;
        position.moves_ = popcount(mask);
        position.won_ = position.hasConnect(mask ^ current);
        return position;
    }

    Position Position::fromMoves(int rows, int cols, int connect, const std::string &columns)
    {
        Position position(rows, cols, connect);

        for (char ch : columns)
        {
            int col;
            if (ch >= '1' && ch <= '9')
                col = ch - '1';
            else if (ch >= 'a' && ch <= 'z')
                col = 9 + (ch - 'a');
            else
                throw std::invalid_argument(std::string("Invalid move character: ") + ch);

            if (position.isTerminal())
                throw std::invalid_argument("Move after the end of the game");
            if (!position.canPlay(col))
                throw std::invalid_argument(std::string("Illegal move: ") + ch);

            position.play(col);
        }

        return position;
    }

    int Position::cell(int row_from_top, int col) const
    {
        const uint64_t bit = 1ULL << (col * geo_->stride + (geo_->rows - 1 - row_from_top));
        if (cur_ & bit)
            return 1;
        if (mask_ & bit)
            return 2;
        return 0;
    }

    std::vector<int> Position::legalMoves() const
    {
        std::vector<int> moves;
        moves.reserve(geo_->cols);
        for (int col = 0; col < geo_->cols; ++col)
        {
            if (canPlay(col))
                moves.push_back(col);
        }
        return moves;
    }

    bool Position::hasConnect(uint64_t stones) const
    {
        const int k = geo_->connect;

        for (int d : geo_->directions)
        {
            // Bit x survives iff x, x + d, x + 2d, ... x + (k-1)d are all set
            uint64_t m = stones;
            for (int i = 1; i < k && m != 0; ++i)
            {
                const int shift = i * d;
                m &= (shift >= 64) ? 0ULL : (stones >> shift);
            }

            if (m != 0)
                return true;
        }

        return false;
    }

    uint64_t Position::winningCells(uint64_t stones, uint64_t occupied) const
    {
        const int k = geo_->connect;
        uint64_t result = 0;

        for (int d : geo_->directions)
        {
            // behind[i] has bit x set iff x - d, x - 2d ... x - i*d are all stones
            // ahead[i]  has bit x set iff x + d, x + 2d ... x + i*d are all stones
            uint64_t behind[kMaxConnect];
            uint64_t ahead[kMaxConnect];
            behind[0] = ahead[0] = ~0ULL;

            for (int i = 1; i < k; ++i)
            {
                const int shift = i * d;
                if (shift >= 64)
                {
                    behind[i] = ahead[i] = 0;
                }
                else
                {
                    behind[i] = behind[i - 1] & (stones << shift);
                    ahead[i] = ahead[i - 1] & (stones >> shift);
                }
            }

            // The new stone can occupy any of the k slots of a line: `a` stones
            // before it and k-1-a stones after it must already be there.
            for (int a = 0; a < k; ++a)
                result |= behind[a] & ahead[k - 1 - a];
        }

        return result & geo_->board_mask & ~occupied;
    }

    uint64_t Position::nonLosingMoves() const
    {
        uint64_t possible = legalMoveMask();
        const uint64_t opponent_cells = opponentThreats();

        // The opponent threatens to win at a playable cell: we must take it
        const uint64_t forced = possible & opponent_cells;
        if (forced != 0)
        {
            if ((forced & (forced - 1)) != 0)
                return 0; // two different cells to block: lost
            possible = forced;
        }

        // Never play right below a cell where the opponent would win
        return possible & ~(opponent_cells >> 1);
    }

    Position Position::mirrored() const
    {
        Position result(*this);
        result.cur_ = 0;
        result.mask_ = 0;

        const uint64_t column_bits = (1ULL << geo_->stride) - 1;
        for (int col = 0; col < geo_->cols; ++col)
        {
            const int mirrored_col = geo_->cols - 1 - col;
            result.cur_ |= ((cur_ >> (col * geo_->stride)) & column_bits) << (mirrored_col * geo_->stride);
            result.mask_ |= ((mask_ >> (col * geo_->stride)) & column_bits) << (mirrored_col * geo_->stride);
        }

        // Mirroring cannot create or destroy a line, so `won_` is unchanged
        return result;
    }

    void Position::encode(float *out) const
    {
        const uint64_t opponent_stones = cur_ ^ mask_;
        for (int row = 0; row < geo_->rows; ++row)
        {
            for (int col = 0; col < geo_->cols; ++col)
            {
                const int bit = col * geo_->stride + (geo_->rows - 1 - row);
                float *cell_out = out + (row * geo_->cols + col) * 2;
                cell_out[0] = static_cast<float>((cur_ >> bit) & 1ULL);
                cell_out[1] = static_cast<float>((opponent_stones >> bit) & 1ULL);
            }
        }
    }

    std::string Position::toString() const
    {
        // x = the player who moved first: it is the player to move when an even number of stones is on the board
        const bool current_is_x = (moves_ % 2 == 0);

        std::string text;
        for (int row = 0; row < geo_->rows; ++row)
        {
            for (int col = 0; col < geo_->cols; ++col)
            {
                const int value = cell(row, col);
                char symbol = '.';
                if (value == 1)
                    symbol = current_is_x ? 'x' : 'o';
                else if (value == 2)
                    symbol = current_is_x ? 'o' : 'x';
                text += symbol;
                text += ' ';
            }
            text += '\n';
        }

        for (int col = 0; col < geo_->cols; ++col)
        {
            text += static_cast<char>(col < 9 ? '1' + col : 'a' + (col - 9));
            text += ' ';
        }
        text += '\n';
        return text;
    }
}
