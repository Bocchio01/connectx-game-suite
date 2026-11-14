#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol/constants.hpp"

namespace gateway
{
    /** How the gateway is set up. The defaults are meant for a gateway that sits behind a reverse proxy. */
    struct Settings
    {
        // Where the browsers connect. By default only the machine itself is allowed in: the reverse proxy is the
        // one that talks to the world (and takes care of TLS).
        std::string bind_address = "127.0.0.1";
        uint16_t port = 8081;

        // The game server the browsers are connected to
        std::string server_host = "127.0.0.1";
        uint16_t server_port = Protocol::DEFAULT_PORT;

        // The pages that may open a connection, as their browsers name them in the Origin header
        // ("https://bocchio.dev"). Empty: any page may, which is fine on a development machine and not elsewhere.
        std::vector<std::string> allowed_origins;

        // Limits. They keep one visitor, or one program that pretends to be many, from filling the server.
        size_t max_connections = 100;
        size_t max_connections_per_address = 6;
        size_t max_rooms = 30;     // games created through the gateway and still open
        size_t max_computers = 4;  // computers playing at once (0: no computer at all)

        // Timing
        std::chrono::milliseconds handshake_timeout{5000};
        std::chrono::milliseconds ping_interval{25000}; // also keeps the reverse proxy from closing idle connections
        std::chrono::milliseconds silence_limit{75000}; // a browser that does not even answer the pings is gone
        std::chrono::milliseconds idle_limit{30 * 60 * 1000};
    };

    /** What the sessions share: how many rooms and computers there are in all. */
    struct Shared
    {
        std::atomic<size_t> rooms{0};
        std::atomic<size_t> computers{0};
    };
}
