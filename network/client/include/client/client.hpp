#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <optional>
#include <future>
#include <sockpp/tcp_connector.h>

#include "protocol/protocol.hpp"

/**
 * High-level client for Connect X game
 * Handles connection, message serialization, and state management
 */
class Client
{
public:
    // Callback types
    using DisconnectedCallback = std::function<void()>;
    using GameStateUpdateCallback = std::function<void(const GameStateUpdate &)>;
    using MoveResultCallback = std::function<void(bool success, const std::string &message)>;
    using ErrorCallback = std::function<void(uint16_t error_code, const std::string &message)>;

    Client();
    ~Client();

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * Connect to server
     * @param host Server hostname or IP
     * @param port Server port
     * @param timeout_ms How long to wait for the server to accept the connection; 0 = the operating system's
     *        own limit (about 20 s on Windows, 2 minutes on Linux, when the address does not answer at all)
     * @return true if connection successful
     * @throws sockpp::getaddrinfo_error (a std::runtime_error) if the host name cannot be resolved
     */
    bool connect(const std::string &host, uint16_t port, int timeout_ms = 10000);

    /**
     * Disconnect from server
     */
    void disconnect();

    /**
     * Check if connected to server
     */
    bool isConnected() const { return connected_; }

    /**
     * Check if event loop is running
     */
    bool isRunning() const { return running_; }

    /**
     * Run the client event loop (blocking)
     * Processes incoming messages until disconnected
     */
    void run();

    /**
     * Stop the event loop
     */
    void stop();

    // ========================================================================
    // Game Actions
    // ========================================================================

    /**
     * Send connection request with player name
     * @param player_name Your display name
     */
    bool sendConnectRequest(const std::string &player_name);

    /**
     * Create a new game with custom configuration
     */
    bool sendCreateGame(const GameConfig &config, const std::string &name = "");

    /**
     * Request list of available games
     */
    bool sendListGames();

    /**
     * Join an existing game by ID
     */
    bool sendJoinGame(uint32_t game_id);

    /**
     * Make a move (place piece in column)
     * @param column Column index (0-based)
     */
    bool sendMove(uint8_t column);

    /**
     * Send disconnect message
     */
    bool sendDisconnect(const std::string &reason = "Client disconnecting");

    // ===========================================================================
    // Synchronous Requests
    // ===========================================================================
    //
    // Each one sends the request and blocks until the server has answered. They need the event loop
    // (run()) to be running in another thread, which is the one that receives the answer: calling them
    // from a callback would block that very thread, so it is refused. Only one request is in flight at a
    // time (the answers do not say which request they belong to); the others wait their turn.
    //
    // The result is std::nullopt if the answer does not arrive in `timeout_ms` or the connection is lost.
    // If the server refuses the request with an ERROR message the call returns at once with
    // success == false and the server's explanation in `message` (for requestGamesList(), which has
    // neither, with std::nullopt); the error is also reported to the onError() callback, first.
    std::optional<ConnectResponse> requestConnect(const std::string &player_name, int timeout_ms = 3000);
    std::optional<CreateGameResponse> requestCreateGame(const GameConfig &config, const std::string &name = "", int timeout_ms = 3000);
    std::optional<ListGamesResponse> requestGamesList(int timeout_ms = 3000);
    std::optional<JoinGameResponse> requestJoinGame(uint32_t game_id, int timeout_ms = 3000);

    // ========================================================================
    // Callbacks (set these to handle events)
    // ========================================================================

    void onDisconnected(DisconnectedCallback callback)
    {
        disconnected_callback_ = callback;
    }

    void onGameStateUpdate(GameStateUpdateCallback callback)
    {
        game_state_callback_ = callback;
    }

    void onMoveResult(MoveResultCallback callback)
    {
        move_result_callback_ = callback;
    }

    void onError(ErrorCallback callback)
    {
        error_callback_ = callback;
    }

    // ========================================================================
    // State Accessors
    // ========================================================================

    /**
     * Get current session token
     */
    const std::string &getSessionToken() const { return session_token_; }

    /**
     * Get assigned player ID
     */
    uint8_t getPlayerId() const { return player_id_; }

    std::optional<uint8_t> getPlayerIndex(const GameStateUpdate &state, uint8_t player_id)
    {
        auto it = std::find(state.players.begin(), state.players.end(), player_id);
        if (it != state.players.end())
        {
            return static_cast<uint8_t>(std::distance(state.players.begin(), it));
        }
        return std::nullopt;
    }

    /**
     * Get current game info
     */
    const std::optional<GameInfo> &getGameInfo() const { return game_info_; }

    /**
     * Get current game ID
     */
    uint32_t getGameId() const { return game_info_.has_value() ? game_info_->game_id : 0; }

    /**
     * Get current game name
     */
    std::string getGameName() const { return game_info_.has_value() ? game_info_->game_name : std::string(); }

    /**
     * Get current game state (updated by server)
     */
    const std::optional<GameStateUpdate> &getCurrentState() const
    {
        return current_state_;
    }

    /**
     * Check if it's my turn
     */
    bool isMyTurn() const
    {
        if (!current_state_.has_value() || current_state_->status != ProtocolGameStatus::IN_PROGRESS)
            return false;
        return current_state_->current_player == player_id_;
    }

private:
    // Network
    sockpp::tcp_connector socket_;
    std::mutex socket_mutex_; // serialises the closing of the socket
    std::mutex write_mutex_;  // keeps the messages written by different threads apart
    std::atomic<bool> connected_;
    std::atomic<bool> running_;
    std::atomic<bool> loop_active_;                // run() is executing
    std::atomic<std::thread::id> event_thread_id_; // the thread executing run()

    // Session state
    std::string session_token_;
    uint8_t player_id_;
    std::optional<GameInfo> game_info_;
    std::optional<GameStateUpdate> current_state_;
    std::mutex state_mutex_;

    // What the event thread tells the thread blocked in a synchronous request
    struct Reply
    {
        enum class Kind
        {
            RESPONSE,     // the response that was awaited: `payload` is its serialized form
            SERVER_ERROR, // the server sent an ERROR message instead: `payload` is its text
            CLOSED        // the connection was lost
        };

        Kind kind = Kind::CLOSED;
        std::string payload;
    };

    struct PendingRequest
    {
        MessageType expected{};
        std::promise<Reply> promise;
    };

    std::mutex request_mutex_;              // held by the thread making a request, from sending it to the answer
    std::mutex pending_mutex_;              // protects pending_
    std::optional<PendingRequest> pending_; // the request waiting for its answer, if any

    // Callbacks
    DisconnectedCallback disconnected_callback_;
    GameStateUpdateCallback game_state_callback_;
    MoveResultCallback move_result_callback_;
    ErrorCallback error_callback_;

    // Internal methods
    bool sendMessage(MessageType type, const std::string &payload);
    std::optional<Reply> request(MessageType expected, int timeout_ms, const std::function<bool()> &send);
    bool answerPending(Reply::Kind kind, MessageType type, std::string payload);
    std::string readMessage();
    bool readExact(void *buffer, size_t size);
    int waitForData(int timeout_ms);
    void closeSocket();
    bool writeMessage(const std::string &message);
    void processMessage(const std::string &data);
    void storeResponseState(MessageType type, const std::string &payload);

    // Message handlers
    void handleGameStateUpdate(const std::string &payload);
    void handleMoveResult(const std::string &payload);
    void handleError(const std::string &payload);
};