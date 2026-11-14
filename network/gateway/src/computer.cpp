#include <stdexcept>

#include <spdlog/spdlog.h>

#include "gateway/computer.hpp"

#ifdef CONNECTX_GATEWAY_COMPUTER
#include "ai/bot_client.hpp"
#include "ai/factory.hpp"
#include "ai/model_cache.hpp"
#include "ai/position.hpp"
#endif

namespace gateway
{
#ifdef CONNECTX_GATEWAY_COMPUTER

    namespace
    {
        /** How long the computer takes, at least, to answer: quick enough to be fun, slow enough to follow. */
        constexpr int MIN_MOVE_MS = 600;

        /** The network was trained for this board only. */
        constexpr const char *NETWORK = "connectx_6x7";

        struct LevelDefinition
        {
            const char *name;
            const char *summary;
            const char *spec_standard; // on the standard board, when the pretrained network is installed
            const char *spec_other;    // everywhere else
        };

        const LevelDefinition LEVELS[] = {
            {"Beginner",
             "Sees only the next move: takes a win, blocks yours, otherwise stays near the centre.",
             "greedy",
             "greedy"},
            {"Easy",
             "Thinks four moves ahead: sees simple traps, falls for deeper ones.",
             "alphabeta:depth=4",
             "alphabeta:depth=4"},
            {"Medium",
             "Thinks eight moves ahead: rarely misses a tactic.",
             "alphabeta:depth=8",
             "alphabeta:depth=8"},
            {"Hard",
             "A strong player: it hardly ever overlooks a threat.",
             "az:model=connectx_6x7,sims=100",
             "mcts:sims=3000,rollout=tactical"},
            {"Expert",
             "The strongest level: slow to answer, very accurate.",
             "az:model=connectx_6x7,sims=400",
             "mcts:sims=10000,rollout=tactical"},
        };

        constexpr int LEVEL_COUNT = static_cast<int>(sizeof(LEVELS) / sizeof(LEVELS[0]));

        bool isStandardBoard(const GameConfig &config)
        {
            return config.rows == 6 && config.cols == 7 && config.connect_length == 4 && config.num_players == 2;
        }

        bool networkInstalled()
        {
            try
            {
                ai::resolveModelPath(NETWORK);
                return true;
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        std::string agentSpec(int level, const GameConfig &config)
        {
            const LevelDefinition &row = LEVELS[level - 1];
            return isStandardBoard(config) && networkInstalled() ? row.spec_standard : row.spec_other;
        }
    }

    std::vector<ComputerLevel> computerLevels()
    {
        std::vector<ComputerLevel> levels;

        for (int i = 0; i < LEVEL_COUNT; ++i)
            levels.push_back({i + 1, LEVELS[i].name, LEVELS[i].summary});

        return levels;
    }

    std::string computerRefusal(const GameConfig &config)
    {
        if (config.num_players != 2)
            return "The computer plays two-player games only";

        std::string reason;
        if (!ai::Geometry::isSupported(config.rows, config.cols, config.connect_length, &reason))
            return "The computer cannot play on this board: " + reason;

        return std::string();
    }

    struct ComputerPlayer::Impl
    {
        std::unique_ptr<ai::BotClient> bot;
    };

    ComputerPlayer::ComputerPlayer(const std::string &host, uint16_t port, uint32_t game_id, int level, const GameConfig &config)
        : finished_(false),
          impl_(new Impl)
    {
        if (level < 1 || level > LEVEL_COUNT)
            throw std::runtime_error("There is no such level");

        name_ = std::string("Computer (") + LEVELS[level - 1].name + ")";

        ai::BotOptions options;
        options.host = host;
        options.port = port;
        options.player_name = name_;
        options.join_mode = ai::BotJoinMode::BY_ID;
        options.game_id = game_id;
        options.games = 1;
        options.min_move_ms = MIN_MOVE_MS;
        options.log = [game_id](const std::string &text)
        { spdlog::debug("[computer, game {}] {}", game_id, text); };

        // Throws if the algorithm cannot be set up, e.g. if its network file is not there
        try
        {
            impl_->bot = std::make_unique<ai::BotClient>(options, ai::createAgent(agentSpec(level, config)));
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error(std::string("The computer cannot start: ") + e.what());
        }

        thread_ = std::thread([this, game_id]()
                              {
            try
            {
                impl_->bot->run();
            }
            catch (const std::exception &e)
            {
                spdlog::error("The computer of game {} failed: {}", game_id, e.what());
            }

            finished_ = true; });
    }

    ComputerPlayer::~ComputerPlayer()
    {
        if (impl_ && impl_->bot)
            impl_->bot->requestStop();

        if (thread_.joinable())
            thread_.join();
    }

#else // built without the AI module

    std::vector<ComputerLevel> computerLevels()
    {
        return {};
    }

    std::string computerRefusal(const GameConfig &)
    {
        return "The computer is not available on this server";
    }

    struct ComputerPlayer::Impl
    {
    };

    ComputerPlayer::ComputerPlayer(const std::string &, uint16_t, uint32_t, int, const GameConfig &)
        : finished_(true)
    {
        throw std::runtime_error("The computer is not available on this server");
    }

    ComputerPlayer::~ComputerPlayer() = default;

#endif
}
