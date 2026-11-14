#pragma once

#include <QString>

/**
 * Where the helper programs of the suite are: `connectx-ai` (the computer player) and `connectx-server`
 * (a private server for offline games). They are separate programs that the GUI runs as child processes,
 * exactly as a user would from a terminal: the GUI does not link the AI library. Both are optional; what
 * needs a missing helper is disabled, with a tooltip that says so.
 */
struct HelperPrograms
{
    QString computerPlayer; // full path of connectx-ai, empty if it was not found
    QString server;         // full path of connectx-server, empty if it was not found
    bool network = false;   // the pretrained network (connectx_6x7) is available to the computer player

    bool hasComputerPlayer() const { return !computerPlayer.isEmpty(); }
    bool hasServer() const { return !server.isEmpty(); }

    /** Offline games need both: a private server and somebody to play against. */
    bool canPlayOffline() const { return hasComputerPlayer() && hasServer(); }

    /** Looks for the programs next to the GUI executable first, then on the PATH. */
    static HelperPrograms locate();

    /**
     * Full path of the program called `baseName` ("connectx-ai", no extension), or an empty string:
     * `directory` is searched first (default: the folder of the running executable), then the PATH.
     */
    static QString find(const QString &baseName, const QString &directory = QString());

    /**
     * Whether the computer player at `computerPlayer` finds the pretrained network: it looks in the
     * `models` folder next to itself, then in `../models`, next to itself, in `models` of the current
     * folder and in the folders listed in CONNECTX_MODEL_PATH (see ai/src/model_cache.cpp).
     */
    static bool findsNetwork(const QString &computerPlayer);
};
