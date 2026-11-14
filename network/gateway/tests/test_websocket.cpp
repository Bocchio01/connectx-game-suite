// Tests of the WebSocket codec: the handshake and the frames, all on strings

#include <string>
#include <vector>

#include "gateway/websocket.hpp"
#include "test_support.hpp"

using namespace websocket;

namespace
{
    std::string hex(const std::string &bytes)
    {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (unsigned char c : bytes)
        {
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0F]);
        }
        return out;
    }

    /** A frame as a browser sends it: always masked. */
    std::string clientFrame(Opcode opcode, const std::string &payload, bool fin = true, bool masked = true,
                            const char key[4] = "\x37\xfa\x21\x3d")
    {
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));

        const uint8_t mask_bit = masked ? 0x80 : 0x00;
        if (payload.size() < 126)
        {
            frame.push_back(static_cast<char>(mask_bit | payload.size()));
        }
        else if (payload.size() <= 0xFFFF)
        {
            frame.push_back(static_cast<char>(mask_bit | 126));
            frame.push_back(static_cast<char>(payload.size() >> 8));
            frame.push_back(static_cast<char>(payload.size() & 0xFF));
        }
        else
        {
            frame.push_back(static_cast<char>(mask_bit | 127));
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.push_back(static_cast<char>((static_cast<uint64_t>(payload.size()) >> shift) & 0xFF));
        }

        if (masked)
        {
            frame.append(key, 4);
            for (size_t i = 0; i < payload.size(); ++i)
                frame.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
        }
        else
        {
            frame += payload;
        }

        return frame;
    }

    Reader::Event readOne(const std::string &bytes, size_t max_message = MAX_MESSAGE_SIZE)
    {
        Reader reader(max_message);
        reader.feed(bytes.data(), bytes.size());
        return reader.next();
    }

    const std::string BROWSER_REQUEST =
        "GET /connectx/ws HTTP/1.1\r\n"
        "Host: api.bocchio.dev\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Origin: https://bocchio.dev\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
}

