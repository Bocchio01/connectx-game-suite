// Integration tests of the gateway: a real game server and a real gateway run inside the test process,
// and the "browsers" are WebSocket clients written here (they speak just enough of the protocol).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <sockpp/tcp_connector.h>

#include "gateway/gateway.hpp"
#include "gateway/gatekeeper.hpp"
#include "gateway/socket_util.hpp"
#include "gateway/websocket.hpp"
#include "server/game_server.hpp"
#include "test_support.hpp"

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
    constexpr const char *ORIGIN = "https://bocchio.dev";

    // ========================================================================
    // A game server and a gateway, each in a thread of its own
    // ========================================================================

    class TestServer
    {
    public:
        TestServer()
        {
            for (uint16_t port = 38200; port < 38300 && !started_; ++port)
            {
                server_ = std::make_unique<GameServer>(port, "127.0.0.1");
                finished_ = false;
                thread_ = std::thread([this]()
                                      {
                    server_->start();
                    finished_ = true; });

                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!server_->isRunning() && !finished_ && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(5ms);

                if (server_->isRunning())
                {
                    port_ = port;
                    started_ = true;
                }
                else if (thread_.joinable())
                {
                    thread_.join();
                }
            }
        }

        ~TestServer()
        {
            if (!started_)
                return;

            server_->stop();

            // Closing the listening socket does not wake accept() up everywhere: knock on the door
            if (!finished_)
            {
                sockpp::tcp_connector poke;
                poke.connect(sockpp::inet_address("127.0.0.1", port_), 200ms);
            }

            if (thread_.joinable())
                thread_.join();
        }

        bool ok() const { return started_; }
        uint16_t port() const { return port_; }

    private:
        std::unique_ptr<GameServer> server_;
        std::thread thread_;
        std::atomic<bool> finished_{false};
        bool started_ = false;
        uint16_t port_ = 0;
    };

    class TestGateway
    {
    public:
        explicit TestGateway(gateway::Settings settings)
        {
            settings.bind_address = "127.0.0.1";
            settings.port = 0; // any free port

            gateway_ = std::make_unique<gateway::Gateway>(settings);
            thread_ = std::thread([this]()
                                  { gateway_->start(); });

            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!gateway_->isRunning() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
        }

        ~TestGateway()
        {
            gateway_->stop();
            if (thread_.joinable())
                thread_.join();
        }

        bool ok() const { return gateway_->isRunning(); }
        uint16_t port() const { return gateway_->port(); }
        size_t connections() { return gateway_->connectionCount(); }

    private:
        std::unique_ptr<gateway::Gateway> gateway_;
        std::thread thread_;
    };

    gateway::Settings settingsFor(const TestServer &server)
    {
        gateway::Settings settings;
        settings.server_host = "127.0.0.1";
        settings.server_port = server.port();
        settings.allowed_origins = {ORIGIN};
        return settings;
    }

    // ========================================================================
    // A browser
    // ========================================================================

    /** Something that comes from the gateway: a message, or the end of the connection. */
    struct Incoming
    {
        enum class Kind
        {
            MESSAGE,
            CLOSE,   // a Close frame: `code`
            DROPPED, // the connection ended without one
            PING,
            TIMEOUT
        };

        Kind kind = Kind::TIMEOUT;
        json message;
        uint16_t code = 0;
    };

    class Browser
    {
    public:
        /** Connects and goes through the opening handshake; ok() tells how it went. */
        explicit Browser(uint16_t port, const std::string &origin = ORIGIN, const std::string &path = "/connectx/ws",
                         const std::string &extra_headers = "", bool answer_pings = true)
            : answer_pings_(answer_pings)
        {
            if (!socket_.connect(sockpp::inet_address("127.0.0.1", port), 2s))
                return;

            const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
            std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                                  key + "\r\n";
            if (!origin.empty())
                request += "Origin: " + origin + "\r\n";
            request += extra_headers + "\r\n";

            gateway::sendAll(socket_, request.data(), request.size());

            // The answer: up to the blank line
            std::string head;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (head.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                char c;
                if (!gateway::waitForData(socket_, 50))
                    continue;
                if (socket_.read(&c, 1) != 1)
                    break;
                head.push_back(c);
            }

            status_ = head.size() > 12 ? std::atoi(head.c_str() + 9) : 0;
            accepted_ = status_ == 101 && head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
        }

        bool ok() const { return accepted_; }
        int status() const { return status_; }

        void send(const json &message)
        {
            sendFrame(websocket::Opcode::TEXT, message.dump());
        }

        void sendText(const std::string &text)
        {
            sendFrame(websocket::Opcode::TEXT, text);
        }

        void sendFrame(websocket::Opcode opcode, const std::string &payload)
        {
            // What a browser does: always masked
            const char key[4] = {0x12, 0x34, 0x56, 0x78};
            std::string frame;
            frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

            if (payload.size() < 126)
            {
                frame.push_back(static_cast<char>(0x80 | payload.size()));
            }
            else
            {
                frame.push_back(static_cast<char>(0x80 | 126));
                frame.push_back(static_cast<char>(payload.size() >> 8));
                frame.push_back(static_cast<char>(payload.size() & 0xFF));
            }

            frame.append(key, 4);
            for (size_t i = 0; i < payload.size(); ++i)
                frame.push_back(static_cast<char>(payload[i] ^ key[i % 4]));

            gateway::sendAll(socket_, frame.data(), frame.size());
        }

        /** The next thing from the gateway (pings are answered here, unless this browser is deaf). */
        Incoming next(std::chrono::milliseconds timeout = 3s)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            // Nothing came in time, or the connection ended (in the middle of a frame, perhaps)
            auto failed = [this]()
            {
                return ended_ ? Incoming{Incoming::Kind::DROPPED, json(), 0} : Incoming{};
            };

            for (;;)
            {
                uint8_t header[2];
                if (!readExactly(reinterpret_cast<char *>(header), 2, deadline))
                    return failed();

                const auto opcode = static_cast<websocket::Opcode>(header[0] & 0x0F);
                uint64_t length = header[1] & 0x7F;

                if (length == 126)
                {
                    uint8_t extra[2];
                    if (!readExactly(reinterpret_cast<char *>(extra), 2, deadline))
                        return failed();
                    length = (extra[0] << 8) | extra[1];
                }
                else if (length == 127)
                {
                    uint8_t extra[8];
                    if (!readExactly(reinterpret_cast<char *>(extra), 8, deadline))
                        return failed();
                    length = 0;
                    for (uint8_t b : extra)
                        length = (length << 8) | b;
                }

                std::string payload(static_cast<size_t>(length), '\0');
                if (length > 0 && !readExactly(&payload[0], static_cast<size_t>(length), deadline))
                    return failed();

                Incoming incoming;

                switch (opcode)
                {
                case websocket::Opcode::TEXT:
                    incoming.kind = Incoming::Kind::MESSAGE;
                    incoming.message = json::parse(payload, nullptr, false);
                    return incoming;

                case websocket::Opcode::PING:
                    if (answer_pings_)
                    {
                        sendFrame(websocket::Opcode::PONG, payload);
                        continue;
                    }
                    incoming.kind = Incoming::Kind::PING;
                    return incoming;

                case websocket::Opcode::PONG:
                    continue;

                case websocket::Opcode::CLOSE:
                    incoming.kind = Incoming::Kind::CLOSE;
                    incoming.code = payload.size() >= 2 ? static_cast<uint16_t>((uint8_t(payload[0]) << 8) | uint8_t(payload[1])) : 1005;
                    return incoming;

                default:
                    continue;
                }
            }
        }

        /** Waits for a message of the given type, ignoring the others; null if it does not come. */
        json expect(int type, std::chrono::milliseconds timeout = 3s)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (std::chrono::steady_clock::now() < deadline)
            {
                const Incoming incoming = next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));

                if (incoming.kind == Incoming::Kind::MESSAGE && incoming.message.is_object() && incoming.message["type"].get<int>() == type)
                    return incoming.message["payload"];
                if (incoming.kind == Incoming::Kind::CLOSE || incoming.kind == Incoming::Kind::DROPPED)
                    break;
            }

            return json();
        }

        /** Connects and introduces itself: the token stays in the gateway. */
        bool introduce(const std::string &name)
        {
            send({{"type", 0x01}, {"payload", {{"player_name", name}, {"protocol_version", 1}}}});
            const json response = expect(0x02);
            return response.is_object() && response.value("success", false);
        }

        /** The games of the lobby, by name. */
        std::vector<json> games()
        {
            send({{"type", 0x22}, {"payload", json::object()}});
            const json response = expect(0x23);
            std::vector<json> list;
            if (response.is_object())
                for (const json &game : response["games"])
                    list.push_back(game);
            return list;
        }

        /** Who is to move, from a state update. */
        static int currentPlayer(const json &state) { return state["current_player"].get<int>(); }

    private:
        bool readExactly(char *buffer, size_t size, std::chrono::steady_clock::time_point deadline)
        {
            size_t received = 0;

            while (received < size)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                if (!gateway::waitForData(socket_, 50))
                    continue;

                const ssize_t n = socket_.read(buffer + received, size - received);
                if (n <= 0)
                {
                    ended_ = true;
                    return false;
                }

                received += static_cast<size_t>(n);
            }

            return true;
        }

        sockpp::tcp_connector socket_;
        bool answer_pings_;
        bool accepted_ = false;
        bool ended_ = false;
        int status_ = 0;
    };

    json createRequest(const std::string &name, int rows = 6, int cols = 7, int players = 2, int connect = 4)
    {
        return {{"type", 0x20},
                {"payload", {{"name", name}, {"config", {{"rows", rows}, {"cols", cols}, {"num_players", players}, {"connect_length", connect}}}}}};
    }

    int errorCode(const json &payload)
    {
        return payload.is_object() ? payload.value("error_code", -1) : -2;
    }
}

