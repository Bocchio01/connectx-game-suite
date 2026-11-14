#include <algorithm>
#include <cstddef>

#include "gui/game_logic.hpp"

namespace GameLogic
{
    Board makeEmptyBoard(int rows, int cols)
    {
        if (rows <= 0 || cols <= 0)
        {
            return {};
        }
        return Board(static_cast<std::size_t>(rows), std::vector<uint8_t>(static_cast<std::size_t>(cols), 0));
    }

    bool hasShape(const Board &board, int rows, int cols)
    {
        if (rows < 0 || cols < 0 || board.size() != static_cast<std::size_t>(rows))
        {
            return false;
        }
        return std::all_of(board.begin(), board.end(), [cols](const std::vector<uint8_t> &row)
                           { return row.size() == static_cast<std::size_t>(cols); });
    }

    int landingRow(const Board &board, int col)
    {
        if (col < 0)
        {
            return -1;
        }
        for (int row = static_cast<int>(board.size()) - 1; row >= 0; --row)
        {
            const auto &line = board[static_cast<std::size_t>(row)];
            if (static_cast<std::size_t>(col) >= line.size())
            {
                return -1;
            }
            if (line[static_cast<std::size_t>(col)] == 0)
            {
                return row;
            }
        }
        return -1;
    }

    int countDiscs(const Board &board)
    {
        int count = 0;
        for (const auto &row : board)
        {
            count += static_cast<int>(std::count_if(row.begin(), row.end(), [](uint8_t v)
                                                    { return v != 0; }));
        }
        return count;
    }

    std::vector<std::vector<Cell>> findWinningLines(const Board &board, uint8_t player, int connectLength)
    {
        std::vector<std::vector<Cell>> lines;
        if (player == 0 || connectLength < 1 || board.empty())
        {
            return lines;
        }

        const int rows = static_cast<int>(board.size());

        // Value of a cell, or -1 outside the board (tolerates ragged boards).
        auto at = [&](int r, int c) -> int
        {
            if (r < 0 || r >= rows)
            {
                return -1;
            }
            const auto &line = board[static_cast<std::size_t>(r)];
            if (c < 0 || static_cast<std::size_t>(c) >= line.size())
            {
                return -1;
            }
            return line[static_cast<std::size_t>(c)];
        };

        // (row step, column step): horizontal, vertical, diagonal down-right, diagonal up-right.
        static const int directions[4][2] = {{0, 1}, {1, 0}, {1, 1}, {-1, 1}};

        for (const auto &dir : directions)
        {
            for (int r = 0; r < rows; ++r)
            {
                const int cols = static_cast<int>(board[static_cast<std::size_t>(r)].size());
                for (int c = 0; c < cols; ++c)
                {
                    if (at(r, c) != player)
                    {
                        continue;
                    }
                    // Only start a run at its first disc, so that every maximal run is reported once.
                    if (at(r - dir[0], c - dir[1]) == player)
                    {
                        continue;
                    }

                    std::vector<Cell> run;
                    for (int rr = r, cc = c; at(rr, cc) == player; rr += dir[0], cc += dir[1])
                    {
                        run.push_back(Cell{rr, cc});
                    }

                    if (static_cast<int>(run.size()) >= connectLength)
                    {
                        lines.push_back(std::move(run));
                    }
                }
            }
        }

        return lines;
    }

    std::vector<Cell> findWinningCells(const Board &board, uint8_t player, int connectLength)
    {
        std::vector<Cell> cells;
        for (const auto &line : findWinningLines(board, player, connectLength))
        {
            cells.insert(cells.end(), line.begin(), line.end());
        }
        std::sort(cells.begin(), cells.end());
        cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
        return cells;
    }

    std::optional<std::vector<DiscChange>> findNewDiscs(const Board &prev, const Board &next)
    {
        if (prev.size() != next.size())
        {
            return std::nullopt;
        }

        std::vector<DiscChange> changes;
        for (std::size_t r = 0; r < next.size(); ++r)
        {
            if (prev[r].size() != next[r].size())
            {
                return std::nullopt;
            }
            for (std::size_t c = 0; c < next[r].size(); ++c)
            {
                const uint8_t before = prev[r][c];
                const uint8_t after = next[r][c];
                if (before == after)
                {
                    continue;
                }
                if (before != 0)
                {
                    return std::nullopt; // a disc was removed or replaced: not an incremental change
                }
                changes.push_back(DiscChange{Cell{static_cast<int>(r), static_cast<int>(c)}, after});
            }
        }

        return changes;
    }
}
