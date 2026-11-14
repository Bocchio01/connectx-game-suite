#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "protocol/protocol.hpp"

namespace gateway
{
    /**
     * What a browser can ask the gateway itself, as opposed to the game server. They use the same envelope as the
     * game protocol (`{"type": N, "payload": {...}}`) and the numbers continue where MessageType stops.
     */
    namespace WebMessage
    {
        constexpr uint8_t REQ_COMPUTER_LEVELS = 0x30; // payload: {}
        constexpr uint8_t RES_COMPUTER_LEVELS = 0x31; // payload: {available, levels: [{level, name, summary}]}
        constexpr uint8_t REQ_ADD_COMPUTER = 0x32;    // payload: {level}: the computer joins the game I am in
        constexpr uint8_t RES_ADD_COMPUTER = 0x33;    // payload: {success, message, name}
    }

    /** Error codes of the gateway, in the ranges of the game protocol (see Protocol::ErrorCode). */
    namespace ErrorCode
    {
        constexpr uint16_t ALREADY_IN_GAME = 204;
        constexpr uint16_t TOO_MANY_ROOMS = 205;
    }

    /** Cleans up a name typed by a visitor: no control characters, no spaces at the ends, at most `max_length` bytes. */
    std::string sanitizeName(const std::string &name, size_t max_length = Protocol::MAX_PLAYER_NAME_LENGTH);

    /** Why a game cannot be created with this shape (an empty text if it can). */
    std::string checkGameConfig(int rows, int cols, int players, int connect_length);

    /** A message of the gateway's own (see WebMessage) in the envelope of the game protocol. */
    std::string webMessage(uint8_t type, const nlohmann::json &payload);

    /** An error message for the browser, in the format of the game protocol. */
    std::string errorMessage(uint16_t code, const std::string &text);

    /** How fast a browser may talk: a burst is fine, a flood is not (a token bucket). */
    class RateLimiter
    {
    public:
        using Clock = std::chrono::steady_clock;

        /** @param burst messages that can be sent at once @param per_second messages allowed every second after that */
        RateLimiter(double burst, double per_second, Clock::time_point now = Clock::now());

        /** Whether one more message is fine at this moment (and, if so, it is counted). */
        bool allow(Clock::time_point now = Clock::now());

    private:
        double burst_;
        double per_second_;
        double tokens_;
        Clock::time_point last_;
    };

    /** What to do with a message that comes from the browser. */
    struct Verdict
    {
        enum class Action
        {
            FORWARD, // send `text` to the game server
            ANSWER,  // do not bother the server: send `text` back to the browser
            LOCAL,   // it is for the gateway: `type` and `payload`
            CLOSE    // the browser says goodbye
        };

        Action action = Action::ANSWER;
        std::string text;
        uint8_t type = 0;
        nlohmann::json payload;
    };

    /** What came out of a message from the game server. */
    struct Translation
    {
        std::string to_browser;
        std::optional<std::string> to_server; // something the gateway has to say to the server as well
        bool room_created = false;            // this very message told that the visitor's game exists
    };

    /**
     * The doorman of one browser. It stands between the browser and its connection to the game server and
     * makes sure that the visitor can only do what a visitor should be able to do:
     *
     * - the session token never reaches the browser: the gatekeeper keeps it and puts it in the requests;
     * - names and board shapes are checked before the server sees them;
     * - a connection takes part in one game only, and whoever creates a game joins it at once (the server forgets
     *   a game when its last player leaves, but a game that nobody ever joined would stay for ever);
     * - what a browser may send is a short list, nothing else gets through.
     *
     * It knows nothing about sockets: it turns strings into strings, which makes it easy to test.
     */
    class Gatekeeper
    {
    public:
        /** @param may_create_room asked before a game is created: the gateway may not want any more rooms */
        explicit Gatekeeper(std::function<bool()> may_create_room = nullptr);

        /** Looks at a message from the browser. */
        Verdict inspect(const std::string &text);

        /** Looks at a message from the game server, and makes it fit for the browser. */
        Translation translate(const std::string &text);

        bool inGame() const { return in_game_; }
        bool roomCreated() const { return room_created_; }
        const std::optional<GameInfo> &game() const { return game_; }

        /** The gateway tells that a computer has joined the game: one is all a visitor gets. */
        void computerAdded() { computer_added_ = true; }

    private:
        Verdict decide(const std::string &text);
        Verdict forward(MessageType type, const std::string &payload);
        Verdict refuse(uint16_t code, const std::string &text);

        std::function<bool()> may_create_room_;

        std::string token_;
        bool connect_sent_ = false;
        bool create_pending_ = false;
        bool join_pending_ = false;
        bool in_game_ = false;
        bool room_created_ = false;
        bool computer_added_ = false;
        std::optional<GameInfo> game_;
    };
}
