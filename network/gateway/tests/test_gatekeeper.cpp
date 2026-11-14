// Tests of the doorman of a browser: what it lets through, what it refuses and what it keeps for itself

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "gateway/gatekeeper.hpp"
#include "test_support.hpp"

using json = nlohmann::json;
using namespace gateway;
using namespace std::chrono_literals;

namespace
{
    std::string message(int type, const json &payload = json::object())
    {
        return json{{"type", type}, {"payload", payload}}.dump();
    }

    std::string connectMessage(const std::string &name = "Ada")
    {
        return message(0x01, {{"player_name", name}, {"protocol_version", 1}});
    }

    json createPayload(const std::string &name = "Duel", int rows = 6, int cols = 7, int players = 2, int connect = 4)
    {
        return {{"name", name}, {"config", {{"rows", rows}, {"cols", cols}, {"num_players", players}, {"connect_length", connect}}}};
    }

    /** What the server answers to a connection request. */
    std::string connectResponse(const std::string &token = "secret-token-1234")
    {
        return message(0x02, {{"success", true}, {"session_token", token}, {"message", "Connected successfully"}});
    }

    std::string createResponse(int id = 7)
    {
        return message(0x21, {{"success", true},
                              {"message", "Game created successfully"},
                              {"game_info", {{"game_id", id}, {"name", "Duel"}, {"current_players", 0}, {"status", 0}, {"config", {{"rows", 6}, {"cols", 7}, {"num_players", 2}, {"connect_length", 4}}}}}});
    }

    std::string joinResponse(int id = 7, int player = 1)
    {
        return message(0x25, {{"success", true},
                              {"assigned_player_id", player},
                              {"message", "Joined game successfully"},
                              {"game_info", {{"game_id", id}, {"name", "Duel"}, {"current_players", 1}, {"status", 0}, {"config", {{"rows", 6}, {"cols", 7}, {"num_players", 2}, {"connect_length", 4}}}}}});
    }

    /** A gatekeeper whose visitor has connected and has been given a token. */
    void introduce(Gatekeeper &gatekeeper)
    {
        gatekeeper.inspect(connectMessage());
        gatekeeper.translate(connectResponse());
    }

    json payloadOf(const std::string &text)
    {
        return json::parse(text)["payload"];
    }

    int typeOf(const std::string &text)
    {
        return json::parse(text)["type"].get<int>();
    }
}

void test_names_are_cleaned_up()
{
    CHECK_EQ(sanitizeName("Ada"), "Ada");
    CHECK_EQ(sanitizeName("  Ada Lovelace  "), "Ada Lovelace");
    CHECK_EQ(sanitizeName("A\tb\nc\x01"), "Abc");
    CHECK_EQ(sanitizeName("   "), "");
    CHECK_EQ(sanitizeName(""), "");
    CHECK_EQ(sanitizeName(std::string(100, 'x')).size(), size_t(32));

    // Cut to length without leaving half of a character behind: "è" takes two bytes
    const std::string accents = "caff\xC3\xA8 caff\xC3\xA8 caff\xC3\xA8 caff\xC3\xA8 caff\xC3\xA8 caff\xC3\xA8";
    const std::string cut = sanitizeName(accents);
    CHECK(cut.size() <= 32);
    CHECK(cut.size() >= 31);
    CHECK(cut.compare(0, 6, "caff\xC3\xA8") == 0);
    CHECK(cut.back() != '\xC3');
}

void test_game_shapes_are_checked()
{
    CHECK_EQ(checkGameConfig(6, 7, 2, 4), "");
    CHECK_EQ(checkGameConfig(4, 4, 2, 3), "");
    CHECK_EQ(checkGameConfig(20, 20, 4, 20), "");
    CHECK_EQ(checkGameConfig(6, 7, 3, 4), "");

    CHECK(!checkGameConfig(3, 7, 2, 4).empty());   // too small
    CHECK(!checkGameConfig(6, 21, 2, 4).empty());  // too large
    CHECK(!checkGameConfig(6, 7, 1, 4).empty());   // one player is not a game
    CHECK(!checkGameConfig(6, 7, 5, 4).empty());   // there is room for four
    CHECK(!checkGameConfig(6, 7, 2, 2).empty());   // the engine refuses to connect fewer than 3
    CHECK(!checkGameConfig(6, 7, 2, 8).empty());   // it would never be won
}