// ============================================================================
// The opening handshake
// ============================================================================

void test_who_may_connect()
{
    TestServer server;
    CHECK(server.ok());
    TestGateway gateway(settingsFor(server));
    CHECK(gateway.ok());

    Browser good(gateway.port());
    CHECK(good.ok());
    CHECK_EQ(good.status(), 101);

    // The path of the reverse proxy, and the root, are fine; anything else is not
    CHECK(Browser(gateway.port(), ORIGIN, "/").ok());
    CHECK_EQ(Browser(gateway.port(), ORIGIN, "/somewhere/else").status(), 404);

    // Only the pages that are allowed to, and only browsers (which always say which page they are)
    CHECK_EQ(Browser(gateway.port(), "https://evil.example").status(), 403);
    CHECK_EQ(Browser(gateway.port(), "").status(), 403);

    // A request that is no WebSocket handshake
    sockpp::tcp_connector plain;
    CHECK(plain.connect(sockpp::inet_address("127.0.0.1", gateway.port()), 2s));
    const std::string request = "GET /connectx/ws HTTP/1.1\r\nHost: localhost\r\nOrigin: " + std::string(ORIGIN) + "\r\n\r\n";
    plain.write_n(request.data(), request.size());
    char answer[64] = {0};
    CHECK(gateway::waitForData(plain, 2000));
    plain.read(answer, sizeof(answer) - 1);
    CHECK(std::string(answer).compare(0, 12, "HTTP/1.1 400") == 0);
}

