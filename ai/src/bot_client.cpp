#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include "ai/bot_client.hpp"

namespace ai
{
    namespace
    {
        /** Runs a function when it goes out of scope, whatever the way out (return or exception). */
        struct ScopeGuard
        {
            std::function<void()> function;
            ~ScopeGuard() { function(); }
        };
    }

    BotClient::BotClient(const BotOptions &options, AgentPtr agent)
        : options_(options),
          agent_(std::move(agent)),
          stop_(false),
          state_sequence_(0),
          disconnected_(false),
          move_rejected_(false)
    {
        if (options_.player_name.empty())
            options_.player_name = agent_ ? agent_->name() : "Bot";
        if (options_.player_name.size() > Protocol::MAX_PLAYER_NAME_LENGTH)
            options_.player_name.resize(Protocol::MAX_PLAYER_NAME_LENGTH);
    }

    void BotClient::say(const std::string &text) const
    {
        if (options_.log)
            options_.log(text);
    }

    void BotClient::requestStop()
    {
        // Only an atomic store: safe to call from a signal handler. The main loop polls the flag
        // several times per second.
        stop_ = true;
    }

    int BotClient::run()
    {
        int played = 0;
        for (int game = 0; options_.games <= 0 || game < options_.games; ++game)
        {
            if (stop_)
                break;

            switch (playOneGame(game))
            {
            case Outcome::FINISHED:
                ++played;
                break;
            case Outcome::ABORTED:
                break;
            case Outcome::FAILED:
                return EXIT_FAILURE;
            }
        }

        return (played > 0 || stop_) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    bool BotClient::joinGame(Client &client, JoinGameResponse &response, std::string &error)
    {
        auto failure = [&](const std::string &what)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error = what + (last_error_.empty() ? " (no response from the server)" : ": " + last_error_);
            return false;
        };

        std::optional<JoinGameResponse> join;

        switch (options_.join_mode)
        {
        case BotJoinMode::AUTO:
            say("Joining the first available game...");
            join = client.requestJoinGame(0);
            break;

        case BotJoinMode::BY_ID:
            say("Joining game " + std::to_string(options_.game_id) + "...");
            join = client.requestJoinGame(options_.game_id);
            break;

        case BotJoinMode::BY_NAME:
        {
            say("Looking for the game '" + options_.game_name + "'...");
            const std::optional<ListGamesResponse> list = client.requestGamesList();
            if (!list)
                return failure("Could not get the list of games");

            std::optional<uint32_t> target;
            for (const GameInfo &info : list->games)
            {
                if (info.game_name == options_.game_name && info.current_players < info.config.num_players &&
                    info.status == ProtocolGameStatus::NOT_STARTED)
                {
                    target = info.game_id;
                    break;
                }
            }
            if (!target)
            {
                error = "No open game named '" + options_.game_name + "'";
                return false;
            }
            join = client.requestJoinGame(*target);
            break;
        }

        case BotJoinMode::CREATE:
        {
            say("Creating the game '" + options_.game_name + "'...");
            const std::optional<CreateGameResponse> created = client.requestCreateGame(options_.game_config, options_.game_name);
            if (!created)
                return failure("Could not create the game");
            if (!created->success)
            {
                error = "Could not create the game: " + created->message;
                return false;
            }
            join = client.requestJoinGame(created->game_info.game_id);
            break;
        }
        }

        if (!join)
            return failure("Could not join the game");
        if (!join->success)
        {
            error = "Could not join the game: " + join->message;
            return false;
        }

        response = *join;
        return true;
    }

