#include <chrono>
#include <cstring>
#include <spdlog/spdlog.h>

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#endif

#include "client/client.hpp"

namespace
{
    // How often a blocked reader checks whether it was asked to stop (see Client::readExact)
    constexpr int POLL_INTERVAL_MS = 100;
}

Client::Client()
    : connected_(false),
      running_(false),
      loop_active_(false),
      player_id_(0)
{
}

Client::~Client()
{
    disconnect();
}

// ============================================================================
// Connection Management
// ============================================================================

bool Client::connect(const std::string &host, uint16_t port, int timeout_ms)
{
    spdlog::info("Connecting to {}:{}...", host, port);

    if (connected_)
    {
        spdlog::warn("Already connected");
        return false;
    }

    // A previous event loop may still be leaving (disconnect() from another thread only asks it to):
    // wait for it before touching the socket
    for (int i = 0; i < 500 && loop_active_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // Initialize sockpp
    sockpp::socket_initializer::initialize();

    // Connect (a timeout of 0 or less means: as long as the operating system waits)
    if (!socket_.connect(sockpp::inet_address(host, port), std::chrono::milliseconds(timeout_ms)))
    {
        spdlog::error("Connection failed: {}", socket_.last_error_str());
        return false;
    }

    connected_ = true;
    spdlog::info("Connected to the server");

    return true;
}

void Client::closeSocket()
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_.is_open())
    {
        socket_.shutdown();
        socket_.close();
    }
}

void Client::disconnect()
{
    spdlog::info("Disconnecting from server...");

    // disconnect() can be reached from the event thread (read error) and from
    // a UI/worker thread at the same time: flip the flag atomically so that the
    // teardown (and the disconnected callback) runs exactly once.
    if (!connected_.exchange(false))
    {
        spdlog::warn("Not connected");
        return;
    }

    running_ = false;

    // Whoever is waiting for the answer of a request will not get it: do not make them wait for the timeout
    answerPending(Reply::Kind::CLOSED, MessageType{}, std::string());

    if (loop_active_ && event_thread_id_.load() != std::this_thread::get_id())
    {
        // Another thread is blocked reading from the socket, and closing a socket under the feet
        // of a reader is not safe (nor does it wake the reader up on every platform). Ask it to
        // leave: the event loop notices within one poll interval (immediately where shutdown()
        // is enough) and closes the socket itself when it exits.
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (socket_.is_open())
            socket_.shutdown();
    }
    else
    {
        closeSocket();
    }

    if (disconnected_callback_)
    {
        disconnected_callback_();
    }

    spdlog::info("Disconnected");
}

void Client::run()
{
    spdlog::debug("Starting event loop...");

    if (!connected_)
    {
        spdlog::error("Cannot run event loop: not connected");
        return;
    }

    event_thread_id_ = std::this_thread::get_id();
    loop_active_ = true;
    running_ = true;

    while (running_ && connected_)
    {
        try
        {
            std::string message = readMessage();

            if (message.empty())
            {
                // Connection closed, error, or we were asked to stop
                if (connected_)
                    spdlog::info("Connection closed by server");
                disconnect();
                break;
            }

            processMessage(message);
        }
        catch (const std::exception &e)
        {
            spdlog::error("Error in event loop: {}", e.what());
            disconnect();
            break;
        }
    }

    // While the loop was running it owned the socket: a disconnect() issued by another thread
    // left the closing to us
    loop_active_ = false;
    if (!connected_)
        closeSocket();

    spdlog::debug("Event loop stopped");
}

void Client::stop()
{
    running_ = false;
    if (connected_)
    {
        disconnect();
    }
}

// ============================================================================
// Game Actions
// ============================================================================

bool Client::sendConnectRequest(const std::string &player_name)
{
    spdlog::debug("Sending connect request as '{}'", player_name);

    ConnectRequest req(player_name);
    std::string payload = MessageSerializer::serialize(req);
    return sendMessage(MessageType::REQ_CONNECT, payload);
}

bool Client::sendMove(uint8_t column)
{
    spdlog::debug("Sending move request: column {}", (int)column);

    MakeMoveRequest req(session_token_, column);
    std::string payload = MessageSerializer::serialize(req);
    return sendMessage(MessageType::MAKE_MOVE, payload);
}