void test_any_page_may_connect_when_none_is_listed()
{
    TestServer server;
    gateway::Settings settings = settingsFor(server);
    settings.allowed_origins.clear();
    TestGateway gateway(settings);

    CHECK(Browser(gateway.port(), "https://anything.example").ok());
    CHECK(Browser(gateway.port(), "").ok());
}

// ============================================================================
// Playing
// ============================================================================

void test_connecting_hides_the_token()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser browser(gateway.port());
    CHECK(browser.ok());

    browser.send({{"type", 0x01}, {"payload", {{"player_name", "Ada"}, {"protocol_version", 1}}}});
    const json response = browser.expect(0x02);

    CHECK(response.is_object());
    CHECK_EQ(response.value("success", false), true);
    CHECK_EQ(response.value("session_token", "?"), "");
}

void test_the_lobby_and_a_game_between_two_browsers()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser ada(gateway.port());
    Browser bob(gateway.port());
    CHECK(ada.introduce("Ada"));
    CHECK(bob.introduce("Bob"));
    CHECK(ada.games().empty());

    // Ada opens a room: she is in it without asking
    ada.send(createRequest("Duel"));
    const json created = ada.expect(0x21);
    CHECK_EQ(created.value("success", false), true);
    const json joined = ada.expect(0x25);
    CHECK_EQ(joined.value("success", false), true);
    CHECK_EQ(joined["assigned_player_id"].get<int>(), 1);
    const json waiting = ada.expect(0x12);
    CHECK_EQ(waiting["status"].get<int>(), 0); // not started
    CHECK_EQ(waiting["players"].size(), size_t(1));
    CHECK_EQ(waiting["player_names"][0].get<std::string>(), "Ada");

    // Bob finds it in the lobby
    const std::vector<json> games = bob.games();
    CHECK_EQ(games.size(), size_t(1));
    CHECK_EQ(games[0]["game_name"].get<std::string>(), "Duel");
    CHECK_EQ(games[0]["current_players"].get<int>(), 1);

    bob.send({{"type", 0x24}, {"payload", {{"game_id", games[0]["game_id"]}}}});
    const json bob_joined = bob.expect(0x25);
    CHECK_EQ(bob_joined.value("success", false), true);
    CHECK_EQ(bob_joined["assigned_player_id"].get<int>(), 2);

    // The game starts for both
    const json ada_state = ada.expect(0x12);
    const json bob_state = bob.expect(0x12);
    CHECK_EQ(ada_state["status"].get<int>(), 1);
    CHECK_EQ(bob_state["status"].get<int>(), 1);
    CHECK_EQ(ada_state["rows"].get<int>(), 6);
    CHECK_EQ(ada_state["cols"].get<int>(), 7);

    // Whoever is to move plays the first column; the other is told it is not his turn
    Browser &first = Browser::currentPlayer(ada_state) == 1 ? ada : bob;
    Browser &second = Browser::currentPlayer(ada_state) == 1 ? bob : ada;

    second.send({{"type", 0x10}, {"payload", {{"column", 3}}}});
    CHECK_EQ(errorCode(second.expect(0xFF)), Protocol::ErrorCode::NOT_YOUR_TURN);

    first.send({{"type", 0x10}, {"payload", {{"column", 3}}}});
    CHECK_EQ(first.expect(0x11).value("success", false), true);

    const json after = second.expect(0x12);
    CHECK_EQ(after["board"][5][3].get<int>(), Browser::currentPlayer(ada_state)); // the disc fell to the bottom
    CHECK(Browser::currentPlayer(after) != Browser::currentPlayer(ada_state));
}

