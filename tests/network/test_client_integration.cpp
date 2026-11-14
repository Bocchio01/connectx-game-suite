// Integration tests of the Client against a real GameServer running inside the test process.
// (They used to need a server started by hand on localhost:8080: now they are self-contained.)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "client/client.hpp"
#include "server/game_server.hpp"

// Checks stay active in Release builds too (assert() would vanish with NDEBUG)
static int g_failures = 0;
#define CHECK(condition)                                                                       \
    do                                                                                         \
    {                                                                                          \
        if (!(condition))                                                                      \
        {                                                                                      \
            ++g_failures;                                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition << "\n"; \
        }                                                                                      \
    } while (0)

using namespace std::chrono_literals;

namespace
{
    /** A real server on a free port, running in its own thread. */
    class TestServer
    {
    public:
        explicit TestServer(const std::string &bind_address = "")
        {
            for (uint16_t port = 38080; port < 38180 && !started_; ++port)
            {
                server_ = std::make_unique<GameServer>(port, bind_address);
                finished_ = false;
                thread_ = std::thread([this]()
                                      {
                    server_->start(); // blocks until stop(); returns at once if the port is taken
                    finished_ = true; });

                // Wait until it is listening, or until start() gave up
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!server_->isRunning() && !finished_ && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(5ms);

                if (server_->isRunning())
                {
                    port_ = port;
                    started_ = true;
                }
                else
                {
                    if (thread_.joinable())
                        thread_.join();
                }
            }
        }

        ~TestServer() { stop(); }

        bool ok() const { return started_; }
        uint16_t port() const { return port_; }
        size_t connections() { return server_->connectionCount(); }

        void stop()
        {
            if (!started_)
                return;
            started_ = false;
            server_->stop();

            // Closing the listening socket makes accept() fail at once on Windows but not everywhere: if
            // the server thread has not left after a moment, connect once to wake it up
            // (a connection to a closed port takes ~2 s to be refused on Windows, hence the wait first)
            const auto deadline = std::chrono::steady_clock::now() + 300ms;
            while (!finished_ && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            if (!finished_)
            {
                Client waker;
                waker.connect("127.0.0.1", port_);
                waker.disconnect();
            }

            if (thread_.joinable())
                thread_.join();
        }

    private:
        std::unique_ptr<GameServer> server_;
        std::thread thread_;
        std::atomic<bool> finished_{false};
        uint16_t port_ = 0;
        bool started_ = false;
    };

    /** A client with its event loop in a thread, remembering what it receives. */
    class TestClient
    {
    public:
        TestClient()
        {
            client.onGameStateUpdate([this](const GameStateUpdate &state)
                                     {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    last_state = state;
                    ++states;
                }
                changed.notify_all(); });
            client.onMoveResult([this](bool success, const std::string &)
                                {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    success ? ++moves_accepted : ++moves_rejected;
                }
                changed.notify_all(); });
            client.onError([this](uint16_t code, const std::string &)
                           {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    errors.push_back(code);
                }
                changed.notify_all(); });
            client.onDisconnected([this]()
                                  { ++disconnected_calls; changed.notify_all(); });
        }

        ~TestClient()
        {
            client.stop();
            if (loop.joinable())
                loop.join();
        }

        bool start(uint16_t port, const std::string &name)
        {
            if (!client.connect("127.0.0.1", port))
                return false;
            loop = std::thread([this]()
                               { client.run(); });
            auto response = client.requestConnect(name);
            return response && response->success;
        }

        template <typename Predicate>
        bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5000ms)
        {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, timeout, [&]
                                    { return predicate(); });
        }

        Client client;
        std::thread loop;
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<GameStateUpdate> last_state;
        int states = 0;
        int moves_accepted = 0;
        int moves_rejected = 0;
        std::vector<uint16_t> errors;
        std::atomic<int> disconnected_calls{0};
    };
}

void test_invalid_connection()
{
    std::cout << "Testing connection to a port where nobody listens..." << std::flush;

    Client client;
    CHECK(!client.connect("127.0.0.1", 1)); // (reserved port: refused)
    CHECK(!client.isConnected());

    std::cout << " done" << std::endl;
}

