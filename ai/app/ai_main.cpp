// connectx-ai: a computer player for the ConnectX server

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>

#include <spdlog/spdlog.h>

#include "ai/bot_client.hpp"
#include "ai/factory.hpp"

namespace
{
    ai::BotClient *g_bot = nullptr;

    void onSignal(int)
    {
        if (g_bot != nullptr)
            g_bot->requestStop();
    }

    void printHelp()
    {
        std::cout << "ConnectX AI client: a computer player\n\n"
                  << "Usage: connectx-ai [options]\n\n"
                  << "Options:\n"
                  << "  -h, --help             Show this help message\n"
                  << "      --list-agents      List the agent types and exit\n"
                  << "      --agent SPEC       The algorithm that plays (default: alphabeta:depth=12,time=1000)\n"
                  << "  -n, --name NAME        Name shown to the other players (default: the agent)\n"
                  << "  -g, --game SPEC        Game to join (see below; default: first available game)\n"
                  << "      --games N          Play N games one after the other, 0 = until stopped (default 1)\n"
                  << "      --delay MS         Never answer faster than this (default 400)\n"
                  << "      --wait SECONDS     Give up waiting for an opponent after this long (default: wait)\n"
                  << "  -v, --verbose          Describe every move (evaluation, nodes, time)\n"
                  << "      --host HOST        Server hostname (default: localhost)\n"
                  << "  -p, --port PORT        Server port (default: " << Protocol::DEFAULT_PORT << ")\n"
                  << "\nGame specification (as in connectx-cli):\n"
                  << "  <game_id>              Join game by ID (e.g., -g 42)\n"
                  << "  <name>                 Join the open game with that name (e.g., -g Duel)\n"
                  << "  'name<r c p n>'        Create a game and join it: rows, columns, players (2 for the AI), connect length\n"
                  << "                         Example: -g 'Duel<6 7 2 4>'\n"
                  << "\nAgents (type[:key=value,...]; run --list-agents for the details):\n"
                  << "  random, greedy, minimax:depth=5, alphabeta:depth=10,time=1000, solver:time=3000,\n"
                  << "  mcts:sims=5000,rollout=tactical, nn:model=<file>, az:model=<file>,sims=400\n"
                  << "\nExamples:\n"
                  << "  connectx-ai                                    # a decent opponent, joins any open game\n"
                  << "  connectx-ai -g Duel --agent solver:time=3000   # (almost) unbeatable\n"
                  << "  connectx-ai -g 'Duel<6 7 2 4>' --agent az:model=connectx_6x7.cxnn,sims=400\n"
                  << "  connectx-ai --games 0 --agent mcts:sims=3000   # keeps playing game after game\n";
    }

    void printAgents()
    {
        std::cout << "Available agents (type[:key=value,...]):\n\n";
        for (const ai::AgentInfo &info : ai::availableAgents())
        {
            std::cout << "  " << info.type << "\n      " << info.summary << "\n";
            if (!info.parameters.empty())
                std::cout << "      parameters: " << info.parameters << "\n";
            std::cout << "      example:    " << info.example << "\n\n";
        }
    }
}

int main(int argc, char *argv[])
{
    spdlog::set_level(spdlog::level::err);

    ai::BotOptions options;
    options.min_move_ms = 400;
    options.log = [](const std::string &text)
    { std::cout << text << std::endl; };

    std::string agent_spec = "alphabeta:depth=12,time=1000";

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
                printHelp();
                return EXIT_SUCCESS;
            }
            else if (arg == "--list-agents")
            {
                printAgents();
                return EXIT_SUCCESS;
            }
            else if (arg == "--agent")
                agent_spec = value();
            else if (arg == "-n" || arg == "--name")
                options.player_name = value();
            else if (arg == "--host")
                options.host = value();
            else if (arg == "-p" || arg == "--port")
                options.port = static_cast<uint16_t>(std::stoi(value()));
            else if (arg == "--games")
                options.games = std::stoi(value());
            else if (arg == "--delay")
                options.min_move_ms = std::stoi(value());
            else if (arg == "--wait")
                options.wait_for_opponent_ms = std::stoi(value()) * 1000;
            else if (arg == "-v" || arg == "--verbose")
                options.verbose = true;
            else if (arg == "-g" || arg == "--game")
            {
                const std::string game = value();
                if (game.find('<') != std::string::npos)
                {
                    // name<rows cols players connect>
                    const std::regex pattern(R"(([^<]*)<(\d+)\s+(\d+)\s+(\d+)\s+(\d+)>)");
                    std::smatch match;
                    if (!std::regex_match(game, match, pattern))
                        throw std::invalid_argument("Invalid game specification '" + game + "' (expected name<rows cols players connect>)");

                    options.join_mode = ai::BotJoinMode::CREATE;
                    options.game_name = match[1];
                    options.game_config = GameConfig(static_cast<uint8_t>(std::stoi(match[2])), static_cast<uint8_t>(std::stoi(match[3])),
                                                     static_cast<uint8_t>(std::stoi(match[4])), static_cast<uint8_t>(std::stoi(match[5])));
                }
                else
                {
                    try
                    {
                        size_t used = 0;
                        const unsigned long id = std::stoul(game, &used);
                        if (used != game.size())
                            throw std::invalid_argument("name");
                        options.join_mode = ai::BotJoinMode::BY_ID;
                        options.game_id = static_cast<uint32_t>(id);
                    }
                    catch (const std::exception &)
                    {
                        options.join_mode = ai::BotJoinMode::BY_NAME;
                        options.game_name = game;
                    }
                }
            }
            else
            {
                std::cerr << "Unknown option: " << arg << "\n\n";
                printHelp();
                return EXIT_FAILURE;
            }
        }

        ai::AgentPtr agent = ai::createAgent(agent_spec);
        ai::BotClient bot(options, std::move(agent));

        g_bot = &bot;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        const int code = bot.run();
        g_bot = nullptr;
        return code;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
