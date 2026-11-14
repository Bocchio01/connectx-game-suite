#pragma once

#include "ai/position.hpp"

namespace ai
{
    /**
     * Score of a position in which the side to move wins. The search subtracts
     * the number of plies needed, so that faster wins are preferred (and slower
     * defeats are preferred over faster ones).
     */
    constexpr int kWinScore = 1000000;

    /** Heuristic scores never exceed this value: they always stay below any proven win. */
    constexpr int kMaxHeuristic = 100000;

    /**
     * Static evaluation of a non-terminal position, from the point of view of
     * the player to move (positive = good for the player to move).
     *
     * The classic "open lines" heuristic: look at every line of `connect`
     * cells; a line that contains stones of one player only is an opportunity
     * for that player, worth more the more stones are already in it
     * (1, 10, 100 ... points). Stones in the central column(s) get a small
     * bonus because they take part in more lines.
     */
    int evaluate(const Position &position);
}
