#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <QMetaObject>

#include <sockpp/exception.h>

#include "client/client.hpp"
#include "gui/network_controller.hpp"

namespace
{
    // Same default as the Client library: long enough for a slow link, short enough to give feedback.
    // (A request refused by the server returns at once, and so does one that is waiting when the
    // connection is closed: the timeout only matters for a server that stays silent.)
    constexpr int kRequestTimeoutMs = 3000;

    // How long to wait for the TCP connection to a host that does not answer at all (the Client's default
    // is 10 s). The user can cancel at any moment; this only bounds the attempt that is abandoned.
    constexpr int kConnectTimeoutMs = 8000;

    // How long ending a session may block the GUI while its worker finishes. A healthy session needs
    // about one poll interval of the Client's reader (100 ms) to wind down; a worker that is still
    // busy after this is blocked in a call that cannot be interrupted: the TCP connect to a host that
    // does not answer, which ends by itself after kConnectTimeoutMs (see NetworkController::endSession).
    constexpr std::chrono::milliseconds kJoinGrace{600};

    // Sanity limit for the states buffered while waiting for the join response.
    constexpr int kMaxEarlyStates = 64;

    std::atomic<int> g_stuckThreads{0};

    /** Turns the terse server error messages into texts suitable for the user. */
    QString friendlyError(quint16 code, const QString &serverMessage)
    {
        namespace Err = Protocol::ErrorCode;

        switch (code)
        {
        case Err::GAME_NOT_FOUND:
            return NetworkController::tr("That game does not exist any more.");
        case Err::GAME_FULL:
            return NetworkController::tr("That game is already full.");
        case Err::GAME_ALREADY_STARTED:
            return NetworkController::tr("That game has already started.");
        case Err::GAME_NOT_STARTED:
            return NetworkController::tr("The game has not started yet.");
        case Err::NOT_YOUR_TURN:
            return NetworkController::tr("It is not your turn.");
        case Err::COLUMN_FULL:
            return NetworkController::tr("That column is full.");
        case Err::INVALID_MOVE:
            return NetworkController::tr("That move is not valid.");
        case Err::GAME_ALREADY_OVER:
            return NetworkController::tr("The game is already over.");
        case Err::SESSION_EXPIRED:
        case Err::INVALID_SESSION_TOKEN:
            return NetworkController::tr("The session is no longer valid. Please reconnect.");
        case Err::PROTOCOL_VERSION_MISMATCH:
            return NetworkController::tr("The server speaks an incompatible protocol version.");
        case Err::INVALID_MESSAGE:
            if (serverMessage == QLatin1String("Game name already exists"))
            {
                return NetworkController::tr("A game with this name already exists.");
            }
            if (serverMessage == QLatin1String("Invalid game configuration"))
            {
                return NetworkController::tr("The server rejected the game settings.");
            }
            break;
        default:
            break;
        }

        if (serverMessage.isEmpty())
        {
            return NetworkController::tr("The server reported error %1.").arg(code);
        }
        return serverMessage;
    }
}

// ============================================================================
// Session: one connection = one Client + worker thread + event thread
// ============================================================================

/**
 * Threads of a session:
 *  - worker thread: connects, then executes the queued jobs one at a time (the synchronous requests
 *    of the Client block until the answer, and the Client allows one at a time anyway); it also owns
 *    the event thread, which it starts after connecting and joins before exiting, i.e. before the
 *    Client can be destroyed;
 *  - event thread: runs Client::run(); all the Client callbacks are invoked there.
 * Both only ever talk to the GUI through post(), which delivers a queued call to the
 * controller (and does nothing once the controller has detached from the session).
 *
 * How the Client behaves, and what the jobs rely on: a request returns std::nullopt when the answer does
 * not come in time or when the connection is closed meanwhile (disconnect() releases it at once); when the
 * server refuses it with an ERROR message it returns at once, with success == false, AFTER the error
 * callback has run (so the code of the error, which the response does not carry, is already known).
 */
class NetworkController::Session : public std::enable_shared_from_this<NetworkController::Session>
{
public:
    Session(quint64 id, NetworkController *owner, ConnectionInfo info)
        : id_(id), owner_(owner), info_(std::move(info))
    {
    }

