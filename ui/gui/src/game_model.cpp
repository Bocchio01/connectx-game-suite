#include <algorithm>

#include "gui/game_model.hpp"

namespace
{
    constexpr int kMaxEarlyStates = 32;
}

void GameModel::reset()
{
    active_ = false;
    disconnected_ = false;
    myId_ = 0;
    computerName_.clear();
    info_ = GameInfo();
    hasState_ = false;
    state_ = GameStateUpdate();
    board_.clear();
    acceptedStates_ = 0;
    names_.clear();
    present_.clear();
    winningLines_.clear();
    early_.clear();
}

void GameModel::begin(const GameInfo &info, int myPlayerId, const QString &myName)
{
    const QList<GameStateUpdate> early = early_;

    reset();
    active_ = true;
    info_ = info;
    myId_ = myPlayerId;
    board_ = GameLogic::makeEmptyBoard(info.config.rows, info.config.cols);

    if (!myName.isEmpty())
    {
        names_[myPlayerId] = myName;
    }
    present_.insert(myPlayerId);

    for (const GameStateUpdate &state : early)
    {
        acceptState(state);
    }
}

bool GameModel::applyState(const GameStateUpdate &state)
{
    if (!active_)
    {
        // The state overtook the join notification: keep it, begin() will apply it.
        if (early_.size() >= kMaxEarlyStates)
        {
            early_.removeFirst();
        }
        early_.push_back(state);
        return true;
    }
    return acceptState(state);
}

bool GameModel::acceptState(const GameStateUpdate &state)
{
    if (state.game_id != info_.game_id)
    {
        return false;
    }
    if (state.rows == 0 || state.cols == 0 || !GameLogic::hasShape(state.board, state.rows, state.cols))
    {
        return false;
    }

    present_.clear();
    const std::size_t named = std::min(state.players.size(), state.player_names.size());
    for (std::size_t i = 0; i < state.players.size(); ++i)
    {
        const int id = state.players[i];
        present_.insert(id);
        if (i < named)
        {
            names_[id] = QString::fromStdString(state.player_names[i]);
        }
    }

    state_ = state;
    hasState_ = true;
    board_ = state.board;
    ++acceptedStates_;

    winningLines_.clear();
    if (state.status == ProtocolGameStatus::FINISHED_WIN && state.winner.has_value())
    {
        winningLines_ = GameLogic::findWinningLines(board_, *state.winner, connectLength());
    }

    return true;
}

void GameModel::markDisconnected()
{
    disconnected_ = true;
}

void GameModel::setComputerOpponent(const QString &name)
{
    computerName_ = name;
}

int GameModel::presentPlayers() const
{
    if (!active_)
    {
        return 0;
    }
    if (hasState_)
    {
        return static_cast<int>(present_.size());
    }
    return std::max<int>(1, info_.current_players);
}

GameModel::Phase GameModel::phase() const
{
    if (!active_)
    {
        return Phase::Idle;
    }

    Phase phase = Phase::Waiting;
    if (hasState_)
    {
        switch (state_.status)
        {
        case ProtocolGameStatus::NOT_STARTED:
            phase = Phase::Waiting;
            break;
        case ProtocolGameStatus::IN_PROGRESS:
            phase = presentPlayers() < 2 ? Phase::Abandoned : Phase::Playing;
            break;
        case ProtocolGameStatus::FINISHED_WIN:
            phase = Phase::Won;
            break;
        case ProtocolGameStatus::FINISHED_DRAW:
            phase = Phase::Drawn;
            break;
        }
    }

    // A finished game keeps showing its result even if the connection is gone afterwards.
    if (disconnected_ && phase != Phase::Won && phase != Phase::Drawn)
    {
        return Phase::Disconnected;
    }
    return phase;
}

bool GameModel::isFinished() const
{
    const Phase p = phase();
    return p == Phase::Won || p == Phase::Drawn;
}

bool GameModel::isInProgress() const
{
    return phase() == Phase::Playing;
}

