#pragma once

#include <memory>

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include "protocol/protocol.hpp"

#include "gui/metatypes.hpp"

/** Parameters of a server connection. */
struct ConnectionInfo
{
    QString host;
    quint16 port = 0;
    QString playerName;
};

Q_DECLARE_METATYPE(ConnectionInfo)

/**
 * Qt facade over the blocking network Client library.
 *
 * The controller lives in the GUI thread and is the ONLY place that touches the Client.
 * Every connection is a "session" that owns a Client, a worker thread (which executes the
 * blocking request/response calls one at a time) and the Client event thread. Results and
 * server events are delivered to the GUI thread as queued signals, so receivers never
 * need locks. Slots must be called from the GUI thread.
 *
 * Guarantees:
 *  - joined() is always emitted before the first stateUpdated() of that game, even if the
 *    server's state broadcast overtakes the join response;
 *  - signals of a session that has been ended (disconnect, new connection...) are dropped;
 *  - a request refused by the server is reported at once, with the friendly text of the error;
 *  - destroying the controller (or ending a session) never blocks for long: a request that is
 *    waiting for its answer is released by closing the connection, and the only call that cannot
 *    be interrupted, the TCP connect to a host that does not answer, is abandoned: its worker
 *    cleans itself up when the call returns (the connect gives up after a few seconds).
 *
 * One game per connection: to play another game, call reconnect() and join again.
 */
class NetworkController : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,
        Connecting,
        Connected
    };

    explicit NetworkController(QObject *parent = nullptr);
    ~NetworkController() override;

    Q_DISABLE_COPY(NetworkController)

    State state() const { return state_; }
    bool isConnected() const { return state_ == State::Connected; }

    /** Parameters of the current (or most recent) connection attempt. */
    const ConnectionInfo &connectionInfo() const { return info_; }

    /**
     * Number of abandoned worker threads that are still blocked in a TCP connect.
     * Normally 0; if it is not 0 when the program ends, exit without running static
     * destructors (the main() of the application does so).
     */
    static int stuckThreadCount();

public slots:
    /** Opens a new connection (ending the current one, if any). Emits connected() or connectionFailed(). */
    void connectToServer(const QString &host, quint16 port, const QString &playerName);

    /** Drops the current connection (silently) and opens a new one with the same parameters. */
    void reconnect();

    /** Drops the current connection. No signal is emitted: the caller asked for it. */
    void disconnectFromServer();

    /** Asks for the list of games; the answer arrives as gamesListed(). Requests are coalesced. */
    void refreshGames();

    /** Creates a game and joins it. Emits joined() or createFailed()/joinFailed(). */
    void createGame(const GameConfig &config, const QString &gameName);

    /** Joins a game by id; id 0 means "quick play". Emits joined() or joinFailed(). */
    void joinGame(quint32 gameId);

    /** Drops a disc in a column (0-based). Problems are reported with moveRejected()/serverError(). */
    void sendMove(int column);

signals:
    /** The handshake with the server succeeded. */
    void connected(const ConnectionInfo &info);

    /** The connection attempt failed (message is meant for the user). */
    void connectionFailed(const QString &message);

    /** The connection ended without being requested (server closed it, network lost...). */
    void disconnected(const QString &reason);

    void gamesListed(const QList<GameInfo> &games);
    void refreshFailed(const QString &message);

    /** A game was joined; the player id selects colour and symbol. */
    void joined(const GameInfo &info, int playerId);
    void joinFailed(const QString &message);
    void createFailed(const QString &message);

    /** New state of the game that was joined (board, turn, players, result). */
    void stateUpdated(const GameStateUpdate &state);

    /** The server refused a move. */
    void moveRejected(const QString &message);

    /** Any error message sent by the server (already translated to a friendly text). */
    void serverError(quint16 code, const QString &message);

private:
    class Session;

    void endSession();

    // Handlers of the events posted by the session threads; they run in the GUI thread.
    void handleConnected(const ConnectionInfo &info);
    void handleConnectionFailed(const QString &message);
    void handleDisconnected(const QString &reason);
    void handleGamesListed(const QList<GameInfo> &games);
    void handleRefreshFailed(const QString &message);
    void handleJoined(const GameInfo &info, int playerId);
    void handleJoinFailed(const QString &message);
    void handleCreateFailed(const QString &message);
    void handleState(const GameStateUpdate &state);
    void handleMoveRejected(const QString &message);
    void handleServerError(quint16 code, const QString &message);

    State state_ = State::Idle;
    ConnectionInfo info_;
    std::shared_ptr<Session> session_;
    quint64 sessionCounter_ = 0;
    quint64 sessionId_ = 0; // id of the live session, 0 if none
    bool refreshInFlight_ = false;
    bool joined_ = false;
    QList<GameStateUpdate> earlyStates_; // states that overtook the join response
};
