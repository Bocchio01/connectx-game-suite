#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <sockpp/tcp_acceptor.h>

#include "gateway/settings.hpp"
#include "gateway/web_connection.hpp"
#include "gateway/websocket.hpp"

namespace gateway
{
    /**
     * Lets browsers play on a game server.
     *
     * A browser cannot open a TCP connection, which is what the clients of the server (the CLI, the GUI, the
     * computer players) do: all it can do is to open a WebSocket. The gateway is the adapter: it listens for
     * WebSockets and gives each browser a TCP connection of its own to the game server, carrying the messages from
     * one to the other. The server needs to know nothing about it, and it holds no game: whatever is in a game
     * lives in the server.
     *
     * It is meant to sit behind a reverse proxy (nginx, for instance) that takes care of TLS, which is mandatory for
     * a page served over https. See the README of this module.
     */
    class Gateway
    {
    public:
        explicit Gateway(Settings settings);
        ~Gateway();

        Gateway(const Gateway &) = delete;
        Gateway &operator=(const Gateway &) = delete;

        /** Starts listening and serving (blocking, until stop() is called). */
        void start();

        /** Stops listening, says goodbye to every browser and waits for the sessions to end. */
        void stop();

        bool isRunning() const { return running_; }

        /** The port it listens on: the one that was asked for, or the one the system chose if it was 0. */
        uint16_t port() const { return port_; }

        /** Number of browsers connected right now. */
        size_t connectionCount();

    private:
        void acceptConnections();
        void serve(sockpp::tcp_socket socket);

        bool originAllowed(const websocket::HandshakeRequest &request) const;
        static std::string clientAddress(const WebConnection &web, const websocket::HandshakeRequest &request);

        Settings settings_;
        Shared shared_;

        sockpp::tcp_acceptor acceptor_;
        uint16_t port_;
        std::atomic<bool> running_;
        std::atomic<size_t> threads_; // connections being served, handshakes included

        std::mutex mutex_; // protects what follows
        std::map<std::string, size_t> per_address_;
        std::set<WebConnection *> connections_;
    };
}