bool GameModel::canMove() const
{
    return phase() == Phase::Playing && myId_ != 0 && currentPlayer() == myId_;
}

int GameModel::currentPlayer() const
{
    return hasState_ ? state_.current_player : 0;
}

int GameModel::winnerId() const
{
    if (phase() == Phase::Won && state_.winner.has_value())
    {
        return *state_.winner;
    }
    return 0;
}

QString GameModel::playerName(int id) const
{
    const QString name = names_.value(id);
    return name.isEmpty() ? tr("Player %1").arg(id) : name;
}

QList<PlayerEntry> GameModel::players() const
{
    QList<PlayerEntry> entries;
    if (!active_)
    {
        return entries;
    }

    // Normally 1..num_players; widen it if the server ever reports a higher id.
    int seats = info_.config.num_players;
    for (auto it = names_.constBegin(); it != names_.constEnd(); ++it)
    {
        seats = std::max(seats, it.key());
    }
    for (int id : present_)
    {
        seats = std::max(seats, id);
    }

    const Phase current = phase();
    const bool finished = (current == Phase::Won || current == Phase::Drawn);
    for (int id = 1; id <= seats; ++id)
    {
        PlayerEntry entry;
        entry.id = id;
        entry.name = names_.value(id);
        entry.isMe = (id == myId_);

        // Once the game is over nobody is "left": the computer, for one, always ends right after its game,
        // and who stays connected to a finished game says nothing.
        entry.present = present_.contains(id) || (finished && names_.contains(id));
        entry.left = !entry.present && names_.contains(id);
        entry.isCurrent = (current == Phase::Playing && currentPlayer() == id);
        entry.isWinner = (current == Phase::Won && winnerId() == id);
        if (entry.name.isEmpty() && !entry.isMe && hasComputerOpponent())
        {
            entry.placeholder = tr("Waiting for the computer…");
        }
        entries.push_back(entry);
    }
    return entries;
}

QString GameModel::statusText() const
{
    switch (phase())
    {
    case Phase::Idle:
        return QString();
    case Phase::Waiting:
        if (hasComputerOpponent())
        {
            return tr("Waiting for the computer…");
        }
        return tr("Waiting for players (%1/%2)…").arg(presentPlayers()).arg(numPlayers());
    case Phase::Playing:
        if (canMove())
        {
            return tr("Your turn");
        }
        return tr("Waiting for %1…").arg(playerName(currentPlayer()));
    case Phase::Won:
        return winnerId() == myId_ ? tr("You win!") : tr("%1 wins").arg(playerName(winnerId()));
    case Phase::Drawn:
        return tr("It's a draw");
    case Phase::Abandoned:
        return tr("All other players left");
    case Phase::Disconnected:
        return tr("Connection lost");
    }
    return QString();
}

QString GameModel::bannerTitle() const
{
    switch (phase())
    {
    case Phase::Waiting:
        return hasComputerOpponent() ? tr("Waiting for the computer") : tr("Waiting for players");
    case Phase::Won:
        return winnerId() == myId_ ? tr("You win!") : tr("%1 wins!").arg(playerName(winnerId()));
    case Phase::Drawn:
        return tr("Draw");
    case Phase::Abandoned:
        return tr("Game abandoned");
    case Phase::Disconnected:
        return tr("Connection lost");
    case Phase::Idle:
    case Phase::Playing:
        break;
    }
    return QString();
}

QString GameModel::bannerSubtitle() const
{
    switch (phase())
    {
    case Phase::Waiting:
        if (hasComputerOpponent())
        {
            return tr("%1 is joining the game").arg(computerName_);
        }
        return tr("%1 of %2 players joined").arg(presentPlayers()).arg(numPlayers());
    case Phase::Won:
        return tr("%1 in a row").arg(connectLength());
    case Phase::Drawn:
        return tr("The board is full");
    case Phase::Abandoned:
        return tr("All the other players left the game");
    case Phase::Disconnected:
        return tr("The connection to the server was lost");
    case Phase::Idle:
    case Phase::Playing:
        break;
    }
    return QString();
}
