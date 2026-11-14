#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sstream>

#include "gateway/websocket.hpp"

namespace websocket
{
    namespace
    {
        /** The value that is appended to the client's key before hashing it (RFC 6455, section 1.3). */
        const char *const HANDSHAKE_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string trim(const std::string &text)
        {
            size_t begin = 0;
            size_t end = text.size();

            while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
                ++begin;
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
                --end;

            return text.substr(begin, end - begin);
        }

        /** Whether a comma separated header value (e.g. "keep-alive, Upgrade") contains the token. */
        bool hasToken(const std::string &value, const std::string &token)
        {
            std::istringstream stream(lower(value));
            std::string item;

            while (std::getline(stream, item, ','))
            {
                if (trim(item) == token)
                    return true;
            }

            return false;
        }

        uint32_t rotateLeft(uint32_t value, int bits)
        {
            return (value << bits) | (value >> (32 - bits));
        }

        void append16(std::string &out, uint16_t value)
        {
            out.push_back(static_cast<char>(value >> 8));
            out.push_back(static_cast<char>(value & 0xFF));
        }
    }

    // ========================================================================
    // Small helpers
    // ========================================================================

    std::string sha1(const std::string &data)
    {
        uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

        // Padding: a 1 bit, zeros up to 56 bytes in a block of 64, then the length in bits
        std::string message = data;
        message.push_back(static_cast<char>(0x80));
        while (message.size() % 64 != 56)
            message.push_back('\0');

        const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
        for (int shift = 56; shift >= 0; shift -= 8)
            message.push_back(static_cast<char>((bits >> shift) & 0xFF));

        for (size_t block = 0; block < message.size(); block += 64)
        {
            uint32_t w[80];

            for (int i = 0; i < 16; ++i)
            {
                const unsigned char *p = reinterpret_cast<const unsigned char *>(&message[block + i * 4]);
                w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            }
            for (int i = 16; i < 80; ++i)
                w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

            for (int i = 0; i < 80; ++i)
            {
                uint32_t f;
                uint32_t k;

                if (i < 20)
                {
                    f = (b & c) | (~b & d);
                    k = 0x5A827999;
                }
                else if (i < 40)
                {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                }
                else if (i < 60)
                {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                }
                else
                {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }

                const uint32_t next = rotateLeft(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rotateLeft(b, 30);
                b = a;
                a = next;
            }

            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }

        std::string digest;
        for (uint32_t word : h)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                digest.push_back(static_cast<char>((word >> shift) & 0xFF));
        }

        return digest;
    }

    std::string base64(const std::string &bytes)
    {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);

