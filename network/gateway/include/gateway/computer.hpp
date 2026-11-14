#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "protocol/protocol.hpp"

namespace gateway
{
    /** One of the strengths a visitor can choose for the computer. */
    struct ComputerLevel
    {
        int level; // 1 (the weakest) to computerLevels().size()
        std::string name;
        std::string summary;
    };

    /**
     * The strengths of the computer, from the weakest to the strongest. The list is empty when the gateway was built
     * without the AI module: then nobody can play against the computer.
     *
     * (This is the same ladder as the graphical client, ui/gui/src/computer_levels.cpp: if one is retuned, so
     * should be the other.)
     */
    std::vector<ComputerLevel> computerLevels();

    /** Why the computer cannot play a game of this shape: an empty text if it can. */
    std::string computerRefusal(const GameConfig &config);

    /**
     * A computer opponent. It is a client of the server like any other, indistinguishable from a person: it joins the
     * game and plays it, in a thread of its own, until the game is over, its opponent leaves or it is destroyed.
     */
    class ComputerPlayer
    {
    public:
        /**
         * Starts a computer that joins the game `game_id`.
         * @throws std::runtime_error if there is no such level or the algorithm cannot be set up
         */
        ComputerPlayer(const std::string &host, uint16_t port, uint32_t game_id, int level, const GameConfig &config);

        /** Asks the computer to stop and waits for it to do so. */
        ~ComputerPlayer();

        ComputerPlayer(const ComputerPlayer &) = delete;
        ComputerPlayer &operator=(const ComputerPlayer &) = delete;

        /** The name it plays under, e.g. "Computer (Medium)". */
        const std::string &name() const { return name_; }

        /** Whether it has done playing. */
        bool finished() const { return finished_; }

    private:
        struct Impl;

        std::string name_;
        std::atomic<bool> finished_;
        std::unique_ptr<Impl> impl_;
        std::thread thread_;
    };
}
