// connectx-gateway: lets browsers play on a ConnectX server (see the README of the module)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "gateway/gateway.hpp"

namespace
{
    std::atomic<bool> g_stop{false};

    void onSignal(int)
    {
        // Only an atomic store: this is a signal handler. The main thread polls the flag.
        g_stop = true;
    }

    void printUsage(const char *program)
    {
        std::cout << "Usage: " << program << " [options]\n\n"
                  << "Lets browsers play on a ConnectX server, carrying their WebSocket connections to it.\n\n"
                  << "Options:\n"
                  << "  -h, --help                  Show this message\n"
                  << "  -p, --port PORT             Port to listen on (default: 8081)\n"
                  << "  -b, --bind ADDRESS          Address of the network interface to listen on (default: 127.0.0.1,\n"
                  << "                              the machine itself: a reverse proxy is the one that talks to the world)\n"
                  << "      --server-host HOST      Host of the game server (default: 127.0.0.1)\n"
                  << "      --server-port PORT      Port of the game server (default: " << Protocol::DEFAULT_PORT << ")\n"
                  << "      --origin ORIGIN         A page that may connect, as the browser names it, e.g. https://bocchio.dev\n"
                  << "                              (repeat for more; default: pages of any website)\n"
                  << "      --max-connections N     Browsers connected at once (default: 100)\n"
                  << "      --max-per-address N     Connections from one address (default: 6)\n"
                  << "      --max-rooms N           Games created through the gateway and still open (default: 30)\n"
                  << "      --max-computers N       Computer players at once, 0 = no computer (default: 4)\n"
                  << "  -v, --verbose               Log what every connection does\n";
    }

    int number(const std::string &text, const std::string &option, int min, int max)
    {
        try
        {
            size_t used = 0;
            const int value = std::stoi(text, &used);
            if (used == text.size() && value >= min && value <= max)
                return value;
        }
        catch (const std::exception &)
        {
        }

        throw std::invalid_argument("Invalid value for " + option + ": " + text);
    }
}

int main(int argc, char *argv[])
{
    gateway::Settings settings;

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");

    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string
            {
                if (i + 1 >= argc)
                    throw std::invalid_argument("Missing value after " + arg);
                return argv[++i];
            };

            if (arg == "-h" || arg == "--help")
            {
                printUsage(argv[0]);
                return EXIT_SUCCESS;
            }
            else if (arg == "-p" || arg == "--port")
                settings.port = static_cast<uint16_t>(number(value(), arg, 1, 65535));
            else if (arg == "-b" || arg == "--bind")
                settings.bind_address = value();
            else if (arg == "--server-host")
                settings.server_host = value();
            else if (arg == "--server-port")
                settings.server_port = static_cast<uint16_t>(number(value(), arg, 1, 65535));
            else if (arg == "--origin")
                settings.allowed_origins.push_back(value());
            else if (arg == "--max-connections")
                settings.max_connections = static_cast<size_t>(number(value(), arg, 1, 10000));
            else if (arg == "--max-per-address")
                settings.max_connections_per_address = static_cast<size_t>(number(value(), arg, 1, 10000));
            else if (arg == "--max-rooms")
                settings.max_rooms = static_cast<size_t>(number(value(), arg, 1, 10000));
            else if (arg == "--max-computers")
                settings.max_computers = static_cast<size_t>(number(value(), arg, 0, 1000));
            else if (arg == "-v" || arg == "--verbose")
                spdlog::set_level(spdlog::level::debug);
            else
            {
                std::cerr << "Unknown option: " << arg << "\n\n";
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n\n";
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifndef _WIN32
    // Writing to a browser that has just gone must fail with an error, not kill the program
    std::signal(SIGPIPE, SIG_IGN);
#endif

    gateway::Gateway gateway(settings);

    // A signal handler cannot do much: another thread waits for the flag it raises and stops the gateway
    std::atomic<bool> finished{false};
    std::thread watcher([&]()
                        {
        while (!g_stop && !finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        gateway.stop(); });

    gateway.start(); // returns when stop() is called, or at once if it cannot listen

    finished = true;
    watcher.join();

    return g_stop ? EXIT_SUCCESS : EXIT_FAILURE;
}
