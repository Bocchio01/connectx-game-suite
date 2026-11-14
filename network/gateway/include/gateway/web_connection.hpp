#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <sockpp/tcp_socket.h>

#include "gateway/websocket.hpp"

namespace gateway
{
    /**
     * A browser connected to the gateway: a socket that speaks WebSocket.
     *
     * One thread reads (receive()), any thread can write: the frames of different threads do not get mixed up.
     */
    class WebConnection
    {
    public:
        explicit WebConnection(sockpp::tcp_socket socket);
        ~WebConnection();

        WebConnection(const WebConnection &) = delete;
        WebConnection &operator=(const WebConnection &) = delete;

        /** IP address of the peer, as the socket sees it. */
        const std::string &address() const { return address_; }

        /**
         * Reads the request that opens the connection, up to the blank line that ends its headers.
         * @return std::nullopt if the peer hangs up, sends too much or takes longer than `timeout_ms`
         */
        std::optional<websocket::HandshakeRequest> readRequest(int timeout_ms);

        /** Accepts the connection: from now on the two sides exchange WebSocket frames. */
        bool accept(const websocket::HandshakeRequest &request);

        /** Refuses the request with an HTTP status and hangs up. */
        void reject(int status);

        /**
         * Waits up to `timeout_ms` for what the peer sends next. Pings are answered here, and so are a Close
         * frame and a protocol violation (by closing), so whatever comes back is either a message or the end
         * of the connection. The result is of type NONE if time ran out.
         */
        websocket::Reader::Event receive(int timeout_ms);

        bool sendText(const std::string &message);
        bool sendPing();

        /**
         * Says goodbye with a Close frame and stops sending. Safe from any thread, and to repeat.
         * The socket itself is closed by the destructor, which first waits (a little) for the peer to hang up too.
         */
        void close(uint16_t code = websocket::CloseCode::NORMAL, const std::string &reason = std::string());

        bool isOpen() const { return open_; }

        /** How long ago the peer last sent anything (a Pong counts). */
        std::chrono::steady_clock::duration silence() const;

    private:
        bool sendFrame(const std::string &frame);

        /** Reads and throws away whatever the peer still sends, until it hangs up or for a short while. */
        void drain();

        sockpp::tcp_socket socket_;
        std::string address_;
        std::atomic<bool> open_;
        std::atomic<bool> upgraded_; // the opening handshake is over: before that there is no WebSocket to close
        std::mutex write_mutex_;
        websocket::Reader reader_;
        std::atomic<std::chrono::steady_clock::rep> last_heard_;
    };
}