void test_the_rate_limiter()
{
    const auto start = RateLimiter::Clock::now();
    RateLimiter limiter(5, 2, start); // a burst of 5, then 2 per second

    for (int i = 0; i < 5; ++i)
        CHECK(limiter.allow(start));
    CHECK(!limiter.allow(start));

    CHECK(!limiter.allow(start + 400ms)); // 0.8 of a message has accumulated
    CHECK(limiter.allow(start + 600ms));  // now there is one
    CHECK(!limiter.allow(start + 700ms));

    // A long pause fills the bucket again, but no more than its size
    const auto later = start + 1h;
    for (int i = 0; i < 5; ++i)
        CHECK(limiter.allow(later));
    CHECK(!limiter.allow(later));
}

void test_garbage_is_refused()
{
    Gatekeeper gatekeeper;

    for (const std::string &text : {std::string("not json"), std::string("[1,2,3]"), std::string("42"), std::string("{}"),
                                    message(1000), json{{"type", "one"}}.dump(), json{{"type", 1}, {"payload", 5}}.dump()})
    {
        const Verdict verdict = gatekeeper.inspect(text);
        CHECK(verdict.action == Verdict::Action::ANSWER);
        CHECK_EQ(typeOf(verdict.text), 255);
    }

    // A name that is not a text must not bring anything down
    const Verdict odd = gatekeeper.inspect(message(0x01, {{"player_name", 12}}));
    CHECK(odd.action == Verdict::Action::ANSWER);
    CHECK_EQ(typeOf(odd.text), 255);
}

void test_a_visitor_connects()
{
    Gatekeeper gatekeeper;

    const Verdict verdict = gatekeeper.inspect(connectMessage("  Ada  "));
    CHECK(verdict.action == Verdict::Action::FORWARD);
    CHECK_EQ(typeOf(verdict.text), 0x01);
    CHECK_EQ(payloadOf(verdict.text)["player_name"].get<std::string>(), "Ada");
    CHECK_EQ(payloadOf(verdict.text)["protocol_version"].get<int>(), 1);

    // Once is enough
    const Verdict again = gatekeeper.inspect(connectMessage());
    CHECK(again.action == Verdict::Action::ANSWER);
    CHECK_EQ(payloadOf(again.text)["error_code"].get<int>(), Protocol::ErrorCode::ALREADY_CONNECTED);

    // A visitor without a name is a guest
    Gatekeeper anonymous;
    CHECK_EQ(payloadOf(anonymous.inspect(message(0x01, {{"player_name", "  "}})).text)["player_name"].get<std::string>(), "Guest");
    Gatekeeper silent;
    CHECK_EQ(payloadOf(silent.inspect(message(0x01)).text)["player_name"].get<std::string>(), "Guest");
}

void test_the_token_never_reaches_the_browser()
{
    Gatekeeper gatekeeper;
    gatekeeper.inspect(connectMessage());

    const Translation translation = gatekeeper.translate(connectResponse("very-secret-token"));
    CHECK(translation.to_browser.find("very-secret-token") == std::string::npos);
    CHECK_EQ(payloadOf(translation.to_browser)["success"].get<bool>(), true);
    CHECK_EQ(payloadOf(translation.to_browser)["session_token"].get<std::string>(), "");
    CHECK(!translation.to_server.has_value());

    // ...but it is in the requests to the server, whatever the browser claims it is
    const Verdict list = gatekeeper.inspect(message(0x22, {{"session_token", "forged"}}));
    CHECK(list.action == Verdict::Action::FORWARD);
    CHECK_EQ(payloadOf(list.text)["session_token"].get<std::string>(), "very-secret-token");
    CHECK(list.text.find("forged") == std::string::npos);
}