void test_full_game(TestServer &server)
{
    std::cout << "Testing a whole game between two clients..." << std::flush;

    TestClient a, b;
    CHECK(a.start(server.port(), "Alice"));
    CHECK(b.start(server.port(), "Bob"));
    CHECK(!a.client.getSessionToken().empty());
    CHECK(a.client.getSessionToken() != b.client.getSessionToken());

    // Alice creates a game, both join it
    auto created = a.client.requestCreateGame(GameConfig(6, 7, 2, 4), "integration");
    CHECK(created && created->success);
    const uint32_t game_id = created ? created->game_info.game_id : 0;

    auto joined_a = a.client.requestJoinGame(game_id);
    CHECK(joined_a && joined_a->success);
    auto joined_b = b.client.requestJoinGame(game_id);
    CHECK(joined_b && joined_b->success);
    if (!joined_a || !joined_b)
        return;

    CHECK(joined_a->assigned_player_id != joined_b->assigned_player_id);
    // The state published before the join response is processed by the caller must already be stored
    CHECK(a.client.getPlayerId() == joined_a->assigned_player_id);
    CHECK(b.client.getPlayerId() == joined_b->assigned_player_id);
    CHECK(a.client.getGameId() == game_id);

    // The game starts when the second player has joined
    CHECK(a.waitFor([&]
                    { return a.last_state && a.last_state->status == ProtocolGameStatus::IN_PROGRESS; }));
    CHECK(b.waitFor([&]
                    { return b.last_state && b.last_state->status == ProtocolGameStatus::IN_PROGRESS; }));

    // A move out of turn is refused with an error
    TestClient &waiting = a.last_state->current_player == a.client.getPlayerId() ? b : a;
    waiting.client.sendMove(0);
    CHECK(waiting.waitFor([&]
                          { return !waiting.errors.empty(); }));
    CHECK(waiting.errors.empty() || waiting.errors[0] == Protocol::ErrorCode::NOT_YOUR_TURN);

    // Whoever starts stacks four stones in column 0 and wins; the other one plays column 1
    auto finished = [](TestClient &c)
    {
        return c.last_state && (c.last_state->status == ProtocolGameStatus::FINISHED_WIN || c.last_state->status == ProtocolGameStatus::FINISHED_DRAW);
    };

    int guard = 0;
    while (!(finished(a) && finished(b)) && guard++ < 40)
    {
        for (TestClient *player : {&a, &b})
        {
            GameStateUpdate state;
            {
                std::lock_guard<std::mutex> lock(player->mutex);
                state = *player->last_state;
            }
            if (state.status == ProtocolGameStatus::IN_PROGRESS && state.current_player == player->client.getPlayerId())
            {
                const int before = player->states;
                const uint8_t column = (state.board[5][0] == 0 || state.board[5][0] == player->client.getPlayerId()) ? 0 : 1;
                player->client.sendMove(player->client.getPlayerId() == a.client.getPlayerId() ? column : column);
                player->waitFor([&]
                                { return player->states > before; }, 3000ms);
            }
        }
        std::this_thread::sleep_for(2ms);
    }

    CHECK(a.waitFor([&]
                    { return finished(a); }));
    CHECK(b.waitFor([&]
                    { return finished(b); }));
    CHECK(a.last_state->status == ProtocolGameStatus::FINISHED_WIN);
    CHECK(a.last_state->winner.has_value());
    CHECK(b.last_state->winner == a.last_state->winner);
    CHECK(a.moves_accepted + b.moves_accepted >= 7);

    std::cout << " done" << std::endl;
}

