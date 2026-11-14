#include <algorithm>

#include "ai/evaluation.hpp"

namespace ai
{
    namespace
    {
        /** Value of a line that contains `count` stones of a single player and no stone of the other. */
        int lineValue(int count)
        {
            static const int values[] = {0, 1, 10, 100, 1000, 10000};
            return values[std::min(count, 5)];
        }
    }

    int evaluate(const Position &position)
    {
        const Geometry &geometry = position.geometry();
        const uint64_t mine = position.current();
        const uint64_t theirs = position.opponent();

        int score = 0;
        for (const uint64_t window : geometry.windows)
        {
            const int my_stones = popcount(mine & window);
            const int their_stones = popcount(theirs & window);

            if (their_stones == 0)
                score += lineValue(my_stones);
            else if (my_stones == 0)
                score -= lineValue(their_stones);
        }

        score += 4 * (popcount(mine & geometry.center_mask) - popcount(theirs & geometry.center_mask));

        return std::max(-kMaxHeuristic, std::min(kMaxHeuristic, score));
    }
}
