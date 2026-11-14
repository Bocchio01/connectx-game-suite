#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <QChar>
#include <QString>
#include <QStringList>

#include "protocol/protocol.hpp"

#include "gui/game_logic.hpp"

/** Builders of protocol/game data for the tests (and the gallery). */
namespace TestBuilders
{
    /** Board from ASCII art: one string per row (top first), '.' = empty, '1'..'4' = player discs. */
    inline GameLogic::Board boardFromArt(const QStringList &rows)
    {
        GameLogic::Board board;
        for (const QString &row : rows)
        {
            std::vector<uint8_t> line;
            for (const QChar ch : row)
            {
                line.push_back(ch == QLatin1Char('.') ? uint8_t(0) : static_cast<uint8_t>(ch.digitValue()));
            }
            board.push_back(line);
        }
        return board;
    }

    inline GameInfo makeInfo(uint32_t id, const std::string &name, int rows, int cols, int players, int connect, int current,
                             ProtocolGameStatus status = ProtocolGameStatus::NOT_STARTED)
    {
        GameInfo info;
        info.game_id = id;
        info.game_name = name;
        info.current_players = static_cast<uint8_t>(current);
        info.status = status;
        info.config = GameConfig(static_cast<uint8_t>(rows), static_cast<uint8_t>(cols), static_cast<uint8_t>(players), static_cast<uint8_t>(connect));
        return info;
    }

    /** State with players 1..names.size() present (use `present` to override the list of ids). */
    inline GameStateUpdate makeState(const GameInfo &info, const GameLogic::Board &board, ProtocolGameStatus status, int current,
                                     const QStringList &names, int winner = 0)
    {
        GameStateUpdate state;
        state.game_id = info.game_id;
        state.board = board;
        state.rows = static_cast<uint8_t>(board.size());
        state.cols = board.empty() ? uint8_t(0) : static_cast<uint8_t>(board.front().size());
        state.status = status;
        state.current_player = static_cast<uint8_t>(current);
        if (winner != 0)
        {
            state.winner = static_cast<uint8_t>(winner);
        }
        for (int i = 0; i < names.size(); ++i)
        {
            state.players.push_back(static_cast<uint8_t>(i + 1));
            state.player_names.push_back(names[i].toStdString());
        }
        return state;
    }
}