void test_stop_from_another_thread(TestServer &server)
{
    std::cout << "Testing stop() from another thread while the event loop is blocked reading..." << std::flush;

    TestClient c;
    CHECK(c.start(server.port(), "Stopper"));
    std::this_thread::sleep_for(150ms); // the event loop is now waiting for data that never comes

    const auto start = std::chrono::steady_clock::now();
    c.client.stop();
    if (c.loop.joinable())
        c.loop.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < 1500ms);
    CHECK(!c.client.isConnected());
    CHECK(c.disconnected_calls == 1);

    c.client.stop(); // safe to call again
    CHECK(c.disconnected_calls == 1);

    std::cout << " done (" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms)" << std::endl;
}

void test_concurrent_disconnects(TestServer &server)
{
    std::cout << "Testing concurrent disconnect() calls..." << std::flush;

    TestClient c;
    CHECK(c.start(server.port(), "Racer"));

    std::thread t1([&]
                   { c.client.disconnect(); });
    std::thread t2([&]
                   { c.client.disconnect(); });
    t1.join();
    t2.join();
    if (c.loop.joinable())
        c.loop.join();

    CHECK(c.disconnected_calls == 1);
    CHECK(!c.client.isConnected());

    std::cout << " done" << std::endl;
}

void test_reconnect_with_the_same_object(TestServer &server)
{
    std::cout << "Testing reconnection with the same Client object..." << std::flush;

    TestClient c;
    for (int round = 0; round < 3; ++round)
    {
        CHECK(c.start(server.port(), "Again"));
        CHECK(c.client.isConnected());
        c.client.stop();
        if (c.loop.joinable())
            c.loop.join();
        CHECK(!c.client.isConnected());
    }
    CHECK(c.disconnected_calls == 3);

    std::cout << " done" << std::endl;
}

void test_server_forgets_connections(TestServer &server)
{
    std::cout << "Testing that the server forgets the connections that ended..." << std::flush;

    // The clients of the previous tests may still be on their way out: wait until the number settles
    size_t before = server.connections();
    for (auto stable_since = std::chrono::steady_clock::now(), deadline = stable_since + 3s;
         std::chrono::steady_clock::now() - stable_since < 300ms && std::chrono::steady_clock::now() < deadline;)
    {
        std::this_thread::sleep_for(20ms);
        if (server.connections() != before)
        {
            before = server.connections();
            stable_since = std::chrono::steady_clock::now();
        }
    }

    // Clients that connect and leave without a word, like a port probe: no session, but a connection
    for (int i = 0; i < 40; ++i)
    {
        Client probe;
        CHECK(probe.connect("127.0.0.1", server.port()));
        probe.disconnect();
    }

    // Clients that introduce themselves and then say goodbye (the server handles the end of those twice:
    // the DISCONNECT message, then the closing of the socket)
    for (int i = 0; i < 10; ++i)
    {
        TestClient c;
        CHECK(c.start(server.port(), "Leaver"));
        c.client.sendDisconnect();
        c.client.stop();
        if (c.loop.joinable())
            c.loop.join();
    }

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (server.connections() != before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    CHECK(server.connections() == before);

    // ...and the ids that were given back are not given twice: simultaneous clients keep working
    TestClient a, b, c;
    CHECK(a.start(server.port(), "A"));
    CHECK(b.start(server.port(), "B"));
    CHECK(c.start(server.port(), "C"));
    CHECK(server.connections() == before + 3);

    std::cout << " done" << std::endl;
}

void test_connect_timeouts(TestServer &server)
{
    std::cout << "Testing the connect timeout..." << std::flush;

    // A reachable server is joined whatever the timeout (0 = the blocking connect of the operating system)
    for (const int timeout_ms : {0, 50, 10000})
    {
        Client client;
        CHECK(client.connect("127.0.0.1", server.port(), timeout_ms));
        CHECK(client.isConnected());
        client.disconnect();
    }

    // A refused connection gives up within the timeout (Windows needs ~2 s to report a refusal itself)
    Client refused;
    const auto start = std::chrono::steady_clock::now();
    CHECK(!refused.connect("127.0.0.1", 1, 300));
    CHECK(std::chrono::steady_clock::now() - start < 2000ms);
    CHECK(!refused.isConnected());

    std::cout << " done" << std::endl;
}

void test_many_immediate_requests(TestServer &server)
{
    std::cout << "Testing thousands of requests whose response comes back at once..." << std::flush;

    // The response of a request can reach the client before the thread that sent it has started
    // waiting for it: it must not be lost (it used to be, now and then, and the request then failed
    // after its timeout). The server answers in microseconds on the loopback interface.
    TestClient c;
    CHECK(c.start(server.port(), "Hammer"));

    const int count = 4000;
    int failed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i)
    {
        if (!c.client.requestGamesList(2000))
            ++failed;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(failed == 0);
    std::cout << " done (" << count << " requests in " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms, "
              << failed << " lost)" << std::endl;
}

void test_concurrent_requests(TestServer &server)
{
    std::cout << "Testing requests made by several threads at the same time..." << std::flush;

    // The responses do not say which request they answer, so the client lets one request at a time
    // through: every thread must get a valid response, not somebody else's, nor a timeout
    TestClient c;
    CHECK(c.start(server.port(), "Crowd"));

    std::atomic<int> failed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&, t]
                             {
            for (int i = 0; i < 250; ++i)
            {
                if (t % 2 == 0)
                {
                    if (!c.client.requestGamesList(2000))
                        ++failed;
                }
                else
                {
                    // refused by the server (no such game), and quickly
                    const auto response = c.client.requestJoinGame(4000000 + t, 2000);
                    if (!response || response->success)
                        ++failed;
                }
            } });
    }
    for (std::thread &thread : threads)
        thread.join();

    CHECK(failed == 0);
    std::cout << " done" << std::endl;
}

