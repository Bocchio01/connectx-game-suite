// Portable Connect-X engine for the Arduino sketch.
//
// Plain C++11 with no library dependencies, so that the very same file is compiled by the Arduino
// toolchain and by the unit tests on the PC (ai/tests/test_arduino_board.cpp checks it against the
// reference engine and against the encoders used to train the network).
//
// It does three things:
//   - the game: drop stones, detect lines, detect a full board;
//   - the encoders: turn a position into the input vector of the network, exactly like
//     ai/src/encoding.cpp (raw, tactical and windows encodings);
//   - the move choice: pick the best legal column from the scores of the network, with the optional
//     "tactics shield" (always take an immediate win, always block an immediate loss).
#pragma once

#include <stdint.h>

#ifndef CONNECTX_ROWS
#define CONNECTX_ROWS 6
#endif
#ifndef CONNECTX_COLS
#define CONNECTX_COLS 7
#endif
#ifndef CONNECTX_CONNECT
#define CONNECTX_CONNECT 4
#endif

namespace connectx
{
    constexpr int ROWS = CONNECTX_ROWS;
    constexpr int COLS = CONNECTX_COLS;
    constexpr int CONNECT = CONNECTX_CONNECT;

    // Network input encodings (see ai/include/ai/encoding.hpp)
    enum Encoding : int
    {
        ENCODING_RAW = 0,      // 2 planes: my stones, the opponent's stones
        ENCODING_TACTICAL = 1, // + cells where I / the opponent complete a line + playable cells
        ENCODING_WINDOWS = 2   // raw planes + 2 numbers for every line of CONNECT cells
    };

    // Number of lines of CONNECT cells on the board (the "windows" of the windows encoding)
    constexpr int windowCount()
    {
        // horizontal + vertical + two diagonals
        return ROWS * (COLS - CONNECT + 1) + COLS * (ROWS - CONNECT + 1) + 2 * (ROWS - CONNECT + 1) * (COLS - CONNECT + 1);
    }

    constexpr int encodedSize(int encoding)
    {
        return encoding == ENCODING_RAW ? ROWS * COLS * 2 : (encoding == ENCODING_TACTICAL ? ROWS * COLS * 5 : ROWS * COLS * 2 + 2 * windowCount());
    }

    constexpr int MAX_ENCODED_SIZE = ROWS * COLS * 5 > ROWS * COLS * 2 + 2 * windowCount() ? ROWS * COLS * 5 : ROWS * COLS * 2 + 2 * windowCount();

    struct Board
    {
        uint8_t cells[ROWS][COLS]; // row 0 is the TOP row; 0 = empty, 1 = first player, 2 = second player
        uint8_t player;            // who has to move: 1 or 2
        uint8_t winner;            // 0 = nobody (yet)
        uint8_t moves;             // stones on the board

        Board() { reset(); }

        void reset()
        {
            for (int r = 0; r < ROWS; ++r)
                for (int c = 0; c < COLS; ++c)
                    cells[r][c] = 0;
            player = 1;
            winner = 0;
            moves = 0;
        }

        bool canPlay(int col) const { return col >= 0 && col < COLS && cells[0][col] == 0; }
        bool isFull() const { return moves == ROWS * COLS; }
        bool isOver() const { return winner != 0 || isFull(); }

        // The row where a stone dropped in `col` lands, or -1 if the column is full
        int landingRow(int col) const
        {
            for (int r = ROWS - 1; r >= 0; --r)
                if (cells[r][col] == 0)
                    return r;
            return -1;
        }

        // Would a stone of `who` at (row, col) be part of a line of CONNECT or more? Only the neighbouring
        // cells are examined (the content of (row, col) itself is ignored): it answers both "would placing
        // a stone here complete a line?" for an empty cell and "does the stone just placed here win?".
        bool completesLine(int row, int col, uint8_t who) const
        {
            static const int8_t steps[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
            for (int d = 0; d < 4; ++d)
            {
                int run = 1;
                for (int sign = -1; sign <= 1; sign += 2)
                {
                    int r = row + sign * steps[d][0];
                    int c = col + sign * steps[d][1];
                    while (r >= 0 && r < ROWS && c >= 0 && c < COLS && cells[r][c] == who)
                    {
                        ++run;
                        r += sign * steps[d][0];
                        c += sign * steps[d][1];
                    }
                }
                if (run >= CONNECT)
                    return true;
            }
            return false;
        }

        // Does the player to move win at once by playing `col`?
        bool winsByPlaying(int col) const
        {
            if (!canPlay(col))
                return false;
            return completesLine(landingRow(col), col, player);
        }

        // Drops a stone for the player to move. Returns true if it wins the game. Precondition: canPlay(col).
        bool play(int col)
        {
            const int row = landingRow(col);
            cells[row][col] = player;
            ++moves;
            if (completesLine(row, col, player))
                winner = player;
            player = static_cast<uint8_t>(3 - player);
            return winner != 0;
        }

        // The content of a cell from the point of view of the player to move: 0 empty, 1 mine, 2 the opponent's
        uint8_t relative(int row, int col) const
        {
            const uint8_t value = cells[row][col];
            if (value == 0)
                return 0;
            return value == player ? 1 : 2;
        }
    };

