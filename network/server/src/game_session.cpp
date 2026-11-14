#include "server/game_session.hpp"
#include <algorithm>

GameSession::GameSession(
    uint32_t id, uint8_t rows, uint8_t cols, uint8_t num_players, uint8_t connect_length,
    const std::string& name)
    : id_(id)
    , name_(name)
    , engine_(rows, cols, num_players, connect_length)
    , max_players_(num_players)
{
}

uint8_t GameSession::addPlayer(uint32_t connection_id)
{
    if (isFull()) return 0;

    // The lowest id nobody holds: a player who left the lobby gives his id to the next one who joins
    // (ids must stay in 1..max_players, the engine only gives turns to those).
    uint8_t player_id = 1;
    while (std::any_of(connection_to_player_.begin(), connection_to_player_.end(),
                       [player_id](const auto& entry) { return entry.second == player_id; }))
    {
        ++player_id;
    }
    connections_.push_back(connection_id);
    connection_to_player_[connection_id] = player_id;

    // Start game when all players joined
    if (connections_.size() == max_players_) engine_.startGame();

    return player_id;
}

void GameSession::removePlayer(uint32_t connection_id)
{
    auto it = std::find(connections_.begin(), connections_.end(), connection_id);
    if (it != connections_.end()) connections_.erase(it);
    connection_to_player_.erase(connection_id);
}

std::optional<uint8_t> GameSession::getPlayerId(uint32_t connection_id) const
{
    auto it = connection_to_player_.find(connection_id);
    if (it != connection_to_player_.end()) return it->second;
    return std::nullopt;
}