void test_request_refused_by_the_server(TestServer &server)
{
    std::cout << "Testing that a request the server refuses fails at once, with the reason..." << std::flush;

    TestClient c;
    CHECK(c.start(server.port(), "Refused"));

    // The server answers a join request for a game that does not exist with an ERROR message, not with
    // a response: the request must not wait for its (long) timeout to find out
    auto start = std::chrono::steady_clock::now();
    const auto joined = c.client.requestJoinGame(4000000, 5000);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(joined.has_value());
    CHECK(joined && !joined->success);
    CHECK(joined && !joined->message.empty());
    CHECK(elapsed < 2000ms);
    {
        // ... and the error callback has been told before the request returned
        std::lock_guard<std::mutex> lock(c.mutex);
        CHECK(!c.errors.empty() && c.errors.back() == Protocol::ErrorCode::GAME_NOT_FOUND);
    }

    // The same for a game name that is taken
    const auto first = c.client.requestCreateGame(GameConfig(6, 7, 2, 4), "taken");
    CHECK(first && first->success);
    start = std::chrono::steady_clock::now();
    const auto second = c.client.requestCreateGame(GameConfig(6, 7, 2, 4), "taken", 5000);
    elapsed = std::chrono::steady_clock::now() - start;
    CHECK(second.has_value());
    CHECK(second && !second->success);
    CHECK(second && !second->message.empty());
    CHECK(elapsed < 2000ms);

    // A refusal does not leave the client in a strange state
    CHECK(c.client.requestGamesList().has_value());
    const auto again = c.client.requestJoinGame(first ? first->game_info.game_id : 0);
    CHECK(again && again->success);

    std::cout << " done" << std::endl;
}

void test_request_released_by_disconnect(TestServer &server)
{
    std::cout << "Testing that disconnect() releases a thread waiting for a response..." << std::flush;

    // Nobody runs the event loop here, so nobody reads the answer: the request would wait for its timeout
    Client client;
    CHECK(client.connect("127.0.0.1", server.port()));

    std::atomic<bool> returned{false};
    std::optional<ConnectResponse> result = ConnectResponse();
    std::chrono::milliseconds elapsed{0};
    std::thread requester([&]
                          {
        const auto start = std::chrono::steady_clock::now();
        result = client.requestConnect("Abandoned", 10000);
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        returned = true; });

    std::this_thread::sleep_for(200ms);
    CHECK(!returned);
    client.disconnect();
    requester.join();

    CHECK(returned);
    CHECK(!result.has_value());
    CHECK(elapsed < 3000ms);

    // After the disconnection a request fails at once instead of waiting
    const auto start = std::chrono::steady_clock::now();
    CHECK(!client.requestGamesList(10000).has_value());
    CHECK(std::chrono::steady_clock::now() - start < 1000ms);

    std::cout << " done (" << elapsed.count() << " ms)" << std::endl;
}

