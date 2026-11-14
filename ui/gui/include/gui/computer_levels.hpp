#pragma once

#include <QString>

#include "protocol/protocol.hpp"

/** Who the creator of a game plays against: other people, or the computer at one of the strength levels. */
struct OpponentChoice
{
    bool computer = false;
    int level = 0; // 1..ComputerLevels::kCount; only meaningful for the computer
};

/**
 * The strength levels of the computer opponent, and what each one means for the `connectx-ai` helper
 * (run `connectx-ai --list-agents` for the agent specs). The table is in computer_levels.cpp, in one
 * place, so that it can be tuned without touching anything else.
 *
 * Pure logic, no widgets and no process handling: the GUI only builds a command line from it.
 */
namespace ComputerLevels
{
    /** Levels are numbered from 1 (Beginner) to kCount (Expert). */
    constexpr int kCount = 5;

    /** The level offered first: strong enough to be interesting, weak enough to be beaten. */
    constexpr int kDefaultLevel = 2;

    bool isValid(int level);

    /** Short name of a level: "Beginner", "Easy", "Medium", "Hard", "Expert". */
    QString name(int level);

    /** One line that tells what to expect from a level (no numbers: it is meant for the selector). */
    QString summary(int level);

    /** How a level plays (for a tooltip); the answer depends on the board and on the network being available. */
    QString details(int level, const GameConfig &config, bool networkAvailable);

    /** Name of the computer player in the game: "Computer (Easy)". */
    QString playerName(int level);

    /** The standard game: 6 rows, 7 columns, connect 4, two players. */
    bool isStandardBoard(const GameConfig &config);

    /**
     * Agent spec to give to `connectx-ai --agent`. The pretrained network only exists for the standard
     * board, so the strongest levels play with something else everywhere else (and when it is missing).
     */
    QString agentSpec(int level, const GameConfig &config, bool networkAvailable);

    /**
     * Whether `connectx-ai` can play this game: two players on a board that fits its engine
     * (columns x (rows + 1) must not exceed 64, the connect length is limited to 3..16 and by the board).
     * `reason` receives an explanation for the user when the answer is no.
     */
    bool isSupported(const GameConfig &config, QString *reason = nullptr);
}