        for (size_t i = 0; i < bytes.size(); i += 3)
        {
            const uint32_t chunk = (uint32_t(static_cast<unsigned char>(bytes[i])) << 16) |
                                   (i + 1 < bytes.size() ? uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8 : 0) |
                                   (i + 2 < bytes.size() ? uint32_t(static_cast<unsigned char>(bytes[i + 2])) : 0);

            out.push_back(alphabet[(chunk >> 18) & 0x3F]);
            out.push_back(alphabet[(chunk >> 12) & 0x3F]);
            out.push_back(i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 0x3F] : '=');
            out.push_back(i + 2 < bytes.size() ? alphabet[chunk & 0x3F] : '=');
        }

        return out;
    }

    bool isValidUtf8(const std::string &text)
    {
        size_t i = 0;

        while (i < text.size())
        {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            size_t length;
            uint32_t code;

            if (lead < 0x80)
            {
                ++i;
                continue;
            }
            else if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
                code = lead & 0x1F;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                length = 3;
                code = lead & 0x0F;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                code = lead & 0x07;
            }
            else
            {
                return false; // a continuation byte on its own, an overlong lead (C0, C1) or beyond U+10FFFF
            }

            if (i + length > text.size())
                return false;

            for (size_t k = 1; k < length; ++k)
            {
                const unsigned char next = static_cast<unsigned char>(text[i + k]);
                if ((next & 0xC0) != 0x80)
                    return false;
                code = (code << 6) | (next & 0x3F);
            }

            // Shorter sequences than needed, the surrogates and what is past the last code point
            if ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
                (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
                return false;

            i += length;
        }

        return true;
    }

    // ========================================================================
    // Opening handshake
    // ========================================================================

    std::string HandshakeRequest::header(const std::string &name) const
    {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }

    std::optional<HandshakeRequest> parseRequest(const std::string &head)
    {
        std::istringstream stream(head);
        std::string line;

        if (!std::getline(stream, line))
            return std::nullopt;

        // Request line: METHOD SP target SP HTTP/x.y
        std::istringstream request_line(trim(line));
        HandshakeRequest request;
        std::string extra;

        if (!(request_line >> request.method >> request.path >> request.version) || (request_line >> extra))
            return std::nullopt;
        if (request.version.compare(0, 5, "HTTP/") != 0)
            return std::nullopt;

        while (std::getline(stream, line))
        {
            line = trim(line);
            if (line.empty())
                break;

            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0)
                return std::nullopt;

            const std::string name = lower(trim(line.substr(0, colon)));
            const std::string value = trim(line.substr(colon + 1));

            auto &stored = request.headers[name];
            stored = stored.empty() ? value : stored + ", " + value;
        }

        return request;
    }

    Upgrade checkUpgrade(const HandshakeRequest &request)
    {
        if (request.method != "GET" || request.version != "HTTP/1.1")
            return Upgrade::BAD_REQUEST;
        if (!hasToken(request.header("upgrade"), "websocket") || !hasToken(request.header("connection"), "upgrade"))
            return Upgrade::BAD_REQUEST;

        // The key is 16 random bytes in base64: 24 characters, the last two being padding
        const std::string key = request.header("sec-websocket-key");
        const bool key_ok = key.size() == 24 && key.compare(22, 2, "==") == 0 &&
                            std::all_of(key.begin(), key.end() - 2, [](unsigned char c)
                                        { return std::isalnum(c) || c == '+' || c == '/'; });
        if (!key_ok)
            return Upgrade::BAD_REQUEST;

        if (request.header("sec-websocket-version") != "13")
            return Upgrade::UNSUPPORTED_VERSION;

        return Upgrade::OK;
    }

    std::string acceptKey(const std::string &client_key)
    {
        return base64(sha1(client_key + HANDSHAKE_GUID));
    }

    std::string acceptResponse(const std::string &client_key)
    {
        return "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " +
               acceptKey(client_key) + "\r\n\r\n";
    }

    std::string rejectResponse(int status)
    {
        const char *reason = "Bad Request";
        switch (status)
        {
        case 403:
            reason = "Forbidden";
            break;
        case 404:
            reason = "Not Found";
            break;
        case 426:
            reason = "Upgrade Required";
            break;
        case 429:
            reason = "Too Many Requests";
            break;
        case 503:
            reason = "Service Unavailable";
            break;
        default:
            status = 400;
            break;
        }

        std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";

        // Tells a client that does speak WebSocket which version to use
        if (status == 426)
            response += "Sec-WebSocket-Version: 13\r\n";

        return response + "Connection: close\r\nContent-Length: 0\r\n\r\n";
    }

    // ========================================================================
    // Frames
    // ========================================================================

    std::string encode(Opcode opcode, const std::string &payload)
    {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode))); // FIN set, no reserved bits

        // Frames from a server are not masked
        if (payload.size() < 126)
        {
            frame.push_back(static_cast<char>(payload.size()));
        }
        else if (payload.size() <= 0xFFFF)
        {
            frame.push_back(126);
            append16(frame, static_cast<uint16_t>(payload.size()));
        }
        else
        {
            frame.push_back(127);
            const uint64_t size = payload.size();
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.push_back(static_cast<char>((size >> shift) & 0xFF));
        }

        return frame + payload;
    }

    std::string encodeClose(uint16_t code, const std::string &reason)
    {
        std::string payload;
        append16(payload, code);

        // A control frame holds 125 bytes at most: 2 for the status, the rest for the reason
        payload += reason.substr(0, 123);

        return encode(Opcode::CLOSE, payload);
    }

    // ========================================================================
    // Reader
    // ========================================================================

    Reader::Reader(size_t max_message_size)
        : max_message_size_(max_message_size)
    {
    }

    void Reader::feed(const char *data, size_t size)
    {
        if (!finished_)
            buffer_.append(data, size);
    }

    Reader::Event Reader::fail(uint16_t code)
    {
        finished_ = true;
        buffer_.clear();
        message_.clear();

        Event event;
        event.type = Event::Type::FAILURE;
        event.code = code;
        return event;
    }

    Reader::Event Reader::next()
    {
        while (!finished_)
        {
            if (buffer_.size() < 2)
                return Event();

            const unsigned char first = static_cast<unsigned char>(buffer_[0]);
            const unsigned char second = static_cast<unsigned char>(buffer_[1]);

            const bool fin = (first & 0x80) != 0;
            const auto opcode = static_cast<Opcode>(first & 0x0F);
            const bool control = (first & 0x08) != 0;
            const bool masked = (second & 0x80) != 0;

            // No extension was agreed on, so a reserved bit means that the peer is not speaking our language
            if ((first & 0x70) != 0)
                return fail(CloseCode::PROTOCOL_ERROR);

            switch (opcode)
            {
            case Opcode::CONTINUATION:
            case Opcode::TEXT:
            case Opcode::BINARY:
            case Opcode::CLOSE:
            case Opcode::PING:
            case Opcode::PONG:
                break;
            default:
                return fail(CloseCode::PROTOCOL_ERROR);
            }

            // Everything a browser sends is masked: a frame that is not is a protocol violation
            if (!masked)
                return fail(CloseCode::PROTOCOL_ERROR);

            // Length: 7 bits, or 16 or 64 more
            size_t offset = 2;
            uint64_t length = second & 0x7F;

            if (length == 126)
            {
                if (buffer_.size() < offset + 2)
                    return Event();
                length = (uint64_t(static_cast<unsigned char>(buffer_[2])) << 8) | static_cast<unsigned char>(buffer_[3]);
                offset += 2;
            }
            else if (length == 127)
            {
                if (buffer_.size() < offset + 8)
                    return Event();
                length = 0;
                for (int i = 0; i < 8; ++i)
                    length = (length << 8) | static_cast<unsigned char>(buffer_[offset + i]);
                offset += 8;

                if (length >> 63)
                    return fail(CloseCode::PROTOCOL_ERROR);
            }

            // A control frame is whole and short; a data frame must not make a message that is too long
            if (control && (!fin || length > 125))
                return fail(CloseCode::PROTOCOL_ERROR);
            if (length > max_message_size_ || (!control && message_.size() + length > max_message_size_))
                return fail(CloseCode::MESSAGE_TOO_BIG);

            if (buffer_.size() < offset + 4 + length)
                return Event();

            const char *mask = &buffer_[offset];
            std::string payload(buffer_, offset + 4, static_cast<size_t>(length));
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);

            buffer_.erase(0, offset + 4 + static_cast<size_t>(length));

            Event event;

            switch (opcode)
            {
            case Opcode::PING:
                event.type = Event::Type::PING;
                event.data = std::move(payload);
                return event;

            case Opcode::PONG:
                event.type = Event::Type::PONG;
                event.data = std::move(payload);
                return event;

            case Opcode::CLOSE:
            {
                // Empty, or a 16-bit status and then a reason; one byte alone cannot be right
                if (payload.size() == 1)
                    return fail(CloseCode::PROTOCOL_ERROR);

                event.type = Event::Type::CLOSE;
                event.code = CloseCode::NO_STATUS;

                if (payload.size() >= 2)
                {
                    event.code = static_cast<uint16_t>((static_cast<unsigned char>(payload[0]) << 8) |
                                                       static_cast<unsigned char>(payload[1]));
                    event.data = payload.substr(2);
                }

                finished_ = true;
                buffer_.clear();
                return event;
            }

            case Opcode::BINARY:
                // Only text is spoken here
                return fail(CloseCode::UNSUPPORTED_DATA);

            case Opcode::TEXT:
            case Opcode::CONTINUATION:
                if (opcode == Opcode::TEXT && in_message_)
                    return fail(CloseCode::PROTOCOL_ERROR); // a new message before the last one is over
                if (opcode == Opcode::CONTINUATION && !in_message_)
                    return fail(CloseCode::PROTOCOL_ERROR); // the rest of a message that never began

                message_ += payload;
                in_message_ = !fin;

                if (!fin)
                    continue;

                if (!isValidUtf8(message_))
                    return fail(CloseCode::INVALID_DATA);

                event.type = Event::Type::MESSAGE;
                event.data = std::move(message_);
                message_.clear();
                return event;
            }
        }

        return Event();
    }
}