    ~Session()
    {
        // The worker has been joined (or detached after finishing) by now.
        if (client_->isConnected())
        {
            client_->disconnect();
        }
    }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // ---- GUI thread API -------------------------------------------------

    void start()
    {
        std::shared_ptr<Session> self = shared_from_this();
        worker_ = std::thread([self]()
                              { self->workerMain(); });
    }

    void enqueue(std::function<void(Session &)> job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_)
            {
                return;
            }
            queue_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    /** From now on nothing is posted to the controller any more. */
    void detachOwner()
    {
        std::lock_guard<std::mutex> lock(ownerMutex_);
        owner_ = nullptr;
    }

    /** Asks the worker to finish and wakes up the event thread; does not wait. */
    void requestStop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();

        // Client::disconnect() is thread safe: it makes the event thread leave its loop, and it releases
        // a request that is waiting for its answer, so the worker does not have to wait for a timeout.
        if (client_->isConnected())
        {
            client_->disconnect();
        }
    }

    /** Waits for the worker to finish and joins it. Returns false on timeout. */
    bool joinFor(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this]()
                          { return finished_; }))
        {
            return false;
        }
        lock.unlock();

        if (worker_.joinable())
        {
            worker_.join();
        }
        return true;
    }

    /**
     * Gives up waiting for a worker that is stuck in the one call that cannot be interrupted, the TCP
     * connect to a host that does not answer: the thread is detached and keeps the session alive until
     * the call returns (at the latest after kConnectTimeoutMs), then everything is released.
     */
    void abandon()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!finished_)
        {
            abandoned_ = true;
            ++g_stuckThreads;
        }
        if (worker_.joinable())
        {
            worker_.detach();
        }
    }

    // ---- jobs (run on the worker thread) --------------------------------

    void listGames()
    {
        if (!client_->isConnected())
        {
            post([](NetworkController &c)
                 { c.handleRefreshFailed(NetworkController::tr("Not connected to the server.")); });
            return;
        }

        const quint64 errorMark = errorCounter();

        std::optional<ListGamesResponse> response = client_->requestGamesList(kRequestTimeoutMs);

        if (stopRequested())
        {
            return;
        }

        if (!response)
        {
            const QString text = failureText(errorMark);
            post([text](NetworkController &c)
                 { c.handleRefreshFailed(text); });
            return;
        }

        QList<GameInfo> games;
        games.reserve(static_cast<qsizetype>(response->games.size()));
        for (const GameInfo &game : response->games)
        {
            games.push_back(game);
        }
        post([games](NetworkController &c)
             { c.handleGamesListed(games); });
    }

    void createAndJoin(GameConfig config, std::string name)
    {
        if (!client_->isConnected())
        {
            post([](NetworkController &c)
                 { c.handleCreateFailed(NetworkController::tr("Not connected to the server.")); });
            return;
        }

        const quint64 errorMark = errorCounter();
        std::optional<CreateGameResponse> created = client_->requestCreateGame(config, name, kRequestTimeoutMs);

        if (stopRequested())
        {
            return;
        }

        if (!created || !created->success)
        {
            const QString text = failureText(errorMark, created ? created->message : std::string());
            post([text](NetworkController &c)
                 { c.handleCreateFailed(text); });
            return;
        }

        // Creating a game does not make the creator join it.
        joinGame(created->game_info.game_id);
    }

    void joinGame(quint32 gameId)
    {
        if (!client_->isConnected())
        {
            post([](NetworkController &c)
                 { c.handleJoinFailed(NetworkController::tr("Not connected to the server.")); });
            return;
        }

        const quint64 errorMark = errorCounter();
        std::optional<JoinGameResponse> response = client_->requestJoinGame(gameId, kRequestTimeoutMs);

        if (stopRequested())
        {
            return;
        }

        if (!response || !response->success)
        {
            const QString text = failureText(errorMark, response ? response->message : std::string());
            post([text](NetworkController &c)
                 { c.handleJoinFailed(text); });
            return;
        }

        const GameInfo info = response->game_info;
        const int playerId = response->assigned_player_id;
        post([info, playerId](NetworkController &c)
             { c.handleJoined(info, playerId); });
    }

    void sendMove(int column)
    {
        if (!client_->isConnected() || !client_->sendMove(static_cast<uint8_t>(column)))
        {
            post([](NetworkController &c)
                 { c.handleMoveRejected(NetworkController::tr("Could not send the move: the connection is down.")); });
        }
    }