void test_nothing_can_be_done_before_connecting()
{
    Gatekeeper gatekeeper;

    for (int type : {0x22, 0x20, 0x24, 0x10})
    {
        const Verdict verdict = gatekeeper.inspect(message(type, createPayload()));
        CHECK(verdict.action == Verdict::Action::ANSWER);
        CHECK_EQ(payloadOf(verdict.text)["error_code"].get<int>(), Protocol::ErrorCode::INVALID_SESSION_TOKEN);
    }

    // A connection request that has not been answered yet leaves the visitor without a token
    gatekeeper.inspect(connectMessage());
    CHECK(gatekeeper.inspect(message(0x22)).action == Verdict::Action::ANSWER);
}

void test_only_requests_get_through()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    // The messages that only the server sends, and those that do not exist
    for (int type : {0x02, 0x11, 0x12, 0x21, 0x23, 0x25, 0xFF, 0x00, 0x77})
    {
        const Verdict verdict = gatekeeper.inspect(message(type));
        CHECK_MSG(verdict.action == Verdict::Action::ANSWER, "type " << type);
        CHECK_EQ(payloadOf(verdict.text)["error_code"].get<int>(), Protocol::ErrorCode::INVALID_MESSAGE);
    }
}

void test_listing_games()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    const Verdict verdict = gatekeeper.inspect(message(0x22));
    CHECK(verdict.action == Verdict::Action::FORWARD);
    CHECK_EQ(typeOf(verdict.text), 0x22);
}

void test_creating_a_game_joins_it()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    const Verdict verdict = gatekeeper.inspect(message(0x20, createPayload("  Duel  ", 6, 7, 2, 4)));
    CHECK(verdict.action == Verdict::Action::FORWARD);

    const json request = payloadOf(verdict.text);
    CHECK_EQ(request["session_token"].get<std::string>(), "secret-token-1234");
    CHECK_EQ(request["name"].get<std::string>(), "Duel");
    CHECK_EQ(request["config"]["rows"].get<int>(), 6);
    CHECK_EQ(request["config"]["connect_length"].get<int>(), 4);

    // The server's answer comes with a request to take the creator's place, made by the gatekeeper itself:
    // a game that nobody ever joins is not forgotten by the server
    const Translation created = gatekeeper.translate(createResponse(7));
    CHECK(created.room_created);
    CHECK(created.to_server.has_value());
    CHECK_EQ(typeOf(*created.to_server), 0x24);
    CHECK_EQ(payloadOf(*created.to_server)["game_id"].get<int>(), 7);
    CHECK_EQ(payloadOf(*created.to_server)["session_token"].get<std::string>(), "secret-token-1234");
    CHECK(gatekeeper.roomCreated());
    CHECK(!gatekeeper.inGame());

    const Translation joined = gatekeeper.translate(joinResponse(7, 1));
    CHECK(!joined.to_server.has_value());
    CHECK(gatekeeper.inGame());
    CHECK(gatekeeper.game().has_value());
    CHECK_EQ(gatekeeper.game()->game_id, uint32_t(7));
    CHECK_EQ(int(gatekeeper.game()->config.cols), 7);
}

void test_bad_games_do_not_reach_the_server()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    const std::vector<std::string> bad = {
        message(0x20, createPayload("x", 3, 7, 2, 4)),
        message(0x20, createPayload("x", 6, 7, 9, 4)),
        message(0x20, createPayload("x", 6, 7, 2, 0)),
        message(0x20, createPayload("x", 6, 7, 2, 30)),
        message(0x20, {{"name", "x"}}),
        message(0x20, {{"name", "x"}, {"config", {{"rows", "six"}, {"cols", 7}, {"num_players", 2}, {"connect_length", 4}}}}),
        message(0x20, {{"name", "x"}, {"config", {{"rows", 6.5}, {"cols", 7}, {"num_players", 2}, {"connect_length", 4}}}}),
        message(0x20, {{"name", 17}, {"config", {{"rows", 6}, {"cols", 7}, {"num_players", 2}, {"connect_length", 4}}}}),
    };

    for (const std::string &text : bad)
    {
        const Verdict verdict = gatekeeper.inspect(text);
        CHECK_MSG(verdict.action == Verdict::Action::ANSWER, text);
    }

    // None of that was counted as a game: a good one still goes through
    CHECK(gatekeeper.inspect(message(0x20, createPayload())).action == Verdict::Action::FORWARD);
}

