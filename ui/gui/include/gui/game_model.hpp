#pragma once

#include <vector>

#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

#include "protocol/protocol.hpp"

#include "gui/game_logic.hpp"

/** One seat of the game, as shown in the player list. */
struct PlayerEntry
{
    int id = 0;             // 1-based player id (selects colour and symbol)
    QString name;           // empty while the seat is still free
    QString placeholder;    // what to show instead of a name while the seat is free (empty: the default text)
    bool isMe = false;
    bool present = false;   // currently connected to the game
    bool left = false;      // joined earlier but disconnected since
    bool isCurrent = false; // it is this player's turn
    bool isWinner = false;
};

/**
 * Everything the game page needs to know about the game it is in, derived from the
 * join response and from the stream of GameStateUpdate messages. Qt-widget-free and
 * tolerant to message ordering (a state update may be applied before begin()).
 */
class GameModel
{
    Q_DECLARE_TR_FUNCTIONS(GameModel)

public:
    enum class Phase
    {
        Idle,        // not in a game
        Waiting,     // in the lobby of the game, waiting for the other players
        Playing,     // game in progress
        Won,         // finished, somebody connected enough discs
        Drawn,       // finished, the board is full
        Abandoned,   // game in progress but every other player left
        Disconnected // the connection to the server was lost
    };

    /** Forgets everything (back to Idle). */
    void reset();

    /**
     * Called when the join succeeded. States received before this call are replayed.
     * @param myName name of the local player, shown until the first state update arrives
     */
    void begin(const GameInfo &info, int myPlayerId, const QString &myName = QString());

    /**
     * Applies a state update. Returns false if it was ignored (malformed or belonging to
     * another game). Updates received before begin() are kept and applied by begin().
     */
    bool applyState(const GameStateUpdate &state);

    /** The connection to the server was lost. */
    void markDisconnected();

    /**
     * The other seat is meant for the computer (called after begin(), which forgets it): while it has not
     * joined, the texts say that the game waits for the computer rather than for "players".
     * `name` is the name the computer will use ("Computer (Easy)"); an empty name means no computer.
     */
    void setComputerOpponent(const QString &name);
    bool hasComputerOpponent() const { return !computerName_.isEmpty(); }
    const QString &computerName() const { return computerName_; }

    // --- session -----------------------------------------------------
    bool isActive() const { return active_; }
    int myPlayerId() const { return myId_; }
    const GameInfo &info() const { return info_; }
    bool connectionLost() const { return disconnected_; }

    // --- board -------------------------------------------------------
    int rows() const { return static_cast<int>(board_.size()); }
    int cols() const { return board_.empty() ? 0 : static_cast<int>(board_.front().size()); }
    int connectLength() const { return info_.config.connect_length; }
    int numPlayers() const { return info_.config.num_players; }
    const GameLogic::Board &board() const { return board_; }

    /** True when the last applied state was the first one of this game: it must not be animated. */
    bool isFirstState() const { return acceptedStates_ == 1; }

    /** Winning runs of the final board (empty unless somebody won). */
    const std::vector<std::vector<GameLogic::Cell>> &winningLines() const { return winningLines_; }

    // --- derived state -----------------------------------------------
    Phase phase() const;
    bool isFinished() const;      // Won or Drawn
    bool isInProgress() const;    // Playing (a game that costs something to leave)
    bool canMove() const;         // my turn in a game in progress
    int currentPlayer() const;    // 0 if unknown
    int winnerId() const;         // 0 unless Won
    QString playerName(int id) const;
    QList<PlayerEntry> players() const;
    int presentPlayers() const;

    QString statusText() const;
    QString bannerTitle() const;
    QString bannerSubtitle() const;

private:
    bool acceptState(const GameStateUpdate &state);

    bool active_ = false;
    bool disconnected_ = false;
    int myId_ = 0;
    QString computerName_;
    GameInfo info_;

    bool hasState_ = false;
    GameStateUpdate state_;
    GameLogic::Board board_;
    int acceptedStates_ = 0;
    QMap<int, QString> names_; // every name seen so far, by player id
    QSet<int> present_;
    std::vector<std::vector<GameLogic::Cell>> winningLines_;
    QList<GameStateUpdate> early_; // states that arrived before begin()
};
