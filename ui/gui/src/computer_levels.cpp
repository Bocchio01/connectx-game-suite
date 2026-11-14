#include <algorithm>

#include <QCoreApplication>

#include "gui/computer_levels.hpp"

namespace
{
    /**
     * One row of the table of levels. Tune the computer opponent here and nowhere else.
     *
     * The specs are `connectx-ai` agent specs (`connectx-ai --list-agents`). `specStandard` is used on the
     * standard board when the pretrained network is available, `specOther` on any other board the helper
     * can play. How strong they are (Elo in the tournament of the AI module, standard board, 1000 = the
     * average of the field): greedy 713, alphabeta depth 4: 949, alphabeta depth 8: 1079, az with 100
     * simulations 1175, az with 400 simulations 1342, mcts with 10000 tactical simulations 1247.
     */
    struct LevelDefinition
    {
        const char *name;
        const char *summary;
        const char *howStandard; // how it plays on the standard board, with the network
        const char *howOther;    // how it plays everywhere else
        const char *specStandard;
        const char *specOther;
    };

    const LevelDefinition kLevels[ComputerLevels::kCount] = {
        {QT_TRANSLATE_NOOP("ComputerLevels", "Beginner"),
         QT_TRANSLATE_NOOP("ComputerLevels", "Sees only the next move: takes a win, blocks yours, otherwise stays near the centre."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Looks one move ahead. It wins when it can, blocks a win of yours and avoids moves that hand you one, "
                                             "otherwise it prefers the centre. It does not plan, so any trap that needs two moves of foresight works."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Looks one move ahead. It wins when it can, blocks a win of yours and avoids moves that hand you one, "
                                             "otherwise it prefers the centre. It does not plan, so any trap that needs two moves of foresight works."),
         "greedy",
         "greedy"},

        {QT_TRANSLATE_NOOP("ComputerLevels", "Easy"),
         QT_TRANSLATE_NOOP("ComputerLevels", "Thinks four moves ahead: sees simple traps, falls for deeper ones."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Searches four moves deep and judges the positions it reaches. It sees short combinations "
                                             "but misses longer plans."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Searches four moves deep and judges the positions it reaches. It sees short combinations "
                                             "but misses longer plans."),
         "alphabeta:depth=4",
         "alphabeta:depth=4"},

        {QT_TRANSLATE_NOOP("ComputerLevels", "Medium"),
         QT_TRANSLATE_NOOP("ComputerLevels", "Thinks eight moves ahead: rarely misses a tactic."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Searches eight moves deep and judges the positions it reaches. It finds and avoids almost every "
                                             "tactic, so you need a real plan to beat it."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Searches eight moves deep and judges the positions it reaches. It finds and avoids almost every "
                                             "tactic, so you need a real plan to beat it."),
         "alphabeta:depth=8",
         "alphabeta:depth=8"},

        {QT_TRANSLATE_NOOP("ComputerLevels", "Hard"),
         QT_TRANSLATE_NOOP("ComputerLevels", "A strong player: it hardly ever overlooks a threat."),
         QT_TRANSLATE_NOOP("ComputerLevels", "A neural network, trained by playing against itself, guides a search that simulates 100 games "
                                             "from every position. It may need a moment to answer."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Plays out 3000 simulated games from every position (a Monte Carlo tree search that takes wins and "
                                             "blocks threats in its simulations). It may need a moment to answer."),
         "az:model=connectx_6x7,sims=100",
         "mcts:sims=3000,rollout=tactical"},

        {QT_TRANSLATE_NOOP("ComputerLevels", "Expert"),
         QT_TRANSLATE_NOOP("ComputerLevels", "The strongest level: slow to answer, very accurate."),
         QT_TRANSLATE_NOOP("ComputerLevels", "The same neural network with a deeper search, 400 simulated games for every move: the strongest "
                                             "opponent there is on this board."),
         QT_TRANSLATE_NOOP("ComputerLevels", "Plays out 10000 simulated games from every position (a Monte Carlo tree search that takes wins and "
                                             "blocks threats in its simulations): the strongest opponent there is on this board. "
                                             "On large boards it can take several seconds per move."),
         "az:model=connectx_6x7,sims=400",
         "mcts:sims=10000,rollout=tactical"},
    };

    // Limits of the bitboard engine of the helper (ai/include/ai/position.hpp, Geometry::isSupported).
    constexpr int kMaxColumns = 16;
    constexpr int kMaxConnect = 16;
    constexpr int kMaxCells = 64; // columns * (rows + 1)

    QString tr(const char *text)
    {
        return QCoreApplication::translate("ComputerLevels", text);
    }

    const LevelDefinition &definition(int level)
    {
        return kLevels[ComputerLevels::isValid(level) ? level - 1 : ComputerLevels::kDefaultLevel - 1];
    }
}

namespace ComputerLevels
{
    bool isValid(int level)
    {
        return level >= 1 && level <= kCount;
    }

    QString name(int level)
    {
        return tr(definition(level).name);
    }

    QString summary(int level)
    {
        return tr(definition(level).summary);
    }

    bool isStandardBoard(const GameConfig &config)
    {
        return config.rows == 6 && config.cols == 7 && config.connect_length == 4 && config.num_players == 2;
    }

    QString details(int level, const GameConfig &config, bool networkAvailable)
    {
        const LevelDefinition &row = definition(level);
        return tr(isStandardBoard(config) && networkAvailable ? row.howStandard : row.howOther);
    }

    QString playerName(int level)
    {
        return QCoreApplication::translate("ComputerLevels", "Computer (%1)").arg(name(level));
    }

    QString agentSpec(int level, const GameConfig &config, bool networkAvailable)
    {
        const LevelDefinition &row = definition(level);
        return QString::fromLatin1(isStandardBoard(config) && networkAvailable ? row.specStandard : row.specOther);
    }

    bool isSupported(const GameConfig &config, QString *reason)
    {
        auto refuse = [reason](const QString &why)
        {
            if (reason != nullptr)
            {
                *reason = why;
            }
            return false;
        };

        const int rows = config.rows;
        const int cols = config.cols;
        const int connect = config.connect_length;

        if (config.num_players != 2)
        {
            return refuse(tr("The computer plays two-player games only."));
        }
        if (rows < 4 || cols < 4)
        {
            return refuse(tr("The computer needs a board of at least 4 × 4."));
        }
        if (connect < 3)
        {
            return refuse(tr("The computer needs a connect length of at least 3."));
        }
        if (connect > std::max(rows, cols))
        {
            return refuse(tr("The connect length does not fit on the board."));
        }
        if (connect > kMaxConnect)
        {
            return refuse(tr("The computer cannot play for more than %1 discs in a row.").arg(kMaxConnect));
        }
        if (cols > kMaxColumns || cols * (rows + 1) > kMaxCells)
        {
            return refuse(tr("The board is too large for the computer: columns × (rows + 1) must not exceed %1 "
                             "(6 × 7, 7 × 8 and 5 × 9 are fine, 8 × 8 is not).")
                              .arg(kMaxCells));
        }
        return true;
    }
}