bool Client::sendCreateGame(const GameConfig &config, const std::string &name)
{
    spdlog::debug("Sending create game request: ({})<{}, {}, {}, {}>",
                  name.empty() ? "default" : name,
                  (int)config.rows, (int)config.cols,
                  (int)config.num_players, (int)config.connect_length);

    CreateGameRequest req;
    req.session_token = session_token_;
    req.game_name = name;
    req.config = config;

    std::string payload = MessageSerializer::serialize(req);
    return sendMessage(MessageType::REQ_CREATE_GAME, payload);
}

bool Client::sendJoinGame(uint32_t game_id)
{
    spdlog::debug("Sending join game request: game ID {}", game_id);

    JoinGameRequest req(session_token_, game_id);
    std::string payload = MessageSerializer::serialize(req);
    return sendMessage(MessageType::REQ_JOIN_GAME, payload);
}

bool Client::sendListGames()
{
    spdlog::debug("Sending list games request");

    ListGamesRequest req(session_token_);
    std::string payload = MessageSerializer::serialize(req);
    return sendMessage(MessageType::REQ_LIST_GAMES, payload);
}

bool Client::sendDisconnect(const std::string &reason)
{
    spdlog::debug("Sending disconnect message: {}", reason);

    DisconnectMessage msg(reason);
    std::string payload = MessageSerializer::serialize(msg);
    return sendMessage(MessageType::DISCONNECT, payload);
}

// ===========================================================================
// Synchronous Requests
// ===========================================================================
std::optional<ConnectResponse> Client::requestConnect(const std::string &player_name, int timeout_ms)
{
    const auto reply = request(MessageType::RES_CONNECT, timeout_ms, [&]
                               { return sendConnectRequest(player_name); });
    if (!reply)
        return std::nullopt;

    ConnectResponse resp;
    if (reply->kind == Reply::Kind::RESPONSE)
        resp = MessageSerializer::deserializeConnectResponse(reply->payload);
    else
        resp.message = reply->payload; // refused with an ERROR message

    if (resp.success)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_token_ = resp.session_token;
        }
        spdlog::debug("Connected successfully, session token: {}", session_token_);
    }
    else
        spdlog::error("Connection failed: {}", resp.message);

    return resp;
}

std::optional<CreateGameResponse> Client::requestCreateGame(const GameConfig &config, const std::string &name, int timeout_ms)
{
    const auto reply = request(MessageType::RES_CREATE_GAME, timeout_ms, [&]
                               { return sendCreateGame(config, name); });
    if (!reply)
        return std::nullopt;

    CreateGameResponse resp;
    if (reply->kind == Reply::Kind::RESPONSE)
        resp = MessageSerializer::deserializeCreateGameResponse(reply->payload);
    else
        resp.message = reply->payload; // refused with an ERROR message

    if (resp.success)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            game_info_ = resp.game_info;
        }
        spdlog::debug("Game created successfully, game ID: {}", resp.game_info.game_id);
    }
    else
        spdlog::warn("Create game failed: {}", resp.message);

    return resp;
}

std::optional<ListGamesResponse> Client::requestGamesList(int timeout_ms)
{
    const auto reply = request(MessageType::RES_LIST_GAMES, timeout_ms, [&]
                               { return sendListGames(); });
    if (!reply || reply->kind != Reply::Kind::RESPONSE)
        return std::nullopt;

    ListGamesResponse resp = MessageSerializer::deserializeGameListResponse(reply->payload);
    if (resp.games.size() > 0)
        spdlog::debug("Received game list response: {} games available", resp.games.size());
    else
        spdlog::warn("List games returned no available games");

    return resp;
}

std::optional<JoinGameResponse> Client::requestJoinGame(uint32_t game_id, int timeout_ms)
{
    const auto reply = request(MessageType::RES_JOIN_GAME, timeout_ms, [&]
                               { return sendJoinGame(game_id); });
    if (!reply)
        return std::nullopt;

    JoinGameResponse resp;
    if (reply->kind == Reply::Kind::RESPONSE)
        resp = MessageSerializer::deserializeJoinGameResponse(reply->payload);
    else
        resp.message = reply->payload; // refused with an ERROR message

    if (resp.success)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            game_info_ = resp.game_info;
            player_id_ = resp.assigned_player_id;
        }
        spdlog::debug("Joined game successfully, game ID: {}, assigned player ID: {}",
                      resp.game_info.game_id, resp.assigned_player_id);
    }
    else
        spdlog::warn("Join game failed: {}", resp.message);

    return resp;
}

