#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "protocol/constants.hpp"

/**
 * Just enough of WebSocket (RFC 6455) to be the server side of a browser connection: the opening handshake
 * and the frames. There are no extensions and no sub-protocols, and the messages are text (JSON).
 *
 * Everything here works on strings, never on sockets, so that it can be tested in isolation.
 */
namespace websocket
{
    /** Longest message accepted, in either direction: the same limit as the game protocol. */
    constexpr size_t MAX_MESSAGE_SIZE = Protocol::MAX_MESSAGE_SIZE;

    enum class Opcode : uint8_t
    {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };

    /** Status codes of a Close frame (RFC 6455, section 7.4). */
    namespace CloseCode
    {
        constexpr uint16_t NORMAL = 1000;
        constexpr uint16_t GOING_AWAY = 1001;
        constexpr uint16_t PROTOCOL_ERROR = 1002;
        constexpr uint16_t UNSUPPORTED_DATA = 1003;
        constexpr uint16_t NO_STATUS = 1005; // never sent: what we report when the peer sent none
        constexpr uint16_t ABNORMAL = 1006;  // never sent: the connection dropped without a Close frame
        constexpr uint16_t INVALID_DATA = 1007;
        constexpr uint16_t POLICY_VIOLATION = 1008;
        constexpr uint16_t MESSAGE_TOO_BIG = 1009;
        constexpr uint16_t INTERNAL_ERROR = 1011;
        constexpr uint16_t TRY_AGAIN_LATER = 1013;
    }

    // ========================================================================
    // Small helpers
    // ========================================================================

    /** SHA-1 digest of the data: 20 raw bytes. */
    std::string sha1(const std::string &data);

    /** Base64 encoding of the bytes. */
    std::string base64(const std::string &bytes);

    /** Whether the text is well formed UTF-8 (no overlong forms, no surrogates, nothing beyond U+10FFFF). */
    bool isValidUtf8(const std::string &text);

    // ========================================================================
    // Opening handshake
    // ========================================================================

    /** The HTTP request a browser sends to open a connection. */
    struct HandshakeRequest
    {
        std::string method;
        std::string path;
        std::string version;
        std::map<std::string, std::string> headers; // names in lower case; repeated headers are joined with ", "

        /** Value of a header (name in lower case), empty if it is missing. */
        std::string header(const std::string &name) const;
    };

    /**
     * Parses the head of an HTTP request: the request line and the headers, up to the blank line.
     * @return std::nullopt if it is not an HTTP request at all
     */
    std::optional<HandshakeRequest> parseRequest(const std::string &head);

    enum class Upgrade
    {
        OK,
        BAD_REQUEST,        // not a request to open a WebSocket
        UNSUPPORTED_VERSION // a WebSocket one, but not of version 13, the only one there is
    };

    /** Checks that the request is a valid WebSocket opening handshake. */
    Upgrade checkUpgrade(const HandshakeRequest &request);

    /** The `Sec-WebSocket-Accept` value that answers a `Sec-WebSocket-Key`. */
    std::string acceptKey(const std::string &client_key);

    /** The response that accepts the handshake. */
    std::string acceptResponse(const std::string &client_key);

    /** The response that refuses a connection with an HTTP status (400, 403, 404, 426, 429, 503). */
    std::string rejectResponse(int status);

    // ========================================================================
    // Frames
    // ========================================================================

    /** A frame as the server sends it: whole (FIN set) and not masked. */
    std::string encode(Opcode opcode, const std::string &payload = std::string());

    /** A Close frame. */
    std::string encodeClose(uint16_t code, const std::string &reason = std::string());

    /**
     * Turns the bytes received from a browser into events, one after the other.
     *
     * It checks everything a server has to check: the frames must be masked, there are no extensions (so
     * no reserved bits), the control frames are short and whole, a message that is split into several
     * frames is put back together, it is not longer than the limit and it is valid UTF-8.
     * After a Close frame or a failure it produces nothing more.
     */
    class Reader
    {
    public:
        struct Event
        {
            enum class Type
            {
                NONE,    // nothing complete yet: feed it more bytes
                MESSAGE, // a text message; `data` is the text
                PING,    // `data` is the payload, to be sent back in a Pong
                PONG,
                CLOSE,  // the peer closes: `code` is its status (NO_STATUS if it gave none), `data` its reason
                FAILURE // the peer broke the rules: the connection must be closed with the status `code`
            };

            Type type = Type::NONE;
            std::string data;
            uint16_t code = 0;
        };

        explicit Reader(size_t max_message_size = MAX_MESSAGE_SIZE);

        /** Adds bytes that have just been received. */
        void feed(const char *data, size_t size);

        /** The next event, or one of type NONE if the bytes so far do not make one. Call until it is NONE. */
        Event next();

    private:
        Event fail(uint16_t code);

        size_t max_message_size_;
        std::string buffer_;
        std::string message_;       // the message being put together
        bool in_message_ = false;   // a message split in several frames has started
        bool finished_ = false;     // a Close frame or a failure has been reported
    };
}