void test_leaving_closes_the_room()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser watcher(gateway.port());
    CHECK(watcher.introduce("Watcher"));

    {
        Browser ada(gateway.port());
        CHECK(ada.introduce("Ada"));
        ada.send(createRequest("Gone soon"));
        CHECK(ada.expect(0x25).is_object());
        CHECK_EQ(watcher.games().size(), size_t(1));
    }

    // Ada has closed her page: the server forgets her room
    std::vector<json> games;
    for (int i = 0; i < 40; ++i)
    {
        games = watcher.games();
        if (games.empty())
            break;
        std::this_thread::sleep_for(50ms);
    }
    CHECK(games.empty());
}

void test_one_game_each_and_no_nonsense()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser browser(gateway.port());
    CHECK(browser.introduce("Ada"));

    browser.send(createRequest("First"));
    CHECK(browser.expect(0x25).is_object());

    // A second game on the same connection
    browser.send(createRequest("Second"));
    CHECK_EQ(errorCode(browser.expect(0xFF)), gateway::ErrorCode::ALREADY_IN_GAME);

    // Messages that are not what they should be
    browser.sendText("this is not json");
    CHECK_EQ(errorCode(browser.expect(0xFF)), Protocol::ErrorCode::INVALID_MESSAGE);

    browser.send({{"type", 0x12}, {"payload", json::object()}}); // only the server sends these
    CHECK_EQ(errorCode(browser.expect(0xFF)), Protocol::ErrorCode::INVALID_MESSAGE);

    browser.send(createRequest("Huge", 99, 99));
    CHECK_EQ(errorCode(browser.expect(0xFF)), gateway::ErrorCode::ALREADY_IN_GAME); // (it is the first thing checked)

    // ...and the connection is still good
    CHECK(browser.games().size() == 1);

    // Heartbeats are answered by the gateway itself
    browser.send({{"type", 0x04}, {"payload", {{"timestamp", 99}}}});
    CHECK_EQ(browser.expect(0x04)["timestamp"].get<int>(), 99);
}

void test_bad_games_are_refused_before_the_server_sees_them()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser browser(gateway.port());
    CHECK(browser.introduce("Ada"));

    browser.send(createRequest("Too big", 99, 99));
    CHECK_EQ(errorCode(browser.expect(0xFF)), Protocol::ErrorCode::INVALID_MESSAGE);

    browser.send(createRequest("Silly", 6, 7, 2, 1));
    CHECK_EQ(errorCode(browser.expect(0xFF)), Protocol::ErrorCode::INVALID_MESSAGE);

    // Nothing was created, so a good one is fine
    browser.send(createRequest("Fine"));
    CHECK(browser.expect(0x21).value("success", false));
}

