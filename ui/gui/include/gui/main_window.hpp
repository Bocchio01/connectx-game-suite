#pragma once

#include <optional>

#include <QMainWindow>
#include <QString>

#include "gui/computer_levels.hpp"
#include "gui/helper_programs.hpp"
#include "gui/network_controller.hpp"

class ComputerPlayer;
class ConnectPage;
class GamePage;
class LobbyPage;
class PrivateServer;
class QAction;
class QCloseEvent;
class QLabel;
class QStackedWidget;

/**
 * Main window: a stack of three pages (Connect -> Lobby -> Game), a menu bar and a status bar.
 *
 * The window owns the NetworkController and is the only place that connects the pages to it:
 * the pages never know about the network, the controller never knows about the pages.
 *
 * It also runs the two helper programs of the offline features, as child processes that never outlive
 * their use: the computer opponent of a game (ComputerPlayer, started once the game exists) and the
 * private server of an offline session (PrivateServer, stopped when the session ends).
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class Page
    {
        Connect,
        Lobby,
        Game
    };

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    Page currentPage() const;

    /** Same as pressing Connect on the first page (used by --connect). */
    void connectNow();

    /** Same as pressing "Play offline against the computer" on the first page (used by --offline). */
    void offlineNow();

    /** Enables/disables the "are you sure?" dialogs (disabled by tests and automation). */
    void setConfirmationsEnabled(bool enabled) { confirmations_ = enabled; }

    /** The helper programs found when the window was created (the tests replace them). */
    const HelperPrograms &helperPrograms() const { return helpers_; }
    void setHelperPrograms(const HelperPrograms &helpers);

    /** How long the computer waits before it answers (500 ms; the tests shorten it). */
    void setComputerDelayMs(int milliseconds) { computerDelayMs_ = milliseconds; }

    /** True while the connection is to the private server of an offline game. */
    bool isOffline() const { return offline_; }

    NetworkController *controller() const { return controller_; }
    ConnectPage *connectPage() const { return connectPage_; }
    LobbyPage *lobbyPage() const { return lobbyPage_; }
    GamePage *gamePage() const { return gamePage_; }
    ComputerPlayer *computerPlayer() const { return computer_; }
    PrivateServer *privateServer() const { return privateServer_; }

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onConnectRequested(const QString &host, quint16 port, const QString &playerName);
    void onConnected(const ConnectionInfo &info);
    void onConnectionFailed(const QString &message);
    void onDisconnected(const QString &reason);
    void onJoined(const GameInfo &info, int playerId);
    void onJoinFailed(const QString &message);
    void onServerError(quint16 code, const QString &message);
    void onLeaveRequested();
    void disconnectFromServer();
    void showAbout();

    // offline games and the computer opponent
    void onOfflineRequested(const QString &playerName);
    void onPrivateServerReady(quint16 port);
    void onPrivateServerFailed(const QString &message);
    void onPrivateServerDied(const QString &message);
    void onNewGameRequested(const GameConfig &config, const QString &gameName, const OpponentChoice &opponent);
    void onPlayAgainRequested();
    void onComputerFailed(const QString &message, const QString &output);
    void onRetryComputer();

private:
    /** A game against the computer: what it takes to start it again ("Play again"). */
    struct ComputerGame
    {
        GameConfig config;
        QString gameName;
        int level = 0;
        quint32 gameId = 0;
    };

    void showPage(Page page);
    void updateConnectionStatus();
    void showNotice(const QString &message, int timeoutMs = 5000);
    void createMenus();

    void startComputer(quint32 gameId);
    void startComputerGame(const ComputerGame &game);
    void forgetComputerGame();
    void leaveOffline();

    NetworkController *controller_;
    ComputerPlayer *computer_;
    PrivateServer *privateServer_;
    QStackedWidget *stack_;
    ConnectPage *connectPage_;
    LobbyPage *lobbyPage_;
    GamePage *gamePage_;
    QLabel *connectionLabel_;
    QAction *disconnectAction_;
    HelperPrograms helpers_;
    bool confirmations_ = true;
    bool reconnecting_ = false;

    // Offline session: the connection is to a private server started by this window.
    bool offline_ = false;         // the connection is (or is being made) to the private server
    bool offlineStarting_ = false; // the private server is starting
    bool openNewGameAfterConnect_ = false;
    QString offlineName_;

    // Games against the computer.
    int computerDelayMs_ = 500;
    std::optional<ComputerGame> pendingComputer_; // asked for in the New game dialog, not joined yet
    std::optional<ComputerGame> computerGame_;    // the game on the game page is against the computer
    std::optional<ComputerGame> rematch_;         // "Play again" was pressed: starts when the new connection is ready
};
