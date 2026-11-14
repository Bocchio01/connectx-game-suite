#include <algorithm>

#include "gateway/socket_util.hpp"
#include "gateway/web_connection.hpp"

namespace gateway
{
    namespace
    {
        /** The request that opens a connection is a few hundred bytes; this is more than generous. */
        constexpr size_t MAX_REQUEST_SIZE = 8192;

        /** How often a blocked reader checks whether somebody closed the connection under its feet. */
        constexpr int POLL_SLICE_MS = 100;

        /** How long, after saying goodbye, to wait for the peer to hang up: a browser (or the proxy) does it at once. */
        constexpr std::chrono::seconds LINGER_TIME{2};

        using Clock = std::chrono::steady_clock;

        std::string ipv4(uint32_t address)
        {
            return std::to_string((address >> 24) & 0xFF) + "." + std::to_string((address >> 16) & 0xFF) + "." +
                   std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
        }
    }

    WebConnection::WebConnection(sockpp::tcp_socket socket)
        : socket_(std::move(socket)),
          open_(socket_.is_open()),
          upgraded_(false),
          last_heard_(Clock::now().time_since_epoch().count())
    {
        if (open_)
        {
            address_ = ipv4(socket_.peer_address().address());
            tune(socket_);
        }
    }

    WebConnection::~WebConnection()
    {
        close(websocket::CloseCode::GOING_AWAY);

        // Closing a socket that still has unread data does not say goodbye, it slams the door: the system resets the
        // connection, and the peer may lose the last things it was sent (the Close frame!) before it reads them
        drain();
    }

    std::optional<websocket::HandshakeRequest> WebConnection::readRequest(int timeout_ms)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        std::string received;

        for (;;)
        {
            const size_t end = received.find("\r\n\r\n");

            if (end != std::string::npos)
            {
                // Whatever follows the blank line already belongs to the WebSocket
                reader_.feed(received.data() + end + 4, received.size() - end - 4);
                return websocket::parseRequest(received.substr(0, end + 4));
            }

            if (received.size() > MAX_REQUEST_SIZE || !open_)
                return std::nullopt;

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0)
                return std::nullopt;

            if (!waitForData(socket_, static_cast<int>(std::min<long long>(remaining, POLL_SLICE_MS))))
                continue;

            char buffer[2048];
            const ssize_t n = socket_.read(buffer, sizeof(buffer));
            if (n <= 0)
                return std::nullopt;

            received.append(buffer, static_cast<size_t>(n));
        }
    }

    bool WebConnection::accept(const websocket::HandshakeRequest &request)
    {
        last_heard_ = Clock::now().time_since_epoch().count();

        if (!sendFrame(websocket::acceptResponse(request.header("sec-websocket-key"))))
            return false;

        upgraded_ = true;
        return true;
    }

    void WebConnection::reject(int status)
    {
        if (!open_.exchange(false))
            return;

        sendFrame(websocket::rejectResponse(status));
        socket_.shutdown(SHUT_WR);
    }

    websocket::Reader::Event WebConnection::receive(int timeout_ms)
    {
        using Event = websocket::Reader::Event;

        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

        auto ended = [](uint16_t code)
        {
            Event event;
            event.type = Event::Type::CLOSE;
            event.code = code;
            return event;
        };

        for (;;)
        {
            Event event = reader_.next();

            switch (event.type)
            {
            case Event::Type::PING:
                sendFrame(websocket::encode(websocket::Opcode::PONG, event.data));
                continue;

            case Event::Type::PONG:
                continue;

            case Event::Type::CLOSE:
                // The peer wants to leave: answer in kind (with the same status) and hang up
                close(event.code == websocket::CloseCode::NO_STATUS ? websocket::CloseCode::NORMAL : event.code);
                return event;

            case Event::Type::FAILURE:
                close(event.code);
                return event;

            case Event::Type::MESSAGE:
                return event;

            case Event::Type::NONE:
                break;
            }

            if (!open_)
                return ended(websocket::CloseCode::NORMAL);

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0)
                return Event();

            if (!waitForData(socket_, static_cast<int>(std::min<long long>(remaining, POLL_SLICE_MS))))
                continue;

            char buffer[4096];
            const ssize_t n = socket_.read(buffer, sizeof(buffer));

            if (n <= 0)
            {
                // The peer went away without a word (or we were shut down)
                open_ = false;
                return ended(websocket::CloseCode::ABNORMAL);
            }

            last_heard_ = Clock::now().time_since_epoch().count();
            reader_.feed(buffer, static_cast<size_t>(n));
        }
    }

    bool WebConnection::sendText(const std::string &message)
    {
        if (!open_ || message.size() > websocket::MAX_MESSAGE_SIZE)
            return false;

        return sendFrame(websocket::encode(websocket::Opcode::TEXT, message));
    }

    bool WebConnection::sendPing()
    {
        if (!open_)
            return false;

        return sendFrame(websocket::encode(websocket::Opcode::PING));
    }

    bool WebConnection::sendFrame(const std::string &frame)
    {
        std::lock_guard<std::mutex> lock(write_mutex_);

        if (!socket_.is_open())
            return false;

        return sendAll(socket_, frame.data(), frame.size());
    }

    void WebConnection::close(uint16_t code, const std::string &reason)
    {
        if (!open_.exchange(false))
            return;

        // Best effort: the peer may be gone already
        if (upgraded_)
            sendFrame(websocket::encodeClose(code, reason));

        // Nothing more goes out, and the peer sees the end of it right after the Close frame. (The reading side is
        // left alone: a reader blocked on this socket notices at its next look; the descriptor itself is closed by
        // the destructor)
        socket_.shutdown(SHUT_WR);
    }

    void WebConnection::drain()
    {
        if (!socket_.is_open())
            return;

        const auto deadline = Clock::now() + LINGER_TIME;
        char buffer[2048];

        while (Clock::now() < deadline)
        {
            if (!waitForData(socket_, POLL_SLICE_MS))
                continue;

            if (socket_.read(buffer, sizeof(buffer)) <= 0)
                break; // the peer has hung up too
        }
    }

    std::chrono::steady_clock::duration WebConnection::silence() const
    {
        return Clock::now() - Clock::time_point(Clock::duration(last_heard_.load()));
    }
}