void test_a_game_name_cannot_be_taken_twice()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser ada(gateway.port());
    Browser bob(gateway.port());
    CHECK(ada.introduce("Ada"));
    CHECK(bob.introduce("Bob"));

    ada.send(createRequest("Same"));
    CHECK(ada.expect(0x25).is_object());

    // The server refuses, and the gatekeeper lets Bob try again with another name
    bob.send(createRequest("Same"));
    CHECK(errorCode(bob.expect(0xFF)) == Protocol::ErrorCode::INVALID_MESSAGE);

    bob.send(createRequest("Other"));
    CHECK(bob.expect(0x25).is_object());
}

// ============================================================================
// Looking after the connection
// ============================================================================

void test_pings()
{
    TestServer server;
    gateway::Settings settings = settingsFor(server);
    settings.ping_interval = 100ms;
    TestGateway gateway(settings);

    // A browser that answers stays
    Browser polite(gateway.port());
    CHECK(polite.introduce("Polite"));
    std::this_thread::sleep_for(500ms);
    CHECK(polite.games().empty());

    // Our own ping is answered with the same payload (a browser does not check, but a proxy might)
    Browser browser(gateway.port(), ORIGIN, "/connectx/ws", "", false);
    browser.sendFrame(websocket::Opcode::PING, "hello");
    const Incoming first = browser.next(2s);
    CHECK(first.kind == Incoming::Kind::PING); // the gateway's own, since a deaf browser reports them
}

void test_a_silent_browser_is_dropped()
{
    TestServer server;
    gateway::Settings settings = settingsFor(server);
    settings.ping_interval = 100ms;
    settings.silence_limit = 500ms;
    TestGateway gateway(settings);

    // This one never answers a ping
    Browser deaf(gateway.port(), ORIGIN, "/connectx/ws", "", false);
    CHECK(deaf.ok());

    Incoming incoming;
    do
    {
        incoming = deaf.next(2s);
    } while (incoming.kind == Incoming::Kind::PING);

    CHECK(incoming.kind == Incoming::Kind::CLOSE);
    CHECK_EQ(incoming.code, websocket::CloseCode::GOING_AWAY);
}

void test_abuse_closes_the_connection()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    // A message that is too long. (The gateway hangs up as soon as it sees the length, while the message is still
    // coming: it must not reset the connection, or the Close frame could be lost on the way.)
    Browser big(gateway.port());
    CHECK(big.ok());
    big.sendText(std::string(10000, 'x'));
    const Incoming too_big = big.next(2s);
    CHECK_MSG(too_big.kind == Incoming::Kind::CLOSE, "kind " << int(too_big.kind));
    CHECK_EQ(too_big.code, websocket::CloseCode::MESSAGE_TOO_BIG);

    // Binary data
    Browser binary(gateway.port());
    CHECK(binary.ok());
    binary.sendFrame(websocket::Opcode::BINARY, "\x01\x02");
    const Incoming rejected = binary.next(2s);
    CHECK(rejected.kind == Incoming::Kind::CLOSE);
    CHECK_EQ(rejected.code, websocket::CloseCode::UNSUPPORTED_DATA);

    // A flood
    Browser flood(gateway.port());
    CHECK(flood.ok());
    for (int i = 0; i < 200; ++i)
        flood.send({{"type", 0x04}, {"payload", {{"timestamp", i}}}});

    // The gateway hangs up on it, after the answers to the messages it let through
    Incoming incoming;
    do
    {
        incoming = flood.next(2s);
    } while (incoming.kind == Incoming::Kind::MESSAGE);

    CHECK_MSG(incoming.kind == Incoming::Kind::CLOSE, "kind " << int(incoming.kind));
    CHECK_EQ(incoming.code, websocket::CloseCode::POLICY_VIOLATION);
}

// ============================================================================
// Limits
// ============================================================================

