#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "ai/agent.hpp"
#include "client/client.hpp"

namespace ai
{
    /** How a bot finds the game it plays in (same options as the command line client). */
    enum class BotJoinMode
    {
        AUTO,    // join the first available game, or let the server create a standard one
        BY_ID,   // join a specific game
        BY_NAME, // join the open game with a given name
        CREATE   // create a game with a custom configuration and join it
    };

    struct BotOptions
    {
        std::string host = "localhost";
        uint16_t port = Protocol::DEFAULT_PORT;

        /** Name shown to the other players (default: the name of the agent). */
        std::string player_name;

        BotJoinMode join_mode = BotJoinMode::AUTO;
        uint32_t game_id = 0;
        std::string game_name;
        GameConfig game_config; // for CREATE

        /** Number of games to play one after the other (0 = until stopped). */
        int games = 1;

        /** Never answer faster than this, so that a human opponent can follow what is happening. */
        int min_move_ms = 0;

        /** How long to wait for an opponent to join (0 = for ever). */
        int wait_for_opponent_ms = 0;

        /** Receives the progress messages (nothing is printed if empty). */
        std::function<void(const std::string &)> log;

        /** Also describe every move (evaluation, nodes, time). */
        bool verbose = false;
    };

    /**
     * A computer player: connects to a server through the same Client class used by the CLI and the
     * GUI, joins (or creates) a game and plays it with an Agent until the game is over.
     *
     * Only two-player games on boards that the bitboard engine supports can be played
     * (see Geometry::isSupported); anything else is refused with an explanatory message.
     */
    class BotClient
    {
    public:
        BotClient(const BotOptions &options, AgentPtr agent);

        /** Plays the requested games. @return 0 if they were all played, non-zero on errors */
        int run();

        /** Asks run() to return as soon as possible (thread-safe). */
        void requestStop();

    private:
        enum class Outcome
        {
            FINISHED, // the game was played to its end
            ABORTED,  // the opponent left, the connection dropped or a stop was requested
            FAILED    // could not connect / join
        };

        Outcome playOneGame(int game_index);
        bool joinGame(Client &client, JoinGameResponse &response, std::string &error);
        void say(const std::string &text) const;

        BotOptions options_;
        AgentPtr agent_;
        std::atomic<bool> stop_;

        // State shared with the Client's event thread
        std::mutex mutex_;
        std::condition_variable condition_;
        std::optional<GameStateUpdate> latest_state_;
        uint64_t state_sequence_;
        bool disconnected_;
        bool move_rejected_;
        std::string last_error_;
    };
}
