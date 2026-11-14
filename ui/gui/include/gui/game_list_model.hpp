#pragma once

#include <QAbstractTableModel>
#include <QList>

#include "protocol/protocol.hpp"

#include "gui/metatypes.hpp"

/**
 * Table model of the games shown in the lobby.
 *
 * setGames() merges the new list into the current one (rows are inserted, removed or updated
 * in place, keyed by game id), so the view keeps its selection and scroll position.
 */
class GameListModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        IdColumn,
        NameColumn,
        BoardColumn,
        PlayersColumn,
        ConnectColumn,
        StatusColumn,
        ColumnCount
    };

    enum Role
    {
        GameIdRole = Qt::UserRole + 1, // quint32
        JoinableRole                   // bool
    };

    explicit GameListModel(QObject *parent = nullptr);

    /** A game can be joined while it waits for players and has free seats. */
    static bool isJoinable(const GameInfo &game);

    void setGames(const QList<GameInfo> &games);

    /** Game shown in a row, or nullptr. */
    const GameInfo *gameAt(int row) const;

    /** Row of a game id, or -1. */
    int rowForId(quint32 gameId) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    QList<GameInfo> games_;
};