void test_connection_limits()
{
    TestServer server;
    gateway::Settings settings = settingsFor(server);
    settings.max_connections_per_address = 2;
    settings.max_connections = 3;
    TestGateway gateway(settings);

    {
        Browser one(gateway.port());
        Browser two(gateway.port());
        CHECK(one.ok());
        CHECK(two.ok());

        // The same address a third time
        CHECK_EQ(Browser(gateway.port()).status(), 429);

        // Another address (as a reverse proxy would tell it) is fine, until the gateway is full
        Browser other(gateway.port(), ORIGIN, "/connectx/ws", "X-Forwarded-For: 203.0.113.9\r\n");
        CHECK(other.ok());
        CHECK_EQ(Browser(gateway.port(), ORIGIN, "/connectx/ws", "X-Forwarded-For: 203.0.113.10\r\n").status(), 503);
    }

    // Everybody has left: there is room again
    for (int i = 0; i < 40 && gateway.connections() > 0; ++i)
        std::this_thread::sleep_for(50ms);
    CHECK(Browser(gateway.port()).ok());
}

void test_room_limit()
{
    TestServer server;
    gateway::Settings settings = settingsFor(server);
    settings.max_rooms = 1;
    TestGateway gateway(settings);

    Browser ada(gateway.port());
    Browser bob(gateway.port());
    CHECK(ada.introduce("Ada"));
    CHECK(bob.introduce("Bob"));

    ada.send(createRequest("One"));
    CHECK(ada.expect(0x25).is_object());

    bob.send(createRequest("Two"));
    CHECK_EQ(errorCode(bob.expect(0xFF)), gateway::ErrorCode::TOO_MANY_ROOMS);
}

void test_the_game_server_is_down()
{
    gateway::Settings settings;
    settings.server_host = "127.0.0.1";
    settings.server_port = 38399; // nobody listens here
    settings.allowed_origins = {ORIGIN};
    TestGateway gateway(settings);

    Browser browser(gateway.port());
    CHECK(browser.ok());

    CHECK_EQ(errorCode(browser.expect(0xFF)), Protocol::ErrorCode::CONNECTION_REFUSED);
    const Incoming closed = browser.next(2s);
    CHECK(closed.kind == Incoming::Kind::CLOSE);
    CHECK_EQ(closed.code, websocket::CloseCode::TRY_AGAIN_LATER);
}

// ============================================================================
// The computer
// ============================================================================

void test_computer_levels()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser browser(gateway.port());
    CHECK(browser.ok());

    browser.send({{"type", gateway::WebMessage::REQ_COMPUTER_LEVELS}, {"payload", json::object()}});
    const json levels = browser.expect(gateway::WebMessage::RES_COMPUTER_LEVELS);
    CHECK(levels.is_object());

#ifdef CONNECTX_GATEWAY_COMPUTER
    CHECK_EQ(levels["available"].get<bool>(), true);
    CHECK_EQ(levels["levels"].size(), size_t(5));
    CHECK_EQ(levels["levels"][0]["level"].get<int>(), 1);
    CHECK(!levels["levels"][0]["name"].get<std::string>().empty());
    CHECK(!levels["levels"][4]["summary"].get<std::string>().empty());
#else
    CHECK_EQ(levels["available"].get<bool>(), false);
#endif
}

#ifdef CONNECTX_GATEWAY_COMPUTER

void test_playing_against_the_computer()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser browser(gateway.port());
    CHECK(browser.introduce("Ada"));

    browser.send(createRequest("Solo"));
    CHECK(browser.expect(0x25).is_object());

    browser.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 1}}}});
    const json added = browser.expect(gateway::WebMessage::RES_ADD_COMPUTER);
    CHECK_EQ(added.value("success", false), true);
    CHECK_EQ(added.value("name", ""), "Computer (Beginner)");

    // Only one computer per game
    browser.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 1}}}});
    CHECK_EQ(errorCode(browser.expect(0xFF)), gateway::ErrorCode::ALREADY_IN_GAME);

    // The computer joins and the game starts
    json state;
    for (int i = 0; i < 10; ++i)
    {
        state = browser.expect(0x12, 3s);
        if (state.is_object() && state["status"].get<int>() == 1)
            break;
    }
    CHECK(state.is_object());
    CHECK_EQ(state["status"].get<int>(), 1);
    CHECK_EQ(state["players"].size(), size_t(2));

    // Play three moves, waiting for the computer's answer every time
    int moves = 0;
    for (int guard = 0; guard < 20 && moves < 3 && state.is_object() && state["status"].get<int>() == 1; ++guard)
    {
        if (Browser::currentPlayer(state) == 1)
        {
            browser.send({{"type", 0x10}, {"payload", {{"column", 2 + moves}}}});
            CHECK(browser.expect(0x11).value("success", false));
            ++moves;
        }

        state = browser.expect(0x12, 5s);
    }

    CHECK(moves == 3);
    CHECK(state.is_object());

    // The computer answered: there are more discs than the ones we dropped
    int discs = 0;
    if (state.is_object())
        for (const json &row : state["board"])
            for (const json &cell : row)
                discs += cell.get<int>() != 0 ? 1 : 0;
    CHECK(discs >= 4);
}