private:
    using Handler = std::function<void(NetworkController &)>;

    /** Delivers `handler` to the controller, in the GUI thread, unless the session is detached. */
    void post(Handler handler)
    {
        std::lock_guard<std::mutex> lock(ownerMutex_);
        if (owner_ == nullptr)
        {
            return;
        }

        NetworkController *owner = owner_;
        const quint64 id = id_;
        QMetaObject::invokeMethod(
            owner,
            [owner, id, handler = std::move(handler)]()
            {
                // Events of a session that has been ended in the meantime are stale.
                if (owner->sessionId_ == id)
                {
                    handler(*owner);
                }
            },
            Qt::QueuedConnection);
    }

    bool stopRequested()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }

    /** Makes the worker leave its loop (used when the session fails by itself). */
    void markStop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
    }

    /** Number of ERROR messages received so far: a request compares it before and after it. */
    quint64 errorCounter()
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return errorCount_;
    }

    /**
     * Text for a request that did not succeed. `errorMark` is errorCounter() from before the request:
     * if an ERROR message arrived meanwhile the server refused the request, and its (translated) text is
     * used, because it knows the error code, which the response of the Client does not carry. Otherwise
     * the message of the response, or the reason why there is no response at all.
     */
    QString failureText(quint64 errorMark, const std::string &responseMessage = std::string())
    {
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            if (errorCount_ != errorMark && !lastError_.isEmpty())
            {
                return lastError_;
            }
        }
        if (!responseMessage.empty())
        {
            return QString::fromStdString(responseMessage);
        }
        if (!client_->isConnected())
        {
            return NetworkController::tr("The server closed the connection.");
        }
        return NetworkController::tr("The server did not answer in time.");
    }

    // ---- Client callbacks (event thread) --------------------------------

    void installCallbacks()
    {
        client_->onGameStateUpdate([this](const GameStateUpdate &state)
                                   {
            GameStateUpdate copy = state;
            post([copy](NetworkController &c)
                 { c.handleState(copy); }); });

        client_->onMoveResult([this](bool success, const std::string &message)
                              {
            if (!success)
            {
                const QString text = QString::fromStdString(message);
                post([text](NetworkController &c)
                     { c.handleMoveRejected(text); });
            } });

        client_->onError([this](uint16_t code, const std::string &message)
                         { handleError(code, QString::fromStdString(message)); });

        client_->onDisconnected([this]()
                                { post([](NetworkController &c)
                                       { c.handleDisconnected(NetworkController::tr("The connection to the server was lost.")); }); });
    }

    void handleError(uint16_t code, const QString &serverMessage)
    {
        const QString text = friendlyError(code, serverMessage);
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            ++errorCount_;
            lastError_ = text;
        }

        post([code, text](NetworkController &c)
             { c.handleServerError(code, text); });

        // If a request is waiting it returns right after this callback and reports its own failure
        // (see failureText()): nothing else to do here.
    }

    // ---- worker thread ---------------------------------------------------

    void workerMain()
    {
        connectAndHandshake();

        for (;;)
        {
            std::function<void(Session &)> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]()
                         { return stop_ || !queue_.empty(); });
                if (stop_)
                {
                    break;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job(*this);
        }

        // Shut down: closing the connection wakes up the event thread, which can then be joined.
        if (client_->isConnected())
        {
            client_->disconnect();
        }
        if (events_.joinable())
        {
            events_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        if (abandoned_)
        {
            --g_stuckThreads;
        }
        cv_.notify_all();
    }

    void connectAndHandshake()
    {
        // 1. TCP connection: blocking (it cannot be interrupted), but bounded by the timeout.
        bool connected = false;
        QString failure;
        try
        {
            connected = client_->connect(info_.host.toStdString(), info_.port, kConnectTimeoutMs);
            if (!connected)
            {
                failure = NetworkController::tr("Could not connect to %1:%2. Is the server running, and are the address and the port correct?")
                              .arg(info_.host)
                              .arg(info_.port);
            }
        }
        catch (const sockpp::getaddrinfo_error &error)
        {
            failure = NetworkController::tr("Could not resolve the host name \"%1\": %2")
                          .arg(info_.host, QString::fromLocal8Bit(error.what()));
        }
        catch (const std::exception &error)
        {
            failure = NetworkController::tr("Could not connect to %1:%2: %3")
                          .arg(info_.host)
                          .arg(info_.port)
                          .arg(QString::fromLocal8Bit(error.what()));
        }

        if (stopRequested())
        {
            // The attempt was abandoned while it was blocked: undo it quietly.
            if (connected && client_->isConnected())
            {
                client_->disconnect();
            }
            return;
        }

        if (!connected)
        {
            post([failure](NetworkController &c)
                 { c.handleConnectionFailed(failure); });
            markStop();
            return;
        }

        // 2. Event thread (callbacks must be installed before it starts).
        installCallbacks();
        events_ = std::thread([client = client_.get()]()
                              { client->run(); });

        // 3. Handshake.
        const quint64 errorMark = errorCounter();
        std::optional<ConnectResponse> response = client_->requestConnect(info_.playerName.toStdString(), kRequestTimeoutMs);

        if (stopRequested())
        {
            return;
        }

        if (!response || !response->success)
        {
            const QString text = failureText(errorMark, response ? response->message : std::string());
            post([text](NetworkController &c)
                 { c.handleConnectionFailed(text); });
            markStop();
            return;
        }

        const ConnectionInfo info = info_;
        post([info](NetworkController &c)
             { c.handleConnected(info); });
    }

    // ---- data ----------------------------------------------------------------

    const quint64 id_;
    std::mutex ownerMutex_;
    NetworkController *owner_; // guarded by ownerMutex_
    const ConnectionInfo info_;

    std::mutex mutex_; // guards queue_, stop_, finished_, abandoned_
    std::condition_variable cv_;
    std::deque<std::function<void(Session &)>> queue_;
    bool stop_ = false;
    bool finished_ = false;
    bool abandoned_ = false;

    std::thread worker_; // touched by the GUI thread only (after start())
    std::thread events_; // touched by the worker thread only

    std::mutex errorMutex_;
    quint64 errorCount_ = 0; // ERROR messages received so far
    QString lastError_;      // text of the last one, already translated

    // Declared last: destroyed first, while the members used by its callbacks are still alive.
    std::unique_ptr<Client> client_{std::make_unique<Client>()};
};

