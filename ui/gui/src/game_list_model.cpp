#include <algorithm>

#include <QGuiApplication>
#include <QPalette>

#include "gui/game_list_model.hpp"

namespace
{
    bool sameGame(const GameInfo &a, const GameInfo &b)
    {
        return a.game_id == b.game_id && a.game_name == b.game_name && a.current_players == b.current_players &&
               a.status == b.status && a.config.rows == b.config.rows && a.config.cols == b.config.cols &&
               a.config.num_players == b.config.num_players && a.config.connect_length == b.config.connect_length;
    }

    QString statusText(const GameInfo &game)
    {
        switch (game.status)
        {
        case ProtocolGameStatus::NOT_STARTED:
            return GameListModel::tr("Waiting for players");
        case ProtocolGameStatus::IN_PROGRESS:
            return GameListModel::tr("In progress");
        case ProtocolGameStatus::FINISHED_WIN:
        case ProtocolGameStatus::FINISHED_DRAW:
            return GameListModel::tr("Finished");
        }
        return QString();
    }
}

GameListModel::GameListModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

bool GameListModel::isJoinable(const GameInfo &game)
{
    return game.status == ProtocolGameStatus::NOT_STARTED && game.current_players < game.config.num_players;
}

void GameListModel::setGames(const QList<GameInfo> &incoming)
{
    QList<GameInfo> games = incoming;
    std::sort(games.begin(), games.end(), [](const GameInfo &a, const GameInfo &b)
              { return a.game_id < b.game_id; });

    // 1. Drop the rows that disappeared (bottom up, so the remaining row numbers stay valid).
    for (int row = static_cast<int>(games_.size()) - 1; row >= 0; --row)
    {
        const quint32 id = games_[row].game_id;
        const bool stillThere = std::any_of(games.begin(), games.end(), [id](const GameInfo &g)
                                            { return g.game_id == id; });
        if (!stillThere)
        {
            beginRemoveRows(QModelIndex(), row, row);
            games_.removeAt(row);
            endRemoveRows();
        }
    }

    // 2. Walk the new list in order: update the rows that exist, insert the new ones.
    for (int i = 0; i < games.size(); ++i)
    {
        const GameInfo &game = games[i];
        if (i < games_.size() && games_[i].game_id == game.game_id)
        {
            if (!sameGame(games_[i], game))
            {
                games_[i] = game;
                emit dataChanged(index(i, 0), index(i, ColumnCount - 1));
            }
        }
        else
        {
            beginInsertRows(QModelIndex(), i, i);
            games_.insert(i, game);
            endInsertRows();
        }
    }
}

const GameInfo *GameListModel::gameAt(int row) const
{
    if (row < 0 || row >= games_.size())
    {
        return nullptr;
    }
    return &games_[row];
}

int GameListModel::rowForId(quint32 gameId) const
{
    for (int row = 0; row < games_.size(); ++row)
    {
        if (games_[row].game_id == gameId)
        {
            return row;
        }
    }
    return -1;
}

int GameListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(games_.size());
}

int GameListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant GameListModel::data(const QModelIndex &index, int role) const
{
    const GameInfo *game = index.isValid() ? gameAt(index.row()) : nullptr;
    if (game == nullptr)
    {
        return QVariant();
    }

    const bool joinable = isJoinable(*game);

    switch (role)
    {
    case Qt::DisplayRole:
        switch (index.column())
        {
        case IdColumn:
            return QString::number(game->game_id);
        case NameColumn:
            return game->game_name.empty() ? tr("(unnamed)") : QString::fromStdString(game->game_name);
        case BoardColumn:
            return tr("%1 × %2").arg(static_cast<int>(game->config.rows)).arg(static_cast<int>(game->config.cols));
        case PlayersColumn:
            return tr("%1 / %2").arg(static_cast<int>(game->current_players)).arg(static_cast<int>(game->config.num_players));
        case ConnectColumn:
            return QString::number(static_cast<int>(game->config.connect_length));
        case StatusColumn:
            return statusText(*game);
        }
        break;

    case Qt::TextAlignmentRole:
        if (index.column() == NameColumn || index.column() == StatusColumn)
        {
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignCenter);

    case Qt::ForegroundRole:
        // Games that cannot be joined are shown in the "disabled" colour; the unnamed marker is muted.
        if (!joinable)
        {
            return QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text);
        }
        break;

    case Qt::ToolTipRole:
        if (joinable)
        {
            return tr("Double-click to join this game");
        }
        return game->status == ProtocolGameStatus::NOT_STARTED ? tr("This game is full") : tr("This game has already started");

    case GameIdRole:
        return game->game_id;

    case JoinableRole:
        return joinable;

    default:
        break;
    }

    return QVariant();
}

QVariant GameListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
    {
        return QVariant();
    }

    if (role == Qt::DisplayRole)
    {
        switch (section)
        {
        case IdColumn:
            return tr("ID");
        case NameColumn:
            return tr("Name");
        case BoardColumn:
            return tr("Board");
        case PlayersColumn:
            return tr("Players");
        case ConnectColumn:
            return tr("Connect");
        case StatusColumn:
            return tr("Status");
        }
    }
    else if (role == Qt::TextAlignmentRole)
    {
        if (section == NameColumn || section == StatusColumn)
        {
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignCenter);
    }

    return QVariant();
}