void test_one_game_per_connection()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    CHECK(gatekeeper.inspect(message(0x20, createPayload())).action == Verdict::Action::FORWARD);

    // While the first request is still being answered, and after it has been
    auto refused = [&](const Verdict &verdict)
    {
        return verdict.action == Verdict::Action::ANSWER &&
               payloadOf(verdict.text)["error_code"].get<int>() == ErrorCode::ALREADY_IN_GAME;
    };

    CHECK(refused(gatekeeper.inspect(message(0x20, createPayload("another")))));
    CHECK(refused(gatekeeper.inspect(message(0x24, {{"game_id", 3}}))));

    gatekeeper.translate(createResponse(7));
    gatekeeper.translate(joinResponse(7, 1));

    CHECK(refused(gatekeeper.inspect(message(0x20, createPayload("another")))));
    CHECK(refused(gatekeeper.inspect(message(0x24, {{"game_id", 3}}))));
}

void test_a_refused_request_can_be_made_again()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    CHECK(gatekeeper.inspect(message(0x24, {{"game_id", 3}})).action == Verdict::Action::FORWARD);

    // The server says no: "Game not found"
    gatekeeper.translate(message(0xFF, {{"error_code", 200}, {"error_message", "Game not found"}}));

    const Verdict retry = gatekeeper.inspect(message(0x24, {{"game_id", 4}}));
    CHECK(retry.action == Verdict::Action::FORWARD);
    CHECK_EQ(payloadOf(retry.text)["game_id"].get<int>(), 4);
}

void test_joining_a_game()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    // Any open game, or a new standard one, is game 0
    CHECK(gatekeeper.inspect(message(0x24, {{"game_id", 0}})).action == Verdict::Action::FORWARD);
    gatekeeper.translate(message(0xFF, {{"error_code", 201}, {"error_message", "Game is full"}}));

    for (const json &bad : {json("seven"), json(-1), json(1.5), json(5000000000LL), json()})
    {
        const Verdict verdict = gatekeeper.inspect(message(0x24, {{"game_id", bad}}));
        CHECK(verdict.action == Verdict::Action::ANSWER);
    }
}

void test_moves()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    // No game, no move
    CHECK(gatekeeper.inspect(message(0x10, {{"column", 3}})).action == Verdict::Action::ANSWER);

    gatekeeper.inspect(message(0x24, {{"game_id", 7}}));
    gatekeeper.translate(joinResponse(7, 2));

    const Verdict move = gatekeeper.inspect(message(0x10, {{"column", 3}, {"session_token", "forged"}}));
    CHECK(move.action == Verdict::Action::FORWARD);
    CHECK_EQ(payloadOf(move.text)["column"].get<int>(), 3);
    CHECK_EQ(payloadOf(move.text)["session_token"].get<std::string>(), "secret-token-1234");

    for (const json &bad : {json(-1), json(256), json("3"), json(2.5), json()})
        CHECK(gatekeeper.inspect(message(0x10, {{"column", bad}})).action == Verdict::Action::ANSWER);
}

void test_rooms_can_be_limited()
{
    bool open = true;
    Gatekeeper gatekeeper([&open]()
                          { return open; });
    introduce(gatekeeper);

    open = false;
    const Verdict verdict = gatekeeper.inspect(message(0x20, createPayload()));
    CHECK(verdict.action == Verdict::Action::ANSWER);
    CHECK_EQ(payloadOf(verdict.text)["error_code"].get<int>(), ErrorCode::TOO_MANY_ROOMS);

    // Not a reason to give up for good
    open = true;
    CHECK(gatekeeper.inspect(message(0x20, createPayload())).action == Verdict::Action::FORWARD);
}

