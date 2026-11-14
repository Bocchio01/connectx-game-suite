#pragma once

#include <QMetaType>

#include "protocol/protocol.hpp"

/**
 * Qt meta-type declarations of the protocol structs that travel in signals.
 *
 * Every header that uses one of these types in a signal or slot must include this file (and
 * not declare the types itself): all the moc outputs are compiled in one translation unit, and
 * Q_DECLARE_METATYPE must come before the first use of the type.
 */
Q_DECLARE_METATYPE(GameInfo)
Q_DECLARE_METATYPE(GameConfig)
Q_DECLARE_METATYPE(GameStateUpdate)