void test_request_from_a_callback_is_refused(TestServer &server)
{
    std::cout << "Testing that a request made from a callback is refused instead of blocking the event loop..." << std::flush;

    Client client;
    std::atomic<bool> done{false};
    std::atomic<bool> refused{false};
    std::atomic<long long> nested_ms{-1};

    // The server sends an ERROR for the bogus move below: the callback runs on the event thread, which
    // is the thread that would have to receive the answer of the request made inside it
    client.onError([&](uint16_t, const std::string &)
                   {
        const auto start = std::chrono::steady_clock::now();
        refused = !client.requestGamesList(5000).has_value();
        nested_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        done = true; });

    CHECK(client.connect("127.0.0.1", server.port()));
    std::thread loop([&]
                     { client.run(); });
    const auto connected = client.requestConnect("Nested");
    CHECK(connected && connected->success);

    client.sendJoinGame(4000000); // refused with an ERROR message

    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    CHECK(done);
    CHECK(refused);
    CHECK(nested_ms >= 0 && nested_ms < 1000);

    // The event loop is still alive: an ordinary request works
    CHECK(client.requestGamesList().has_value());

    client.stop();
    loop.join();

    std::cout << " done" << std::endl;
}

void test_server_bound_to_one_address()
{
    std::cout << "Testing a server that listens on one address only..." << std::flush;

    // Loopback only: this is what a private server for a single user needs, it is not reachable from the network
    {
        TestServer local("127.0.0.1");
        CHECK(local.ok());
        TestClient c;
        CHECK(c.start(local.port(), "Local"));
    }

    // An address that is not one of this machine's is refused with an error, not with a crash
    {
        GameServer nowhere(38179, "203.0.113.77"); // (TEST-NET-3: never assigned to a network interface)
        nowhere.start();                           // returns at once
        CHECK(!nowhere.isRunning());
    }

    std::cout << " done" << std::endl;
}

void test_server_going_away()
{
    std::cout << "Testing that a client notices the server going away..." << std::flush;

    TestServer server;
    CHECK(server.ok());

    TestClient c;
    CHECK(c.start(server.port(), "Orphan"));

    server.stop(); // closes every connection
    CHECK(c.waitFor([&]
                    { return c.disconnected_calls >= 1; }, 3000ms));
    if (c.loop.joinable())
        c.loop.join(); // the event loop must have ended by itself
    CHECK(!c.client.isConnected());
    CHECK(c.disconnected_calls == 1);

    std::cout << " done" << std::endl;
}

int main()
{
    std::cout << "\n=== Running Client integration tests (in-process server) ===" << std::endl;

    test_invalid_connection();

    {
        TestServer server;
        if (!server.ok())
        {
            std::cerr << "Could not start a test server: no free port in 38080-38179" << std::endl;
            return EXIT_FAILURE;
        }

        test_full_game(server);
        test_server_forgets_connections(server);
        test_connect_timeouts(server);
        test_many_immediate_requests(server);
        test_concurrent_requests(server);
        test_request_refused_by_the_server(server);
        test_request_released_by_disconnect(server);
        test_request_from_a_callback_is_refused(server);
        test_stop_from_another_thread(server);
        test_concurrent_disconnects(server);
        test_reconnect_with_the_same_object(server);
    }

    test_server_bound_to_one_address();
    test_server_going_away();

    if (g_failures == 0)
    {
        std::cout << "\n=== All Client integration tests passed! ===" << std::endl;
        return EXIT_SUCCESS;
    }

    std::cerr << "\n"
              << g_failures << " check(s) failed" << std::endl;
    return EXIT_FAILURE;
}