void test_heartbeats_and_goodbyes_stay_in_the_gateway()
{
    Gatekeeper gatekeeper;

    const Verdict beat = gatekeeper.inspect(message(0x04, {{"timestamp", 1234}}));
    CHECK(beat.action == Verdict::Action::ANSWER);
    CHECK_EQ(typeOf(beat.text), 0x04);
    CHECK_EQ(payloadOf(beat.text)["timestamp"].get<int>(), 1234);

    CHECK(gatekeeper.inspect(message(0x03, {{"reason", "bye"}})).action == Verdict::Action::CLOSE);
}

void test_the_computer()
{
    Gatekeeper gatekeeper;

    // The levels need nothing at all
    const Verdict levels = gatekeeper.inspect(message(WebMessage::REQ_COMPUTER_LEVELS));
    CHECK(levels.action == Verdict::Action::LOCAL);
    CHECK_EQ(int(levels.type), int(WebMessage::REQ_COMPUTER_LEVELS));

    // A computer needs a game to join
    introduce(gatekeeper);
    CHECK(gatekeeper.inspect(message(WebMessage::REQ_ADD_COMPUTER, {{"level", 2}})).action == Verdict::Action::ANSWER);

    gatekeeper.inspect(message(0x24, {{"game_id", 7}}));
    gatekeeper.translate(joinResponse(7, 1));

    const Verdict add = gatekeeper.inspect(message(WebMessage::REQ_ADD_COMPUTER, {{"level", 2}}));
    CHECK(add.action == Verdict::Action::LOCAL);
    CHECK_EQ(int(add.type), int(WebMessage::REQ_ADD_COMPUTER));
    CHECK_EQ(add.payload["level"].get<int>(), 2);

    // One is all a visitor gets
    gatekeeper.computerAdded();
    CHECK(gatekeeper.inspect(message(WebMessage::REQ_ADD_COMPUTER, {{"level", 3}})).action == Verdict::Action::ANSWER);
}

void test_what_the_server_says_goes_through_as_it_is()
{
    Gatekeeper gatekeeper;
    introduce(gatekeeper);

    const std::string state = message(0x12, {{"game_id", 7}, {"rows", 6}, {"cols", 7}, {"current_player", 1}, {"status", 1}});
    CHECK_EQ(gatekeeper.translate(state).to_browser, state);

    // Even what it does not understand
    CHECK_EQ(gatekeeper.translate("not json at all").to_browser, "not json at all");
    CHECK_EQ(gatekeeper.translate("{\"type\":2}").to_browser, "{\"type\":2}");
    CHECK_EQ(gatekeeper.translate(message(0x21, {{"success", true}})).to_browser, message(0x21, {{"success", true}}));
}

int main()
{
    RUN_TEST(test_names_are_cleaned_up);
    RUN_TEST(test_game_shapes_are_checked);
    RUN_TEST(test_the_rate_limiter);
    RUN_TEST(test_garbage_is_refused);
    RUN_TEST(test_a_visitor_connects);
    RUN_TEST(test_the_token_never_reaches_the_browser);
    RUN_TEST(test_nothing_can_be_done_before_connecting);
    RUN_TEST(test_only_requests_get_through);
    RUN_TEST(test_listing_games);
    RUN_TEST(test_creating_a_game_joins_it);
    RUN_TEST(test_bad_games_do_not_reach_the_server);
    RUN_TEST(test_one_game_per_connection);
    RUN_TEST(test_a_refused_request_can_be_made_again);
    RUN_TEST(test_joining_a_game);
    RUN_TEST(test_moves);
    RUN_TEST(test_rooms_can_be_limited);
    RUN_TEST(test_heartbeats_and_goodbyes_stay_in_the_gateway);
    RUN_TEST(test_the_computer);
    RUN_TEST(test_what_the_server_says_goes_through_as_it_is);
    return testSummary("test_gatekeeper");
}