// ============================================================================
// Internal Methods
// ============================================================================

bool Client::sendMessage(MessageType type, const std::string &payload)
{
    if (!connected_)
    {
        spdlog::error("Cannot send message: not connected");
        return false;
    }

    std::string message = MessageSerializer::wrapMessage(type, payload);
    return writeMessage(message);
}

std::optional<Client::Reply> Client::request(MessageType expected, int timeout_ms, const std::function<bool()> &send)
{
    // The answer is delivered by the event thread: a request made on that thread would wait for itself
    if (loop_active_ && event_thread_id_.load() == std::this_thread::get_id())
    {
        spdlog::error("Synchronous requests cannot be made from a callback: they would block the event loop that has to receive the answer");
        return std::nullopt;
    }

    // One request at a time: an answer does not say which request it belongs to
    std::lock_guard<std::mutex> request_lock(request_mutex_);

    // The answer can arrive the instant the request is out, before this thread has time to start
    // waiting: so the request is registered *first*, and only then sent
    std::future<Reply> answer;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.emplace();
        pending_->expected = expected;
        answer = pending_->promise.get_future();
    }

    if (!send())
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.reset();
        return std::nullopt;
    }

    if (answer.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_) // still waiting: give up (otherwise the answer has arrived this very moment)
        {
            pending_.reset();
            return std::nullopt;
        }
    }

    Reply reply = answer.get();
    if (reply.kind == Reply::Kind::CLOSED)
        return std::nullopt;
    return reply;
}

bool Client::answerPending(Reply::Kind kind, MessageType type, std::string payload)
{
    std::lock_guard<std::mutex> lock(pending_mutex_);

    // A response must be the one that was awaited; an error or a lost connection ends any request
    if (!pending_ || (kind == Reply::Kind::RESPONSE && pending_->expected != type))
        return false;

    Reply reply;
    reply.kind = kind;
    reply.payload = std::move(payload);
    pending_->promise.set_value(std::move(reply));
    pending_.reset();
    return true;
}

int Client::waitForData(int timeout_ms)
{
    const auto handle = socket_.handle();
    if (handle == sockpp::INVALID_SOCKET)
        return -1;

#ifdef _WIN32
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(handle, &readable);

    timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return ::select(0, &readable, nullptr, nullptr, &timeout); // (the first argument is ignored on Windows)
#else
    pollfd descriptor;
    descriptor.fd = handle;
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result < 0 && errno == EINTR)
        return 0;
    return result;
#endif
}

bool Client::readExact(void *buffer, size_t size)
{
    // A plain blocking read cannot be interrupted portably from another thread (closing the
    // socket does not wake it up on Windows, shutdown() does not everywhere). So the reader
    // sleeps in select()/poll() for short intervals and checks the connection flag in between.
    char *out = static_cast<char *>(buffer);
    size_t received = 0;

    while (received < size)
    {
        if (!connected_)
            return false;

        const int ready = waitForData(POLL_INTERVAL_MS);
        if (ready == 0)
            continue; // nothing yet: check again whether we have been asked to stop
        if (ready < 0)
            return false;

        const ssize_t n = socket_.read(out + received, size - received);
        if (n < 0)
        {
#ifndef _WIN32
            if (socket_.last_error() == EINTR)
                continue;
#endif
            return false;
        }
        if (n == 0)
            return false; // closed by the server

        received += static_cast<size_t>(n);
    }

    return true;
}

std::string Client::readMessage()
{
    // Read 4-byte length prefix
    uint32_t length = 0;
    if (!readExact(&length, sizeof(length)))
        return ""; // Connection closed or error

    // Convert from network byte order
    length = ntohl(length);

    // Validate message size
    if (length == 0 || length > Protocol::MAX_MESSAGE_SIZE)
    {
        spdlog::error("Invalid message length: {}", length);
        return "";
    }

    // Read message data
    std::vector<char> buffer(length);
    if (!readExact(buffer.data(), length))
        return ""; // Connection closed or error

    return std::string(buffer.begin(), buffer.end());
}

