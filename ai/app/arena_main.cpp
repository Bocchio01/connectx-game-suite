#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ai/arena.hpp"
#include "ai/factory.hpp"

namespace
{
    void printHelp()
    {
        std::cout << "ConnectX arena: tournaments between AI agents\n\n"
                  << "Usage: connectx-arena [options] <agent> <agent> [<agent> ...]\n\n"
                  << "Every agent plays every other agent (round-robin). Games are played in pairs that\n"
                  << "share the same random opening, with the colours swapped.\n\n"
                  << "Options:\n"
                  << "  -n, --games N         games per pairing (default 100)\n"
                  << "  -t, --threads N       games played in parallel (default: number of cores)\n"
                  << "      --rows R          board rows (default 6)\n"
                  << "      --cols C          board columns (default 7)\n"
                  << "      --connect K       stones to connect (default 4)\n"
                  << "      --random-plies N  random opening moves (default 2)\n"
                  << "      --seed S          random seed (default 1)\n"
                  << "      --list-agents     list the agent types and exit\n"
                  << "  -h, --help            show this message\n\n"
                  << "Agents are written as type[:key=value,...], e.g.\n"
                  << "  connectx-arena -n 200 random greedy alphabeta:depth=6 mcts:sims=2000,rollout=tactical\n";
    }

    void printAgents()
    {
        std::cout << "Available agents (type[:key=value,...]):\n\n";
        for (const ai::AgentInfo &info : ai::availableAgents())
        {
            std::cout << "  " << info.type << "\n"
                      << "      " << info.summary << "\n";
            if (!info.parameters.empty())
                std::cout << "      parameters: " << info.parameters << "\n";
            std::cout << "      example:    " << info.example << "\n\n";
        }
    }
}

int main(int argc, char *argv[])
{
    ai::ArenaOptions options;
    options.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::string> specs;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> const char *
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value after " << name << std::endl;
                std::exit(EXIT_FAILURE);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help")
        {
            printHelp();
            return EXIT_SUCCESS;
        }
        else if (arg == "--list-agents")
        {
            printAgents();
            return EXIT_SUCCESS;
        }
        else if (arg == "-n" || arg == "--games")
            options.games = std::atoi(next("--games"));
        else if (arg == "-t" || arg == "--threads")
            options.threads = std::max(1, std::atoi(next("--threads")));
        else if (arg == "--rows")
            options.rows = std::atoi(next("--rows"));
        else if (arg == "--cols")
            options.cols = std::atoi(next("--cols"));
        else if (arg == "--connect")
            options.connect = std::atoi(next("--connect"));
        else if (arg == "--random-plies")
            options.random_plies = std::max(0, std::atoi(next("--random-plies")));
        else if (arg == "--seed")
            options.seed = static_cast<uint64_t>(std::atoll(next("--seed")));
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "Unknown option: " << arg << "\n\n";
            printHelp();
            return EXIT_FAILURE;
        }
        else
            specs.push_back(arg);
    }

    if (specs.size() < 2)
    {
        printHelp();
        return EXIT_FAILURE;
    }

    std::string reason;
    if (!ai::Geometry::isSupported(options.rows, options.cols, options.connect, &reason))
    {
        std::cerr << "Unsupported board: " << reason << std::endl;
        return EXIT_FAILURE;
    }

    try
    {
        // Fail early on bad specs
        for (const std::string &spec : specs)
            ai::createAgent(spec);

        std::cout << "Board " << options.rows << "x" << options.cols << ", connect " << options.connect << ", "
                  << options.games << " games per pairing, " << options.threads << " thread(s), "
                  << options.random_plies << " random opening plies\n\n";

        const auto start = std::chrono::steady_clock::now();
        const auto results = ai::playRoundRobin(specs, options, [](const ai::PairResult &pair)
                                                {
            std::cout << "  " << pair.a << "  vs  " << pair.b << ":  " << pair.a_wins << " - " << pair.b_wins
                      << " (" << pair.draws << " draws)\n" << std::flush; });

        std::cout << "\n"
                  << ai::formatTournament(specs, results);
        std::cout << "\nTotal time: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