    // ------------------------------------------------------------------------------------------
    // Encoders. `out` receives encodedSize(encoding) floats, laid out as NHWC:
    // out[(row * COLS + col) * planes + plane], row 0 = top, always from the point of view of the
    // player to move ("mine" = the player to move).
    // ------------------------------------------------------------------------------------------

    inline void encodeBoard(const Board &board, int encoding, float *out)
    {
        const uint8_t me = board.player;
        const uint8_t them = static_cast<uint8_t>(3 - me);

        if (encoding == ENCODING_TACTICAL)
        {
            for (int r = 0; r < ROWS; ++r)
            {
                for (int c = 0; c < COLS; ++c)
                {
                    float *cell = out + (r * COLS + c) * 5;
                    const uint8_t value = board.cells[r][c];
                    cell[0] = value == me ? 1.0f : 0.0f;
                    cell[1] = value == them ? 1.0f : 0.0f;
                    cell[2] = (value == 0 && board.completesLine(r, c, me)) ? 1.0f : 0.0f;
                    cell[3] = (value == 0 && board.completesLine(r, c, them)) ? 1.0f : 0.0f;
                    cell[4] = (value == 0 && board.landingRow(c) == r) ? 1.0f : 0.0f;
                }
            }
            return;
        }

        // raw planes (also the first part of the windows encoding)
        for (int r = 0; r < ROWS; ++r)
        {
            for (int c = 0; c < COLS; ++c)
            {
                float *cell = out + (r * COLS + c) * 2;
                cell[0] = board.cells[r][c] == me ? 1.0f : 0.0f;
                cell[1] = board.cells[r][c] == them ? 1.0f : 0.0f;
            }
        }

        if (encoding == ENCODING_WINDOWS)
        {
            // Lines in the order of the C++ code: direction by direction (vertical, horizontal, "\", "/"),
            // then by column, then by row counted from the bottom
            static const int8_t steps[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}}; // (d_col, d_row from the bottom)
            float *window_out = out + ROWS * COLS * 2;
            for (int d = 0; d < 4; ++d)
            {
                for (int c = 0; c < COLS; ++c)
                {
                    for (int row_from_bottom = 0; row_from_bottom < ROWS; ++row_from_bottom)
                    {
                        const int end_col = c + steps[d][0] * (CONNECT - 1);
                        const int end_row = row_from_bottom + steps[d][1] * (CONNECT - 1);
                        if (end_col < 0 || end_col >= COLS || end_row < 0 || end_row >= ROWS)
                            continue;

                        int mine = 0, theirs = 0;
                        for (int i = 0; i < CONNECT; ++i)
                        {
                            const int r = ROWS - 1 - (row_from_bottom + steps[d][1] * i);
                            const int cc = c + steps[d][0] * i;
                            if (board.cells[r][cc] == me)
                                ++mine;
                            else if (board.cells[r][cc] == them)
                                ++theirs;
                        }
                        *window_out++ = theirs == 0 ? static_cast<float>(mine) / CONNECT : 0.0f;
                        *window_out++ = mine == 0 ? static_cast<float>(theirs) / CONNECT : 0.0f;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // Move choice
    // ------------------------------------------------------------------------------------------

    // The 1-move tactics shield: the leftmost column that wins at once, else the leftmost column that
    // blocks an immediate win of the opponent, else -1.
    inline int tacticalColumn(const Board &board)
    {
        for (int c = 0; c < COLS; ++c)
            if (board.winsByPlaying(c))
                return c;

        Board opponent = board;
        opponent.player = static_cast<uint8_t>(3 - board.player);
        for (int c = 0; c < COLS; ++c)
            if (board.canPlay(c) && opponent.winsByPlaying(c))
                return c;
        return -1;
    }

    // The legal column with the highest score (ties: the leftmost); with `shield`, wins and blocks first.
    // Returns -1 if the board is full.
    inline int chooseColumn(const Board &board, const float *scores, bool shield)
    {
        if (shield)
        {
            const int forced = tacticalColumn(board);
            if (forced >= 0)
                return forced;
        }

        int best = -1;
        for (int c = 0; c < COLS; ++c)
        {
            if (board.canPlay(c) && (best < 0 || scores[c] > scores[best]))
                best = c;
        }
        return best;
    }

    // ------------------------------------------------------------------------------------------
    // Search with the network as evaluation function (what the C++ agent "nnsearch" does)
    //
    // Classic negamax with alpha-beta pruning. The network is asked the only question search cannot
    // answer by itself: "how good is this position for the player to move?" (its value head, in
    // (-1, 1)). Everything that can be decided exactly is decided exactly, without the network:
    // an immediate win ends the search of a node, and moves that let the opponent win at once are
    // never searched.
    // ------------------------------------------------------------------------------------------

    constexpr float WIN_SCORE = 1.0f;      // a proven win; faster wins score a little higher
    constexpr float PLY_PENALTY = 0.001f;
    constexpr float NETWORK_SCALE = 0.9f;  // the network's opinion never outweighs a proven result
    constexpr float SEARCH_INFINITY = 10.0f;

    // Columns from the centre outwards: the central columns are tried first
    inline int centreColumn(int i)
    {
        return COLS / 2 + (1 - 2 * (i % 2)) * (i + 1) / 2;
    }

    // `evaluator(board)` must return the value in (-1, 1) of the position for the player to move.
    template <typename Evaluator>
    float negamax(const Board &board, int depth, float alpha, float beta, int ply, Evaluator &evaluator)
    {
        if (board.isFull())
            return 0.0f;

        // The player to move wins at once?
        for (int c = 0; c < COLS; ++c)
        {
            if (board.winsByPlaying(c))
                return WIN_SCORE - PLY_PENALTY * static_cast<float>(ply + 1);
        }

        // Moves after which the opponent cannot win at once
        int safe[COLS];
        int safeCount = 0;
        for (int i = 0; i < COLS; ++i)
        {
            const int c = centreColumn(i);
            if (!board.canPlay(c))
                continue;

            Board child = board;
            child.play(c);

            bool opponentWins = false;
            for (int o = 0; o < COLS && !opponentWins; ++o)
                opponentWins = child.winsByPlaying(o);
            if (!opponentWins)
                safe[safeCount++] = c;
        }

        if (safeCount == 0)
            return -(WIN_SCORE - PLY_PENALTY * static_cast<float>(ply + 2)); // every move loses

        if (depth == 0)
            return NETWORK_SCALE * evaluator(board);

        float best = -SEARCH_INFINITY;
        for (int k = 0; k < safeCount; ++k)
        {
            Board child = board;
            child.play(safe[k]);

            const float score = -negamax(child, depth - 1, -beta, -alpha, ply + 1, evaluator);
            if (score > best)
                best = score;
            if (score > alpha)
                alpha = score;
            if (alpha >= beta)
                break;
        }
        return best;
    }

    // The best column for the player to move with a search of `depth` plies (>= 1); `scores` (COLS floats)
    // receives the score of every column (-SEARCH_INFINITY for full ones). Returns -1 if the board is full.
    template <typename Evaluator>
    int searchColumn(const Board &board, int depth, Evaluator &evaluator, float *scores)
    {
        int bestColumn = -1;
        float bestScore = -SEARCH_INFINITY;

        for (int c = 0; c < COLS; ++c)
            scores[c] = -SEARCH_INFINITY;

        for (int i = 0; i < COLS; ++i)
        {
            const int c = centreColumn(i);
            if (!board.canPlay(c))
                continue;

            Board child = board;
            const bool won = child.play(c);
            const float score = won ? WIN_SCORE - PLY_PENALTY
                                    : -negamax(child, depth - 1, -SEARCH_INFINITY, SEARCH_INFINITY, 1, evaluator);
            scores[c] = score;

            if (score > bestScore + 1e-6f)
            {
                bestScore = score;
                bestColumn = c;
            }
        }
        return bestColumn;
    }
}