// ============================================================================
// NetworkController
// ============================================================================

NetworkController::NetworkController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ConnectionInfo>("ConnectionInfo");
    qRegisterMetaType<GameInfo>("GameInfo");
    qRegisterMetaType<GameConfig>("GameConfig");
    qRegisterMetaType<GameStateUpdate>("GameStateUpdate");
    qRegisterMetaType<QList<GameInfo>>("QList<GameInfo>");
}

NetworkController::~NetworkController()
{
    state_ = State::Idle;
    endSession();
}

int NetworkController::stuckThreadCount()
{
    return g_stuckThreads.load();
}

void NetworkController::connectToServer(const QString &host, quint16 port, const QString &playerName)
{
    endSession();

    info_.host = host.trimmed();
    info_.port = port;
    info_.playerName = playerName.trimmed();

    if (info_.host.isEmpty() || info_.port == 0 || info_.playerName.isEmpty())
    {
        state_ = State::Idle;
        emit connectionFailed(tr("Please enter a server address, a port and a player name."));
        return;
    }

    state_ = State::Connecting;
    session_ = std::make_shared<Session>(++sessionCounter_, this, info_);
    sessionId_ = sessionCounter_;
    session_->start();
}

void NetworkController::reconnect()
{
    const ConnectionInfo info = info_;
    connectToServer(info.host, info.port, info.playerName);
}

