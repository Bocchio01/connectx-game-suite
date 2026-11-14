#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sockpp/tcp_connector.h>

#include "gateway/computer.hpp"
#include "gateway/gatekeeper.hpp"
#include "gateway/settings.hpp"
#include "gateway/web_connection.hpp"

namespace gateway
{
    /**
     * Everything that happens between one browser and the game server, for as long as the browser stays.
     *
     * The browser gets a connection of its own to the server (as a terminal client would), and the session carries
     * the messages across, one JSON message per WebSocket message, the server's own framing taken away and put back.
     * The server cannot tell the visitor from any other client; it is the gatekeeper that makes sure he behaves.
     *
     * run() uses two threads: the caller's reads from the browser, another one reads from the server.
     */
    class Session
    {
    public:
        Session(WebConnection &web, const Settings &settings, Shared &shared);
        ~Session();

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        /** Serves the browser until either side hangs up. */
        void run();

    private:
        bool connectToServer();
        void readFromServer();
        bool readServerMessage(std::string &message);
        bool writeToServer(const std::string &message);

        void handleBrowserMessage(const std::string &text);
        void answerLocal(const Verdict &verdict);
        void addComputer(const nlohmann::json &payload);
        void releaseFinishedComputer();
        void tidyUp();

        WebConnection &web_;
        const Settings &settings_;
        Shared &shared_;

        std::mutex gatekeeper_mutex_; // the gatekeeper is used by both threads
        Gatekeeper gatekeeper_;
        RateLimiter limiter_;

        sockpp::tcp_connector server_;
        std::mutex server_write_mutex_;
        std::thread server_reader_;
        std::atomic<bool> running_;

        bool room_counted_ = false; // touched only by the reader thread, and by the other one after it is gone
        std::unique_ptr<ComputerPlayer> computer_;
        bool computer_counted_ = false;
    };
}