void test_the_computer_refuses_what_it_cannot_play()
{
    TestServer server;
    TestGateway gateway(settingsFor(server));

    Browser browser(gateway.port());
    CHECK(browser.introduce("Ada"));

    // Three players: the computer plays two-player games only
    browser.send(createRequest("Trio", 6, 7, 3, 4));
    CHECK(browser.expect(0x25).is_object());

    browser.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 2}}}});
    const json refused = browser.expect(gateway::WebMessage::RES_ADD_COMPUTER);
    CHECK_EQ(refused.value("success", true), false);
    CHECK(refused.value("message", "").find("two-player") != std::string::npos);

    // The same for a board that is too large for its engine
    Browser other(gateway.port());
    CHECK(other.introduce("Bob"));
    other.send(createRequest("Big", 10, 10, 2, 4));
    CHECK(other.expect(0x25).is_object());
    other.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 2}}}});
    CHECK_EQ(other.expect(gateway::WebMessage::RES_ADD_COMPUTER).value("success", true), false);

    // And a level that does not exist
    Browser third(gateway.port());
    CHECK(third.introduce("Cleo"));
    third.send(createRequest("Level"));
    CHECK(third.expect(0x25).is_object());
    third.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 99}}}});
    CHECK_EQ(third.expect(gateway::WebMessage::RES_ADD_COMPUTER).value("success", true), false);
}

void test_computers_are_limited()
{
    TestServer server;
    gateway::Settings settings = settingsFor(server);
    settings.max_computers = 1;
    TestGateway gateway(settings);

    Browser ada(gateway.port());
    Browser bob(gateway.port());
    CHECK(ada.introduce("Ada"));
    CHECK(bob.introduce("Bob"));

    ada.send(createRequest("A"));
    CHECK(ada.expect(0x25).is_object());
    bob.send(createRequest("B"));
    CHECK(bob.expect(0x25).is_object());

    ada.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 1}}}});
    CHECK_EQ(ada.expect(gateway::WebMessage::RES_ADD_COMPUTER).value("success", false), true);

    bob.send({{"type", gateway::WebMessage::REQ_ADD_COMPUTER}, {"payload", {{"level", 1}}}});
    const json busy = bob.expect(gateway::WebMessage::RES_ADD_COMPUTER);
    CHECK_EQ(busy.value("success", true), false);
    CHECK(busy.value("message", "").find("busy") != std::string::npos);
}

#endif

int main(int argc, char *argv[])
{
#ifndef _WIN32
    // The game server of the tests lives in this process: like the real one, it must not die for a closed connection
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // `test_gateway <name>` runs a single test
    const std::string only = argc > 1 ? argv[1] : "";
#define RUN(test)                          do                                     {                                          if (only.empty() || only == #test)             RUN_TEST(test);                } while (0)

    RUN(test_who_may_connect);
    RUN(test_any_page_may_connect_when_none_is_listed);
    RUN(test_connecting_hides_the_token);
    RUN(test_the_lobby_and_a_game_between_two_browsers);
    RUN(test_leaving_closes_the_room);
    RUN(test_one_game_each_and_no_nonsense);
    RUN(test_bad_games_are_refused_before_the_server_sees_them);
    RUN(test_a_game_name_cannot_be_taken_twice);
    RUN(test_pings);
    RUN(test_a_silent_browser_is_dropped);
    RUN(test_abuse_closes_the_connection);
    RUN(test_connection_limits);
    RUN(test_room_limit);
    RUN(test_the_game_server_is_down);
    RUN(test_computer_levels);
#ifdef CONNECTX_GATEWAY_COMPUTER
    RUN(test_playing_against_the_computer);
    RUN(test_the_computer_refuses_what_it_cannot_play);
    RUN(test_computers_are_limited);
#endif
    return testSummary("test_gateway");
}