void NetworkController::disconnectFromServer()
{
    state_ = State::Idle;
    endSession();
}

void NetworkController::endSession()
{
    sessionId_ = 0;
    refreshInFlight_ = false;
    joined_ = false;
    earlyStates_.clear();

    if (!session_)
    {
        return;
    }

    std::shared_ptr<Session> session = std::move(session_);
    session->detachOwner();
    session->requestStop();
    if (!session->joinFor(kJoinGrace))
    {
        // Stuck in a blocking call (e.g. connecting to an unreachable host): it cleans up by itself later.
        session->abandon();
    }
}

void NetworkController::refreshGames()
{
    if (!session_ || state_ != State::Connected || refreshInFlight_)
    {
        return;
    }

    refreshInFlight_ = true;
    session_->enqueue([](Session &session)
                      { session.listGames(); });
}

void NetworkController::createGame(const GameConfig &config, const QString &gameName)
{
    if (!session_ || state_ != State::Connected)
    {
        emit createFailed(tr("Not connected to the server."));
        return;
    }

    const std::string name = gameName.trimmed().toStdString();
    session_->enqueue([config, name](Session &session)
                      { session.createAndJoin(config, name); });
}

void NetworkController::joinGame(quint32 gameId)
{
    if (!session_ || state_ != State::Connected)
    {
        emit joinFailed(tr("Not connected to the server."));
        return;
    }

    session_->enqueue([gameId](Session &session)
                      { session.joinGame(gameId); });
}

void NetworkController::sendMove(int column)
{
    if (!session_ || state_ != State::Connected)
    {
        emit moveRejected(tr("Not connected to the server."));
        return;
    }

    session_->enqueue([column](Session &session)
                      { session.sendMove(column); });
}

// ---- handlers of the events posted by the session (GUI thread) -------------

void NetworkController::handleConnected(const ConnectionInfo &info)
{
    if (state_ != State::Connecting)
    {
        return;
    }
    state_ = State::Connected;
    emit connected(info);
}

void NetworkController::handleConnectionFailed(const QString &message)
{
    if (state_ != State::Connecting)
    {
        return; // late report of an attempt that is already over
    }
    state_ = State::Idle;
    endSession();
    emit connectionFailed(message);
}

void NetworkController::handleDisconnected(const QString &reason)
{
    if (state_ == State::Idle)
    {
        return;
    }

    const bool wasConnecting = (state_ == State::Connecting);
    state_ = State::Idle;
    endSession();

    if (wasConnecting)
    {
        emit connectionFailed(reason);
    }
    else
    {
        emit disconnected(reason);
    }
}

void NetworkController::handleGamesListed(const QList<GameInfo> &games)
{
    refreshInFlight_ = false;
    emit gamesListed(games);
}

void NetworkController::handleRefreshFailed(const QString &message)
{
    refreshInFlight_ = false;
    emit refreshFailed(message);
}

void NetworkController::handleJoined(const GameInfo &info, int playerId)
{
    if (state_ != State::Connected || joined_)
    {
        return;
    }

    joined_ = true;
    emit joined(info, playerId);

    // The state broadcast may have overtaken the join response: deliver it now, in order.
    const QList<GameStateUpdate> early = earlyStates_;
    earlyStates_.clear();
    for (const GameStateUpdate &state : early)
    {
        if (state.game_id == info.game_id)
        {
            emit stateUpdated(state);
        }
    }
}

void NetworkController::handleJoinFailed(const QString &message)
{
    emit joinFailed(message);
}

void NetworkController::handleCreateFailed(const QString &message)
{
    emit createFailed(message);
}

void NetworkController::handleState(const GameStateUpdate &state)
{
    if (!joined_)
    {
        if (earlyStates_.size() >= kMaxEarlyStates)
        {
            earlyStates_.removeFirst();
        }
        earlyStates_.push_back(state);
        return;
    }
    emit stateUpdated(state);
}

void NetworkController::handleMoveRejected(const QString &message)
{
    emit moveRejected(message);
}

void NetworkController::handleServerError(quint16 code, const QString &message)
{
    emit serverError(code, message);
}
