#include <algorithm>
#include <cmath>

#include "gateway/gatekeeper.hpp"
#include "gateway/websocket.hpp"

using json = nlohmann::json;

namespace gateway
{
    namespace
    {
        /** The value of a JSON number that is a whole number within [min, max]. */
        std::optional<int64_t> integerIn(const json &value, int64_t min, int64_t max)
        {
            if (!value.is_number_integer())
                return std::nullopt;

            const int64_t number = value.get<int64_t>();
            if (number < min || number > max)
                return std::nullopt;

            return number;
        }

        std::string envelope(MessageType type, const std::string &payload)
        {
            return MessageSerializer::wrapMessage(type, payload);
        }

        GameInfo parseGameInfo(const json &info)
        {
            GameInfo game;
            game.game_id = info.at("game_id").get<uint32_t>();
            game.game_name = info.value("name", std::string());
            game.current_players = info.value("current_players", 0);
            game.status = static_cast<ProtocolGameStatus>(info.value("status", 0));
            game.config.rows = info.at("config").at("rows").get<uint8_t>();
            game.config.cols = info.at("config").at("cols").get<uint8_t>();
            game.config.num_players = info.at("config").at("num_players").get<uint8_t>();
            game.config.connect_length = info.at("config").at("connect_length").get<uint8_t>();
            return game;
        }
    }

    std::string sanitizeName(const std::string &name, size_t max_length)
    {
        std::string clean;

        for (unsigned char c : name)
        {
            if (c >= 0x20 && c != 0x7F)
                clean.push_back(static_cast<char>(c));
        }

        const size_t begin = clean.find_first_not_of(' ');
        if (begin == std::string::npos)
            return std::string();

        clean = clean.substr(begin, clean.find_last_not_of(' ') - begin + 1);

        // Cut it to length without leaving half of a character behind
        clean = clean.substr(0, max_length);
        while (!clean.empty() && !websocket::isValidUtf8(clean))
            clean.pop_back();

        const size_t end = clean.find_last_not_of(' ');
        return end == std::string::npos ? std::string() : clean.substr(0, end + 1);
    }

    std::string checkGameConfig(int rows, int cols, int players, int connect_length)
    {
        if (rows < Protocol::MIN_BOARD_SIZE || rows > Protocol::MAX_BOARD_SIZE ||
            cols < Protocol::MIN_BOARD_SIZE || cols > Protocol::MAX_BOARD_SIZE)
            return "The board must have between " + std::to_string(Protocol::MIN_BOARD_SIZE) + " and " +
                   std::to_string(Protocol::MAX_BOARD_SIZE) + " rows and columns";

        if (players < 2 || players > Protocol::MAX_PLAYERS)
            return "A game has between 2 and " + std::to_string(Protocol::MAX_PLAYERS) + " players";

        if (connect_length < 3 || connect_length > std::max(rows, cols))
            return "The number of discs to connect must be at least 3 and fit on the board";

        return std::string();
    }

    std::string webMessage(uint8_t type, const json &payload)
    {
        json message;
        message["type"] = type;
        message["payload"] = payload;
        return message.dump();
    }

    std::string errorMessage(uint16_t code, const std::string &text)
    {
        return envelope(MessageType::ERROR, MessageSerializer::serialize(ErrorMessage(code, text)));
    }

    // ========================================================================
    // RateLimiter
    // ========================================================================

    RateLimiter::RateLimiter(double burst, double per_second, Clock::time_point now)
        : burst_(burst), per_second_(per_second), tokens_(burst), last_(now)
    {
    }

    bool RateLimiter::allow(Clock::time_point now)
    {
        const double elapsed = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(burst_, tokens_ + elapsed * per_second_);

        if (tokens_ < 1.0)
            return false;

        tokens_ -= 1.0;
        return true;
    }

    // ========================================================================
    // Gatekeeper
    // ========================================================================

    Gatekeeper::Gatekeeper(std::function<bool()> may_create_room)
        : may_create_room_(std::move(may_create_room))
    {
    }

    Verdict Gatekeeper::refuse(uint16_t code, const std::string &text)
    {
        Verdict verdict;
        verdict.action = Verdict::Action::ANSWER;
        verdict.text = errorMessage(code, text);
        return verdict;
    }