    BotClient::Outcome BotClient::playOneGame(int game_index)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_state_.reset();
            state_sequence_ = 0;
            disconnected_ = false;
            move_rejected_ = false;
            last_error_.clear();
        }

        Client client;

        // Everything the Client tells us arrives on its event thread: just remember it and wake up the player
        client.onGameStateUpdate([this](const GameStateUpdate &state)
                                 {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_state_ = state;
                ++state_sequence_;
            }
            condition_.notify_all(); });

        client.onMoveResult([this](bool success, const std::string &message)
                            {
            if (!success)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    move_rejected_ = true;
                    last_error_ = message;
                }
                condition_.notify_all();
            } });

        client.onError([this](uint16_t, const std::string &message)
                       {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = message; });

        client.onDisconnected([this]()
                              {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                disconnected_ = true;
            }
            condition_.notify_all(); });

        if (!client.connect(options_.host, options_.port))
        {
            say("Cannot connect to " + options_.host + ":" + std::to_string(options_.port));
            return Outcome::FAILED;
        }

        std::thread event_thread([&client]()
                                 { client.run(); });

        // Whatever happens below (return, exception), stop the client and wait for its thread
        ScopeGuard cleanup{[&]()
                           {
                               client.stop();
                               if (event_thread.joinable())
                                   event_thread.join();
                           }};

        const std::optional<ConnectResponse> connected = client.requestConnect(options_.player_name);
        if (!connected || !connected->success)
        {
            say("The server refused the connection" + (connected ? ": " + connected->message : std::string(" (no response)")));
            return Outcome::FAILED;
        }

        JoinGameResponse joined;
        std::string error;
        if (!joinGame(client, joined, error))
        {
            say(error);
            return Outcome::FAILED;
        }

        const GameConfig &config = joined.game_info.config;
        const uint8_t my_id = joined.assigned_player_id;

        std::string reason;
        if (config.num_players != 2)
        {
            say("This game has " + std::to_string(config.num_players) + " players: the AI plays two-player games only");
            return Outcome::FAILED;
        }
        if (!Geometry::isSupported(config.rows, config.cols, config.connect_length, &reason))
        {
            say("This board is not supported by the AI: " + reason);
            return Outcome::FAILED;
        }

        const uint8_t opponent_id = my_id == 1 ? 2 : 1;
        say((options_.games == 1 ? std::string() : "[game " + std::to_string(game_index + 1) + "] ") +
            "Joined game " + std::to_string(joined.game_info.game_id) +
            (joined.game_info.game_name.empty() ? "" : " '" + joined.game_info.game_name + "'") + " as player " +
            std::to_string(static_cast<int>(my_id)) + " (" + std::to_string(config.rows) + "x" + std::to_string(config.cols) +
            ", connect " + std::to_string(config.connect_length) + ") using " + agent_->name());

        agent_->newGame();

        Outcome outcome = Outcome::ABORTED;
        uint64_t handled = 0;
        bool game_started = false;
        const auto joined_at = std::chrono::steady_clock::now();
        bool announced_waiting = false;
        int move_number = 0;

        for (;;)
        {
            GameStateUpdate state;
            bool retry_after_rejection = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(250), [&]
                                    { return stop_ || disconnected_ || move_rejected_ || state_sequence_ != handled; });

                if (stop_)
                    break;
                if (state_sequence_ == handled && !move_rejected_)
                {
                    if (disconnected_)
                    {
                        say("The connection to the server was lost");
                        break;
                    }

                    // Nothing new: still waiting for an opponent / for the opponent's move
                    if (!game_started && options_.wait_for_opponent_ms > 0 &&
                        std::chrono::steady_clock::now() - joined_at > std::chrono::milliseconds(options_.wait_for_opponent_ms))
                    {
                        say("Nobody joined the game: giving up");
                        break;
                    }
                    continue;
                }

                if (move_rejected_)
                {
                    move_rejected_ = false;
                    retry_after_rejection = true;
                    say("The server rejected the move: " + last_error_);
                }
                if (!latest_state_)
                    continue;
                state = *latest_state_;
                handled = state_sequence_;
            }

            switch (state.status)
            {
            case ProtocolGameStatus::NOT_STARTED:
                if (!announced_waiting)
                {
                    announced_waiting = true;
                    say("Waiting for an opponent (" + std::to_string(state.players.size()) + "/" + std::to_string(config.num_players) + ")...");
                }
                continue;

            case ProtocolGameStatus::IN_PROGRESS:
            {
                game_started = true;

                if (state.players.size() < static_cast<size_t>(config.num_players))
                {
                    say("The opponent left the game");
                    return Outcome::ABORTED;
                }

                if (state.current_player != my_id)
                    continue; // the opponent is thinking

                Position position = Position::fromGrid(state.board, my_id, opponent_id, config.connect_length);

                const auto start = std::chrono::steady_clock::now();
                MoveChoice choice;
                if (retry_after_rejection)
                {
                    // Our agent produced something illegal: fall back on any legal move rather than hanging
                    choice.column = position.legalMoves().front();
                    choice.info = "fallback";
                }
                else
                {
                    choice = agent_->chooseMove(position);
                }

                // Keep the pace human: never faster than min_move_ms
                const auto elapsed = std::chrono::steady_clock::now() - start;
                const auto minimum = std::chrono::milliseconds(options_.min_move_ms);
                if (elapsed < minimum)
                    std::this_thread::sleep_for(minimum - elapsed);

                ++move_number;
                if (options_.verbose)
                {
                    std::ostringstream description;
                    description << "move " << move_number << ": column " << (choice.column + 1) << "  [" << choice.info << "]";
                    if (choice.value == choice.value) // not NaN
                        description << " value " << choice.value;
                    description << " nodes " << choice.nodes << " time " << static_cast<int>(choice.seconds * 1000) << " ms\n"
                                << position.toString();
                    say(description.str());
                }
                else
                {
                    say("Playing column " + std::to_string(choice.column + 1));
                }

                if (!client.sendMove(static_cast<uint8_t>(choice.column)))
                {
                    say("Could not send the move: the connection is gone");
                    return Outcome::ABORTED;
                }
                continue;
            }

            case ProtocolGameStatus::FINISHED_WIN:
                say(state.winner && *state.winner == my_id ? "I won!" : "I lost.");
                outcome = Outcome::FINISHED;
                break;

            case ProtocolGameStatus::FINISHED_DRAW:
                say("It's a draw.");
                outcome = Outcome::FINISHED;
                break;
            }

            if (outcome == Outcome::FINISHED)
                break;
        }

        return outcome;
    }
}
