#pragma once

#include <QList>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "protocol/protocol.hpp"

#include "gui/metatypes.hpp"

#include "gui/game_model.hpp"

class BoardWidget;
class CardFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QWidget;

/**
 * Third page: the board and a side panel with the game info, the status line and the players.
 *
 * The page is a view of a GameModel: it only reacts to join/state/connection events and to the
 * user choosing a column, and never talks to the network itself.
 */
class GamePage : public QWidget
{
    Q_OBJECT

public:
    explicit GamePage(QWidget *parent = nullptr);

    /**
     * The join succeeded: shows an empty board and the waiting overlay. `computerName` (the name of the
     * computer opponent, e.g. "Computer (Easy)") tells that the game waits for the computer, not for people.
     */
    void startGame(const GameInfo &info, int playerId, const QString &playerName, const QString &computerName = QString());

    /** A new state of the game arrived. Safe to call before startGame() (the state is kept). */
    void applyState(const GameStateUpdate &state);

    /** The connection to the server was lost. */
    void setConnectionLost();

    /** Shows a short message in the side panel (e.g. a refused move) and gives the turn back. */
    void showMessage(const QString &message);

    /** True when the game is against the computer (see startGame()). */
    bool hasComputerOpponent() const { return model_.hasComputerOpponent(); }

    /**
     * The computer opponent could not be started, or stopped before the game began: shows `message` and the
     * last lines of its `output`, and offers "Retry" (retryComputerRequested()) or going back to the lobby.
     */
    void showComputerProblem(const QString &message, const QString &output);
    void clearComputerProblem();
    bool hasComputerProblem() const { return !computerProblem_.isEmpty(); }

    /** True while leaving the game costs something (a game in progress): ask before leaving. */
    bool needsLeaveConfirmation() const { return model_.isInProgress(); }

    const GameModel &model() const { return model_; }
    BoardWidget *board() const { return board_; }
    QString statusText() const;

signals:
    /** The user chose a column of the board. */
    void moveRequested(int column);

    /** The user wants to leave the game (or go back to the lobby once it is over). */
    void leaveRequested();

    /** The game against the computer is over and the user wants another one with the same settings. */
    void playAgainRequested();

    /** The user asks to start the computer opponent again (after a problem). */
    void retryComputerRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onColumnActivated(int column);

private:
    class PlayerRow;

    void refresh(bool animate);
    void rebuildPlayerRows(int count);
    void applyColors();
    QString hintText() const;

    GameModel model_;
    BoardWidget *board_;
    QLabel *titleLabel_;
    QLabel *detailsLabel_;
    QLabel *statusLabel_;
    QLabel *messageLabel_;
    QLabel *hintLabel_;
    QVBoxLayout *playersLayout_;
    QList<PlayerRow *> rows_;
    QPushButton *leaveButton_;
    QPushButton *lobbyButton_;
    QPushButton *playAgainButton_;
    QWidget *problemBox_;
    QLabel *problemDetailsLabel_;
    QPlainTextEdit *problemOutput_;
    QPushButton *retryButton_;
    QTimer messageTimer_;

    bool movePending_ = false;
    bool wasMyTurn_ = false;
    QString overlayKey_;
    QString computerProblem_; // non-empty while the computer opponent has a problem
};