    Verdict Gatekeeper::forward(MessageType type, const std::string &payload)
    {
        Verdict verdict;
        verdict.action = Verdict::Action::FORWARD;
        verdict.type = static_cast<uint8_t>(type);
        verdict.text = envelope(type, payload);
        return verdict;
    }

    Verdict Gatekeeper::inspect(const std::string &text)
    {
        try
        {
            return decide(text);
        }
        catch (const json::exception &)
        {
            // A field of the wrong kind where a text or a number was expected
            return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Malformed message");
        }
    }

    Verdict Gatekeeper::decide(const std::string &text)
    {
        const json message = json::parse(text, nullptr, false);

        if (message.is_discarded() || !message.is_object())
            return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Malformed message");

        const auto type = integerIn(message.value("type", json()), 0, 255);
        const json payload = message.contains("payload") ? message["payload"] : json::object();

        if (!type || !payload.is_object())
            return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Malformed message");

        // Things that need no session
        switch (*type)
        {
        case static_cast<uint8_t>(MessageType::REQ_CONNECT):
        {
            if (connect_sent_)
                return refuse(Protocol::ErrorCode::ALREADY_CONNECTED, "Already connected");

            std::string name = sanitizeName(payload.value("player_name", std::string()));
            connect_sent_ = true;

            return forward(MessageType::REQ_CONNECT, MessageSerializer::serialize(ConnectRequest(name.empty() ? "Guest" : name)));
        }

        case static_cast<uint8_t>(MessageType::HEARTBEAT):
        {
            HeartbeatMessage beat(payload.value("timestamp", uint64_t(0)));

            Verdict verdict;
            verdict.action = Verdict::Action::ANSWER;
            verdict.text = envelope(MessageType::HEARTBEAT, MessageSerializer::serialize(beat));
            return verdict;
        }

        case static_cast<uint8_t>(MessageType::DISCONNECT):
        {
            Verdict verdict;
            verdict.action = Verdict::Action::CLOSE;
            return verdict;
        }

        case WebMessage::REQ_COMPUTER_LEVELS:
        {
            Verdict verdict;
            verdict.action = Verdict::Action::LOCAL;
            verdict.type = WebMessage::REQ_COMPUTER_LEVELS;
            return verdict;
        }
        }

        // Everything else needs a session. The server answers the connection request on its own time:
        // until its token has arrived there is nothing to put in the requests.
        const bool known = *type == static_cast<uint8_t>(MessageType::REQ_LIST_GAMES) ||
                           *type == static_cast<uint8_t>(MessageType::REQ_CREATE_GAME) ||
                           *type == static_cast<uint8_t>(MessageType::REQ_JOIN_GAME) ||
                           *type == static_cast<uint8_t>(MessageType::MAKE_MOVE) ||
                           *type == WebMessage::REQ_ADD_COMPUTER;
        if (!known)
            return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Unknown message type");
        if (token_.empty())
            return refuse(Protocol::ErrorCode::INVALID_SESSION_TOKEN, "Connect first");

        switch (*type)
        {
        case static_cast<uint8_t>(MessageType::REQ_LIST_GAMES):
            return forward(MessageType::REQ_LIST_GAMES, MessageSerializer::serialize(ListGamesRequest(token_)));

        case static_cast<uint8_t>(MessageType::REQ_CREATE_GAME):
        {
            if (in_game_ || create_pending_ || join_pending_)
                return refuse(ErrorCode::ALREADY_IN_GAME, "You are already in a game");

            const auto config = payload.find("config");
            if (config == payload.end() || !config->is_object())
                return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Invalid game configuration");

            const auto rows = integerIn(config->value("rows", json()), 0, 255);
            const auto cols = integerIn(config->value("cols", json()), 0, 255);
            const auto players = integerIn(config->value("num_players", json()), 0, 255);
            const auto connect = integerIn(config->value("connect_length", json()), 0, 255);
            if (!rows || !cols || !players || !connect)
                return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Invalid game configuration");

            const std::string problem = checkGameConfig(static_cast<int>(*rows), static_cast<int>(*cols),
                                                        static_cast<int>(*players), static_cast<int>(*connect));
            if (!problem.empty())
                return refuse(Protocol::ErrorCode::INVALID_MESSAGE, problem);

            if (may_create_room_ && !may_create_room_())
                return refuse(ErrorCode::TOO_MANY_ROOMS, "There are too many open rooms at the moment: try again in a while");

            CreateGameRequest request;
            request.session_token = token_;
            request.game_name = sanitizeName(payload.value("name", std::string()));
            request.config = GameConfig(static_cast<uint8_t>(*rows), static_cast<uint8_t>(*cols),
                                        static_cast<uint8_t>(*players), static_cast<uint8_t>(*connect));

            create_pending_ = true;
            return forward(MessageType::REQ_CREATE_GAME, MessageSerializer::serialize(request));
        }

        case static_cast<uint8_t>(MessageType::REQ_JOIN_GAME):
        {
            if (in_game_ || create_pending_ || join_pending_)
                return refuse(ErrorCode::ALREADY_IN_GAME, "You are already in a game");

            // (0 asks the server for any game that is waiting for players, or a new standard one)
            const auto game_id = integerIn(payload.value("game_id", json()), 0, UINT32_MAX);
            if (!game_id)
                return refuse(Protocol::ErrorCode::INVALID_MESSAGE, "Invalid game");

            join_pending_ = true;
            return forward(MessageType::REQ_JOIN_GAME,
                           MessageSerializer::serialize(JoinGameRequest(token_, static_cast<uint32_t>(*game_id))));
        }

        case static_cast<uint8_t>(MessageType::MAKE_MOVE):
        {
            if (!in_game_)
                return refuse(Protocol::ErrorCode::GAME_NOT_STARTED, "You are not in a game");

            const auto column = integerIn(payload.value("column", json()), 0, 255);
            if (!column)
                return refuse(Protocol::ErrorCode::INVALID_MOVE, "Invalid column");

            return forward(MessageType::MAKE_MOVE, MessageSerializer::serialize(MakeMoveRequest(token_, static_cast<uint8_t>(*column))));
        }

        default: // REQ_ADD_COMPUTER
        {
            if (!in_game_)
                return refuse(Protocol::ErrorCode::GAME_NOT_STARTED, "Open a game first");
            if (computer_added_)
                return refuse(ErrorCode::ALREADY_IN_GAME, "A computer is already playing in this game");

            Verdict verdict;
            verdict.action = Verdict::Action::LOCAL;
            verdict.type = WebMessage::REQ_ADD_COMPUTER;
            verdict.payload = payload;
            return verdict;
        }
        }
    }

