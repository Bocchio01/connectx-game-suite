#include <cstring>

#ifndef _WIN32
#include <cerrno>
#endif

#include <spdlog/spdlog.h>

#include "gateway/session.hpp"
#include "gateway/socket_util.hpp"

using json = nlohmann::json;

namespace gateway
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        /** How often a blocked reader checks whether it has to stop. */
        constexpr int POLL_SLICE_MS = 100;

        /** How long to wait for the game server to accept the connection. */
        constexpr std::chrono::seconds SERVER_CONNECT_TIMEOUT{3};

        /** What a person can do with a mouse is nowhere near this: a burst of 30 messages, then 15 every second. */
        constexpr double RATE_BURST = 30;
        constexpr double RATE_PER_SECOND = 15;
    }

    Session::Session(WebConnection &web, const Settings &settings, Shared &shared)
        : web_(web),
          settings_(settings),
          shared_(shared),
          gatekeeper_([this]()
                      { return shared_.rooms.load() < settings_.max_rooms; }),
          limiter_(RATE_BURST, RATE_PER_SECOND),
          running_(false)
    {
    }

    Session::~Session()
    {
        tidyUp();
    }

    // ========================================================================
    // Running
    // ========================================================================

    void Session::run()
    {
        if (!connectToServer())
        {
            web_.sendText(errorMessage(Protocol::ErrorCode::CONNECTION_REFUSED, "The game server is not available right now"));
            web_.close(websocket::CloseCode::TRY_AGAIN_LATER, "Game server unavailable");
            return;
        }

        running_ = true;
        server_reader_ = std::thread([this]()
                                     { readFromServer(); });

        auto last_ping = Clock::now();
        auto last_message = Clock::now();

        while (running_ && web_.isOpen())
        {
            const websocket::Reader::Event event = web_.receive(500);
            const auto now = Clock::now();

            if (event.type == websocket::Reader::Event::Type::MESSAGE)
            {
                last_message = now;

                if (!limiter_.allow(now))
                {
                    spdlog::warn("{} is sending too many messages", web_.address());
                    web_.close(websocket::CloseCode::POLICY_VIOLATION, "Too many messages");
                    break;
                }

                handleBrowserMessage(event.data);
            }
            else if (event.type != websocket::Reader::Event::Type::NONE)
            {
                break; // the browser has gone, or broke the rules (the connection is closed already)
            }

            // Looking after the connection: a browser that does not even answer the pings is gone,
            // one that has been doing nothing for a very long time is just holding a place
            if (web_.silence() > settings_.silence_limit)
            {
                web_.close(websocket::CloseCode::GOING_AWAY, "No answer");
                break;
            }
            if (now - last_message > settings_.idle_limit)
            {
                web_.close(websocket::CloseCode::GOING_AWAY, "Idle for too long");
                break;
            }
            if (now - last_ping >= settings_.ping_interval)
            {
                web_.sendPing();
                last_ping = now;
            }

            releaseFinishedComputer();
        }

        tidyUp();
    }

    void Session::tidyUp()
    {
        running_ = false;

        // Hanging up on the server is what takes the visitor out of his game, and ends the game of a computer
        // that was playing him
        server_.shutdown();
        if (server_reader_.joinable())
            server_reader_.join();
        server_.close();

        computer_.reset();
        if (computer_counted_)
        {
            --shared_.computers;
            computer_counted_ = false;
        }
        if (room_counted_)
        {
            --shared_.rooms;
            room_counted_ = false;
        }

        web_.close(websocket::CloseCode::GOING_AWAY);
    }

    // ========================================================================
    // The game server
    // ========================================================================

    bool Session::connectToServer()
    {
        try
        {
            sockpp::socket_initializer::initialize();

            if (!server_.connect(sockpp::inet_address(settings_.server_host, settings_.server_port), SERVER_CONNECT_TIMEOUT))
            {
                spdlog::warn("Cannot reach the game server at {}:{}: {}", settings_.server_host, settings_.server_port,
                             server_.last_error_str());
                return false;
            }

            tune(server_);
            return true;
        }
        catch (const std::exception &e)
        {
            spdlog::error("Cannot reach the game server at {}:{}: {}", settings_.server_host, settings_.server_port, e.what());
            return false;
        }
    }

    bool Session::readServerMessage(std::string &message)
    {
        // Wait in short slices: that is how the thread notices that it has to leave
        auto readExactly = [this](char *buffer, size_t size)
        {
            size_t received = 0;

            while (received < size)
            {
                if (!running_)
                    return false;
                if (!waitForData(server_, POLL_SLICE_MS))
                    continue;

                const ssize_t n = server_.read(buffer + received, size - received);
                if (n < 0)
                {
#ifndef _WIN32
                    if (server_.last_error() == EINTR)
                        continue;
#endif
                    return false;
                }
                if (n == 0)
                    return false; // the server closed the connection

                received += static_cast<size_t>(n);
            }

            return true;
        };

        // The server frames its messages with their length, 4 bytes in network order
        uint32_t length = 0;
        if (!readExactly(reinterpret_cast<char *>(&length), sizeof(length)))
            return false;

        length = ntohl(length);
        if (length == 0 || length > Protocol::MAX_MESSAGE_SIZE)
            return false;

        message.resize(length);
        return readExactly(&message[0], length);
    }

    bool Session::writeToServer(const std::string &message)
    {
        if (message.size() > Protocol::MAX_MESSAGE_SIZE)
            return false;

        std::string frame(sizeof(uint32_t), '\0');
        const uint32_t length = htonl(static_cast<uint32_t>(message.size()));
        std::memcpy(&frame[0], &length, sizeof(length));
        frame += message;

        // (one write, under the lock: messages from the two threads cannot get mixed up)
        std::lock_guard<std::mutex> lock(server_write_mutex_);
        return sendAll(server_, frame.data(), frame.size());
    }

    void Session::readFromServer()
    {
        std::string message;

        while (running_ && readServerMessage(message))
        {
            Translation translation;
            {
                std::lock_guard<std::mutex> lock(gatekeeper_mutex_);
                translation = gatekeeper_.translate(message);
            }

            if (translation.room_created)
            {
                ++shared_.rooms;
                room_counted_ = true;
            }

            if (!web_.sendText(translation.to_browser))
                break;
            if (translation.to_server && !writeToServer(*translation.to_server))
                break;
        }

        // Not asked to stop: the server went away
        if (running_)
        {
            spdlog::info("The game server closed the connection of {}", web_.address());
            web_.close(websocket::CloseCode::INTERNAL_ERROR, "The game server closed the connection");
        }

        running_ = false;
    }

    // ========================================================================
    // The browser
    // ========================================================================

    void Session::handleBrowserMessage(const std::string &text)
    {
        Verdict verdict;
        {
            std::lock_guard<std::mutex> lock(gatekeeper_mutex_);
            verdict = gatekeeper_.inspect(text);
        }

        switch (verdict.action)
        {
        case Verdict::Action::FORWARD:
            if (!writeToServer(verdict.text))
            {
                web_.sendText(errorMessage(Protocol::ErrorCode::CONNECTION_REFUSED, "The game server is not answering"));
                running_ = false;
            }
            break;

        case Verdict::Action::ANSWER:
            web_.sendText(verdict.text);
            break;

        case Verdict::Action::LOCAL:
            answerLocal(verdict);
            break;

        case Verdict::Action::CLOSE:
            running_ = false;
            break;
        }
    }

    void Session::answerLocal(const Verdict &verdict)
    {
        if (verdict.type == WebMessage::REQ_ADD_COMPUTER)
        {
            addComputer(verdict.payload);
            return;
        }

        // REQ_COMPUTER_LEVELS
        json levels = json::array();
        for (const ComputerLevel &level : computerLevels())
            levels.push_back({{"level", level.level}, {"name", level.name}, {"summary", level.summary}});

        json payload;
        payload["available"] = settings_.max_computers > 0 && !levels.empty();
        payload["levels"] = levels;

        web_.sendText(webMessage(WebMessage::RES_COMPUTER_LEVELS, payload));
    }

    void Session::addComputer(const json &payload)
    {
        auto answer = [this](bool success, const std::string &text, const std::string &name = std::string())
        {
            json reply;
            reply["success"] = success;
            reply["message"] = text;
            reply["name"] = name;
            web_.sendText(webMessage(WebMessage::RES_ADD_COMPUTER, reply));
        };

        if (settings_.max_computers == 0 || computerLevels().empty())
            return answer(false, "The computer is not available on this server");

        const json level = payload.contains("level") ? payload["level"] : json();
        if (!level.is_number_integer())
            return answer(false, "Choose how strong the computer should be");

        GameInfo game;
        {
            std::lock_guard<std::mutex> lock(gatekeeper_mutex_);
            if (!gatekeeper_.game())
                return answer(false, "Open a game first");
            game = *gatekeeper_.game();
        }

        const std::string refusal = computerRefusal(game.config);
        if (!refusal.empty())
            return answer(false, refusal);

        // Take a place among the computers, if there is one
        size_t playing = shared_.computers.load();
        do
        {
            if (playing >= settings_.max_computers)
                return answer(false, "All the computers are busy at the moment: try again in a while");
        } while (!shared_.computers.compare_exchange_weak(playing, playing + 1));

        try
        {
            computer_ = std::make_unique<ComputerPlayer>(settings_.server_host, settings_.server_port, game.game_id,
                                                         level.get<int>(), game.config);
        }
        catch (const std::exception &e)
        {
            --shared_.computers;
            spdlog::warn("Could not start a computer: {}", e.what());
            return answer(false, e.what());
        }

        computer_counted_ = true;
        {
            std::lock_guard<std::mutex> lock(gatekeeper_mutex_);
            gatekeeper_.computerAdded();
        }

        answer(true, "The computer is joining the game", computer_->name());
    }

    void Session::releaseFinishedComputer()
    {
        // A computer that has played its game does not need its place any more
        if (computer_ && computer_->finished())
        {
            computer_.reset();

            if (computer_counted_)
            {
                --shared_.computers;
                computer_counted_ = false;
            }
        }
    }
}
