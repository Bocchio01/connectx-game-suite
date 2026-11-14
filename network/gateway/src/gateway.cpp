#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <thread>

#include <spdlog/spdlog.h>

#include "gateway/gateway.hpp"
#include "gateway/session.hpp"
#include "gateway/socket_util.hpp"

namespace gateway
{
    namespace
    {
        /** The path the browsers connect to (the reverse proxy forwards it as it is); the root works too. */
        bool isGatewayPath(const std::string &path)
        {
            const std::string route = path.substr(0, path.find('?'));
            return route == "/connectx/ws" || route == "/";
        }

        /** Whether the text looks like an IP address: only what an address is made of, and not too long. */
        bool looksLikeAddress(const std::string &text)
        {
            return !text.empty() && text.size() <= 45 &&
                   std::all_of(text.begin(), text.end(), [](unsigned char c)
                               { return std::isxdigit(c) || c == '.' || c == ':'; });
        }

        std::string firstItem(const std::string &list)
        {
            const std::string item = list.substr(0, list.find(','));
            const size_t begin = item.find_first_not_of(' ');
            const size_t end = item.find_last_not_of(' ');
            return begin == std::string::npos ? std::string() : item.substr(begin, end - begin + 1);
        }

        /** Runs a function when it goes out of scope, whatever the way out. */
        struct ScopeGuard
        {
            std::function<void()> function;
            ~ScopeGuard() { function(); }
        };
    }

    Gateway::Gateway(Settings settings)
        : settings_(std::move(settings)),
          port_(settings_.port),
          running_(false),
          threads_(0)
    {
    }

    Gateway::~Gateway()
    {
        stop();
    }

    void Gateway::start()
    {
        spdlog::info("Starting the ConnectX web gateway...");

        sockpp::socket_initializer::initialize();

        try
        {
            if (!acceptor_.open(sockpp::inet_address(settings_.bind_address, settings_.port)))
            {
                spdlog::error("Cannot listen on {}:{}: {}", settings_.bind_address, settings_.port, acceptor_.last_error_str());
                return;
            }
        }
        catch (const std::exception &e)
        {
            spdlog::error("Invalid address to listen on '{}': {}", settings_.bind_address, e.what());
            return;
        }

        port_ = acceptor_.address().port();
        running_ = true;

        spdlog::info("Gateway listening on {}:{}, for the game server at {}:{}", settings_.bind_address, port_,
                     settings_.server_host, settings_.server_port);
        if (settings_.allowed_origins.empty())
            spdlog::warn("No allowed origin is set: pages of any website can connect (fine for development, not for production)");

        acceptConnections();
    }

    void Gateway::stop()
    {
        if (!running_.exchange(false))
            return;

        spdlog::info("Stopping the web gateway...");

        acceptor_.close();

        // Say goodbye to every browser: their sessions end by themselves
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (WebConnection *connection : connections_)
                connection->close(websocket::CloseCode::GOING_AWAY, "The server is shutting down");
        }

        for (int i = 0; i < 100 && threads_ > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        spdlog::info("Web gateway stopped.");
    }

    size_t Gateway::connectionCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

    void Gateway::acceptConnections()
    {
        while (running_)
        {
            // Look at the listening socket every now and then, so that stop() is noticed at once
            if (!waitForData(acceptor_, 200))
                continue;

            sockpp::inet_address peer;
            sockpp::tcp_socket socket = acceptor_.accept(&peer);

            if (!socket)
            {
                if (running_)
                    spdlog::error("Error accepting a connection: {}", acceptor_.last_error_str());
                continue;
            }

            // A flood of connections that never say anything must not be able to use up the threads
            if (threads_ >= settings_.max_connections + 50)
            {
                spdlog::warn("Too many connections: dropping one from {}", peer.to_string());
                continue;
            }

            ++threads_;
            std::thread([this, socket = std::move(socket)]() mutable
                        {
                ScopeGuard done{[this]()
                                { --threads_; }};
                try
                {
                    serve(std::move(socket));
                }
                catch (const std::exception &e)
                {
                    spdlog::error("Error while serving a browser: {}", e.what());
                } })
                .detach();
        }
    }

    bool Gateway::originAllowed(const websocket::HandshakeRequest &request) const
    {
        if (settings_.allowed_origins.empty())
            return true;

        // A browser always says which page opens the connection; a program that does not is not a browser
        const std::string origin = request.header("origin");
        return std::find(settings_.allowed_origins.begin(), settings_.allowed_origins.end(), origin) != settings_.allowed_origins.end();
    }

    std::string Gateway::clientAddress(const WebConnection &web, const websocket::HandshakeRequest &request)
    {
        // Behind a reverse proxy every connection comes from the proxy: the visitor's address is in a header,
        // which can be believed only when it is the proxy on this very machine that says so
        if (web.address() == "127.0.0.1")
        {
            std::string forwarded = firstItem(request.header("x-forwarded-for"));
            if (!looksLikeAddress(forwarded))
                forwarded = firstItem(request.header("x-real-ip"));
            if (looksLikeAddress(forwarded))
                return forwarded;
        }

        return web.address();
    }

    void Gateway::serve(sockpp::tcp_socket socket)
    {
        WebConnection web(std::move(socket));

        const auto request = web.readRequest(static_cast<int>(settings_.handshake_timeout.count()));
        if (!request)
            return;

        if (!isGatewayPath(request->path))
        {
            web.reject(404);
            return;
        }

        switch (websocket::checkUpgrade(*request))
        {
        case websocket::Upgrade::OK:
            break;
        case websocket::Upgrade::UNSUPPORTED_VERSION:
            web.reject(426);
            return;
        case websocket::Upgrade::BAD_REQUEST:
            web.reject(400);
            return;
        }

        if (!originAllowed(*request))
        {
            spdlog::warn("Refused a connection from the page '{}'", request->header("origin"));
            web.reject(403);
            return;
        }

        const std::string address = clientAddress(web, *request);

        // Is there room for one more?
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!running_ || connections_.size() >= settings_.max_connections)
            {
                web.reject(503);
                return;
            }
            if (per_address_[address] >= settings_.max_connections_per_address)
            {
                spdlog::warn("Too many connections from {}", address);
                web.reject(429);
                return;
            }

            ++per_address_[address];
            connections_.insert(&web);
        }

        ScopeGuard leave{[&]()
                         {
                             std::lock_guard<std::mutex> lock(mutex_);
                             connections_.erase(&web);
                             if (--per_address_[address] == 0)
                                 per_address_.erase(address);
                         }};

        if (!web.accept(*request))
            return;

        spdlog::info("{} connected ({} in all)", address, connectionCount());

        Session session(web, settings_, shared_);
        session.run();

        spdlog::info("{} left", address);
    }
}