    Translation Gatekeeper::translate(const std::string &text)
    {
        Translation translation;
        translation.to_browser = text;

        try
        {
            json message = json::parse(text, nullptr, false);
            if (message.is_discarded() || !message.is_object() || !message.contains("type") || !message["type"].is_number_integer())
                return translation;

            const auto type = static_cast<MessageType>(message["type"].get<uint8_t>());
            json &payload = message["payload"];

            switch (type)
            {
            case MessageType::RES_CONNECT:
                // The token is the key to the session: it stays here
                if (payload.value("success", false))
                {
                    token_ = payload.value("session_token", std::string());
                    payload["session_token"] = "";
                    translation.to_browser = message.dump();
                }
                break;

            case MessageType::RES_CREATE_GAME:
                create_pending_ = false;
                if (payload.value("success", false))
                {
                    room_created_ = true;
                    translation.room_created = true;

                    // The creator takes his place at once
                    const auto id = payload.at("game_info").at("game_id").get<uint32_t>();
                    join_pending_ = true;
                    translation.to_server = envelope(MessageType::REQ_JOIN_GAME, MessageSerializer::serialize(JoinGameRequest(token_, id)));
                }
                break;

            case MessageType::RES_JOIN_GAME:
                join_pending_ = false;
                if (payload.value("success", false))
                {
                    in_game_ = true;
                    game_ = parseGameInfo(payload.at("game_info"));
                }
                break;

            case MessageType::ERROR:
                create_pending_ = false;
                join_pending_ = false;
                break;

            default:
                break;
            }
        }
        catch (const json::exception &)
        {
            // Not what we expected: the browser gets it as it is
        }

        return translation;
    }
}