void test_hashing_and_encoding()
{
    // The examples of RFC 3174
    CHECK_EQ(hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_EQ(hex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK_EQ(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // Messages of every length around the size of a block
    CHECK_EQ(hex(sha1(std::string(55, 'a'))), "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK_EQ(hex(sha1(std::string(56, 'a'))), "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK_EQ(hex(sha1(std::string(63, 'a'))), "03f09f5b158a7a8cdad920bddc29b81c18a551f5");
    CHECK_EQ(hex(sha1(std::string(64, 'a'))), "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    CHECK_EQ(hex(sha1(std::string(65, 'a'))), "11655326c708d70319be2610e8a57d9a5b959d3b");
    CHECK_EQ(hex(sha1(std::string(119, 'a'))), "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56");
    CHECK_EQ(hex(sha1(std::string(120, 'a'))), "f34c1488385346a55709ba056ddd08280dd4c6d6");

    CHECK_EQ(base64(""), "");
    CHECK_EQ(base64("M"), "TQ==");
    CHECK_EQ(base64("Ma"), "TWE=");
    CHECK_EQ(base64("Man"), "TWFu");
    CHECK_EQ(base64("any carnal pleas"), "YW55IGNhcm5hbCBwbGVhcw==");
}

void test_accept_key_of_the_rfc()
{
    // RFC 6455, section 1.3
    CHECK_EQ(acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    const std::string response = acceptResponse("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(response.compare(0, 34, "HTTP/1.1 101 Switching Protocols\r\n") == 0);
    CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    CHECK(response.size() >= 4 && response.compare(response.size() - 4, 4, "\r\n\r\n") == 0);
}

void test_utf8_validation()
{
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("plain ascii"));
    CHECK(isValidUtf8("caff\xC3\xA8 \xE2\x82\xAC \xF0\x9F\x90\xBC")); // è € 🐼

    CHECK(!isValidUtf8("\xC3"));             // cut in the middle of a character
    CHECK(!isValidUtf8("\x80"));             // a continuation byte on its own
    CHECK(!isValidUtf8("\xC0\xAF"));         // overlong form of "/"
    CHECK(!isValidUtf8("\xE0\x80\xAF"));     // overlong again, in three bytes
    CHECK(!isValidUtf8("\xED\xA0\x80"));     // a surrogate
    CHECK(!isValidUtf8("\xF4\x90\x80\x80")); // beyond U+10FFFF
    CHECK(!isValidUtf8("\xFF"));
}

void test_parse_and_check_a_browser_request()
{
    const auto request = parseRequest(BROWSER_REQUEST);
    CHECK(request.has_value());
    CHECK_EQ(request->method, "GET");
    CHECK_EQ(request->path, "/connectx/ws");
    CHECK_EQ(request->version, "HTTP/1.1");
    CHECK_EQ(request->header("origin"), "https://bocchio.dev"); // names are in lower case
    CHECK_EQ(request->header("sec-websocket-version"), "13");
    CHECK_EQ(request->header("nothing-of-the-kind"), "");
    CHECK(checkUpgrade(*request) == Upgrade::OK);
}

void test_the_upgrade_is_checked_carefully()
{
    auto variant = [](const std::string &from, const std::string &to)
    {
        std::string text = BROWSER_REQUEST;
        const size_t at = text.find(from);
        text.replace(at, from.size(), to);
        return *parseRequest(text);
    };

    // Firefox says "keep-alive, Upgrade", and any capitalisation goes
    CHECK(checkUpgrade(variant("Connection: Upgrade", "Connection: keep-alive, Upgrade")) == Upgrade::OK);
    CHECK(checkUpgrade(variant("Upgrade: websocket", "upgrade: WebSocket")) == Upgrade::OK);

    CHECK(checkUpgrade(variant("GET", "POST")) == Upgrade::BAD_REQUEST);
    CHECK(checkUpgrade(variant("HTTP/1.1", "HTTP/1.0")) == Upgrade::BAD_REQUEST);
    CHECK(checkUpgrade(variant("Upgrade: websocket", "Upgrade: h2c")) == Upgrade::BAD_REQUEST);
    CHECK(checkUpgrade(variant("Connection: Upgrade", "Connection: close")) == Upgrade::BAD_REQUEST);
    CHECK(checkUpgrade(variant("dGhlIHNhbXBsZSBub25jZQ==", "tooshort")) == Upgrade::BAD_REQUEST);
    CHECK(checkUpgrade(variant("dGhlIHNhbXBsZSBub25jZQ==", "dGhlIHNhbXBsZSBub25jZQ!!")) == Upgrade::BAD_REQUEST);
    CHECK(checkUpgrade(variant("Version: 13", "Version: 8")) == Upgrade::UNSUPPORTED_VERSION);

    // Not an HTTP request at all
    CHECK(!parseRequest("").has_value());
    CHECK(!parseRequest("hello\r\n\r\n").has_value());
    CHECK(!parseRequest("GET /\r\n\r\n").has_value());
    CHECK(!parseRequest("GET / FTP/1.0\r\n\r\n").has_value());
    CHECK(!parseRequest("GET / HTTP/1.1\r\nno colon here\r\n\r\n").has_value());
}

void test_repeated_headers_are_joined()
{
    const auto request = parseRequest("GET / HTTP/1.1\r\nConnection: keep-alive\r\nConnection: Upgrade\r\n\r\n");
    CHECK(request.has_value());
    CHECK_EQ(request->header("connection"), "keep-alive, Upgrade");
}

void test_refusals()
{
    CHECK(rejectResponse(403).compare(0, 24, "HTTP/1.1 403 Forbidden\r\n") == 0);
    CHECK(rejectResponse(429).find("Too Many Requests") != std::string::npos);
    CHECK(rejectResponse(426).find("Sec-WebSocket-Version: 13") != std::string::npos);
    CHECK(rejectResponse(403).find("Connection: close") != std::string::npos);
    CHECK(rejectResponse(999).compare(0, 12, "HTTP/1.1 400") == 0); // anything else is a bad request
}

void test_encoding_frames()
{
    // 7-bit length: two bytes of header, not masked
    CHECK_EQ(hex(encode(Opcode::TEXT, "Hello")), "810548656c6c6f");
    CHECK_EQ(hex(encode(Opcode::PONG)), "8a00");

    // 16-bit length
    const std::string medium = encode(Opcode::TEXT, std::string(300, 'x'));
    CHECK_EQ(medium.size(), size_t(4 + 300));
    CHECK_EQ(hex(medium.substr(0, 4)), "817e012c");

    // 64-bit length
    const std::string big = encode(Opcode::TEXT, std::string(70000, 'x'));
    CHECK_EQ(big.size(), size_t(10 + 70000));
    CHECK_EQ(hex(big.substr(0, 10)), "817f0000000000011170");

    // Close: the status in two bytes, then the reason
    CHECK_EQ(hex(encodeClose(1000)), "880203e8");
    CHECK_EQ(hex(encodeClose(1001, "bye")), "880503e9627965");
    CHECK_EQ(encodeClose(1000, std::string(500, 'r')).size(), size_t(2 + 2 + 123)); // (a control frame is short)
}

void test_reading_a_masked_message()
{
    // The example of RFC 6455, section 5.7
    const std::string rfc("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
    const auto event = readOne(rfc);
    CHECK(event.type == Reader::Event::Type::MESSAGE);
    CHECK_EQ(event.data, "Hello");

    // Every length
    for (size_t length : {size_t(0), size_t(1), size_t(125), size_t(126), size_t(127), size_t(4000), size_t(8192)})
    {
        const std::string text(length, 'q');
        const auto got = readOne(clientFrame(Opcode::TEXT, text));
        CHECK(got.type == Reader::Event::Type::MESSAGE);
        CHECK_EQ(got.data.size(), length);
    }
}

void test_bytes_may_arrive_in_pieces_and_together()
{
    const std::string frame = clientFrame(Opcode::TEXT, "{\"type\":4}");

    Reader slow;
    for (size_t i = 0; i < frame.size(); ++i)
    {
        CHECK(slow.next().type == Reader::Event::Type::NONE);
        slow.feed(&frame[i], 1);
    }
    const auto event = slow.next();
    CHECK(event.type == Reader::Event::Type::MESSAGE);
    CHECK_EQ(event.data, "{\"type\":4}");
    CHECK(slow.next().type == Reader::Event::Type::NONE);

    // Three frames in one go
    Reader fast;
    const std::string all = clientFrame(Opcode::TEXT, "one") + clientFrame(Opcode::PING, "p") + clientFrame(Opcode::TEXT, "two");
    fast.feed(all.data(), all.size());
    CHECK_EQ(fast.next().data, "one");
    CHECK(fast.next().type == Reader::Event::Type::PING);
    CHECK_EQ(fast.next().data, "two");
    CHECK(fast.next().type == Reader::Event::Type::NONE);
}

void test_a_message_in_several_frames()
{
    Reader reader;
    const std::string frames = clientFrame(Opcode::TEXT, "Hel", false) +
                               clientFrame(Opcode::PING, "mid-way") + // control frames may come in between
                               clientFrame(Opcode::CONTINUATION, "lo, ", false) +
                               clientFrame(Opcode::CONTINUATION, "world", true);
    reader.feed(frames.data(), frames.size());

    const auto ping = reader.next();
    CHECK(ping.type == Reader::Event::Type::PING);
    CHECK_EQ(ping.data, "mid-way");

    const auto message = reader.next();
    CHECK(message.type == Reader::Event::Type::MESSAGE);
    CHECK_EQ(message.data, "Hello, world");
}

void test_control_frames()
{
    auto ping = readOne(clientFrame(Opcode::PING, "are you there"));
    CHECK(ping.type == Reader::Event::Type::PING);
    CHECK_EQ(ping.data, "are you there");

    auto pong = readOne(clientFrame(Opcode::PONG, "yes"));
    CHECK(pong.type == Reader::Event::Type::PONG);

    // Close: with a status and a reason, with nothing
    const std::string payload = std::string("\x03\xe9", 2) + "going";
    auto close = readOne(clientFrame(Opcode::CLOSE, payload));
    CHECK(close.type == Reader::Event::Type::CLOSE);
    CHECK_EQ(close.code, 1001);
    CHECK_EQ(close.data, "going");

    auto bare = readOne(clientFrame(Opcode::CLOSE, ""));
    CHECK(bare.type == Reader::Event::Type::CLOSE);
    CHECK_EQ(bare.code, CloseCode::NO_STATUS);
}

void test_nothing_after_a_close()
{
    Reader reader;
    const std::string bytes = clientFrame(Opcode::CLOSE, std::string("\x03\xe8", 2)) + clientFrame(Opcode::TEXT, "late");
    reader.feed(bytes.data(), bytes.size());

    CHECK(reader.next().type == Reader::Event::Type::CLOSE);
    CHECK(reader.next().type == Reader::Event::Type::NONE);

    const std::string more = clientFrame(Opcode::TEXT, "later");
    reader.feed(more.data(), more.size());
    CHECK(reader.next().type == Reader::Event::Type::NONE);
}

void test_protocol_violations()
{
    using Type = Reader::Event::Type;

    // Not masked
    auto event = readOne(clientFrame(Opcode::TEXT, "hi", true, false));
    CHECK(event.type == Type::FAILURE);
    CHECK_EQ(event.code, CloseCode::PROTOCOL_ERROR);

    // A reserved bit (there is no extension that could use it)
    std::string reserved = clientFrame(Opcode::TEXT, "hi");
    reserved[0] = static_cast<char>(reserved[0] | 0x40);
    event = readOne(reserved);
    CHECK(event.type == Type::FAILURE);
    CHECK_EQ(event.code, CloseCode::PROTOCOL_ERROR);

    // An opcode that does not exist
    std::string unknown = clientFrame(Opcode::TEXT, "hi");
    unknown[0] = static_cast<char>(0x83);
    event = readOne(unknown);
    CHECK(event.type == Type::FAILURE);

    // A control frame that is split, or longer than 125 bytes
    event = readOne(clientFrame(Opcode::PING, "x", false));
    CHECK(event.type == Type::FAILURE);
    event = readOne(clientFrame(Opcode::PING, std::string(126, 'x')));
    CHECK(event.type == Type::FAILURE);

    // A close frame cannot have a status of one byte
    event = readOne(clientFrame(Opcode::CLOSE, "x"));
    CHECK(event.type == Type::FAILURE);

    // The rest of a message that never began, a message that begins before the last is over
    event = readOne(clientFrame(Opcode::CONTINUATION, "tail"));
    CHECK(event.type == Type::FAILURE);

    Reader reader;
    const std::string two = clientFrame(Opcode::TEXT, "first", false) + clientFrame(Opcode::TEXT, "second");
    reader.feed(two.data(), two.size());
    CHECK(reader.next().type == Type::FAILURE);
    CHECK(reader.next().type == Type::NONE); // and nothing more is said after a failure

    // Binary data is not what is spoken here
    event = readOne(clientFrame(Opcode::BINARY, "\x01\x02"));
    CHECK(event.type == Type::FAILURE);
    CHECK_EQ(event.code, CloseCode::UNSUPPORTED_DATA);

    // The most significant bit of a 64-bit length must be 0
    std::string huge = clientFrame(Opcode::TEXT, "");
    huge[1] = static_cast<char>(0x80 | 127);
    huge.insert(2, std::string("\x80\x00\x00\x00\x00\x00\x00\x00", 8));
    event = readOne(huge);
    CHECK(event.type == Type::FAILURE);
    CHECK_EQ(event.code, CloseCode::PROTOCOL_ERROR);
}

void test_limits_and_encoding_of_messages()
{
    using Type = Reader::Event::Type;

    // At the limit it is fine, one byte more is too much: and it is said before the whole message has arrived
    CHECK(readOne(clientFrame(Opcode::TEXT, std::string(100, 'x')), 100).type == Type::MESSAGE);

    auto event = readOne(clientFrame(Opcode::TEXT, std::string(101, 'x')), 100);
    CHECK(event.type == Type::FAILURE);
    CHECK_EQ(event.code, CloseCode::MESSAGE_TOO_BIG);

    Reader reader(100);
    const std::string header = clientFrame(Opcode::TEXT, std::string(5000, 'x')).substr(0, 8);
    reader.feed(header.data(), header.size());
    CHECK(reader.next().type == Type::FAILURE);

    // The pieces of a message count together
    Reader pieces(100);
    const std::string split = clientFrame(Opcode::TEXT, std::string(60, 'x'), false) + clientFrame(Opcode::CONTINUATION, std::string(60, 'x'));
    pieces.feed(split.data(), split.size());
    CHECK(pieces.next().type == Type::FAILURE);

    // Text must be UTF-8
    event = readOne(clientFrame(Opcode::TEXT, "\xC3\x28"));
    CHECK(event.type == Type::FAILURE);
    CHECK_EQ(event.code, CloseCode::INVALID_DATA);
}

int main()
{
    RUN_TEST(test_hashing_and_encoding);
    RUN_TEST(test_accept_key_of_the_rfc);
    RUN_TEST(test_utf8_validation);
    RUN_TEST(test_parse_and_check_a_browser_request);
    RUN_TEST(test_the_upgrade_is_checked_carefully);
    RUN_TEST(test_repeated_headers_are_joined);
    RUN_TEST(test_refusals);
    RUN_TEST(test_encoding_frames);
    RUN_TEST(test_reading_a_masked_message);
    RUN_TEST(test_bytes_may_arrive_in_pieces_and_together);
    RUN_TEST(test_a_message_in_several_frames);
    RUN_TEST(test_control_frames);
    RUN_TEST(test_nothing_after_a_close);
    RUN_TEST(test_protocol_violations);
    RUN_TEST(test_limits_and_encoding_of_messages);
    return testSummary("test_websocket");
}