bool Client::writeMessage(const std::string &message)
{
    // Validate message size
    if (message.size() > Protocol::MAX_MESSAGE_SIZE)
    {
        spdlog::error("Message too large: {}", message.size());
        return false;
    }

    // The 4-byte length prefix and the message go out in a single write: it is faster (two small
    // writes in a row can stall on Nagle's algorithm) and, under the lock, messages sent from
    // different threads cannot get mixed up
    std::string frame(sizeof(uint32_t), '\0');
    const uint32_t length = htonl(static_cast<uint32_t>(message.size()));
    std::memcpy(&frame[0], &length, sizeof(length));
    frame += message;

    std::lock_guard<std::mutex> lock(write_mutex_);
    return socket_.write_n(frame.data(), frame.size()) == static_cast<ssize_t>(frame.size());
}

void Client::processMessage(const std::string &data)
{
    try
    {
        // Unwrap message
        auto [type, payload] = MessageSerializer::unwrapMessage(data);

        spdlog::debug("Received message of type {}", messageTypeToString(type));

        // Session/game info must be stored *before* the waiting thread is woken up:
        // the server sends the first GAME_STATE_UPDATE right after RES_JOIN_GAME, so
        // callbacks could otherwise run while player_id_/game_info_ are still unset.
        storeResponseState(type, payload);

        // Is it the response a thread is blocked in a synchronous request for?
        if (answerPending(Reply::Kind::RESPONSE, type, payload))
            return;

        // Route to appropriate handler
        switch (type)
        {
        case MessageType::GAME_STATE_UPDATE:
            handleGameStateUpdate(payload);
            break;

        case MessageType::MOVE_RESULT:
            handleMoveResult(payload);
            break;

        case MessageType::ERROR:
            handleError(payload);
            break;

        default:
            // (also a response that arrives after its request has given up)
            spdlog::warn("Ignoring unexpected message of type {}", messageTypeToString(type));
            break;
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("Error processing message: {}", e.what());
    }
}

void Client::storeResponseState(MessageType type, const std::string &payload)
{
    try
    {
        switch (type)
        {
        case MessageType::RES_CONNECT:
        {
            ConnectResponse resp = MessageSerializer::deserializeConnectResponse(payload);
            if (resp.success)
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                session_token_ = resp.session_token;
            }
            break;
        }

        case MessageType::RES_CREATE_GAME:
        {
            CreateGameResponse resp = MessageSerializer::deserializeCreateGameResponse(payload);
            if (resp.success)
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                game_info_ = resp.game_info;
            }
            break;
        }

        case MessageType::RES_JOIN_GAME:
        {
            JoinGameResponse resp = MessageSerializer::deserializeJoinGameResponse(payload);
            if (resp.success)
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                game_info_ = resp.game_info;
                player_id_ = resp.assigned_player_id;
            }
            break;
        }

        default:
            break;
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("Error storing response state: {}", e.what());
    }
}

// ============================================================================
// Message Handlers
// ============================================================================

void Client::handleGameStateUpdate(const std::string &payload)
{
    GameStateUpdate update = MessageSerializer::deserializeGameStateUpdate(payload);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_state_ = update;
    }

    spdlog::debug("Received game state update for game ID: {}", update.game_id);

    if (game_state_callback_)
    {
        game_state_callback_(update);
    }
}

void Client::handleMoveResult(const std::string &payload)
{
    MoveResult result = MessageSerializer::deserializeMoveResult(payload);

    spdlog::debug("Received move result: success={}, message='{}'",
                  result.success, result.message);

    if (move_result_callback_)
    {
        move_result_callback_(result.success, result.message);
    }
}

void Client::handleError(const std::string &payload)
{
    ErrorMessage err = MessageSerializer::deserializeError(payload);

    spdlog::error("Server error {}: {}", err.error_code, err.error_message);

    if (error_callback_)
    {
        error_callback_(err.error_code, err.error_message);
    }

    // A request waiting for its response would otherwise notice the refusal only when its timeout
    // expires. The callback goes first: whoever observes the error knows about it before the request returns.
    answerPending(Reply::Kind::SERVER_ERROR, MessageType::ERROR, err.error_message);
}