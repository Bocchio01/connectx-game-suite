#pragma once

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QString>

/** Symbol drawn on a disc, so that players can be told apart without relying on colour only. */
enum class PlayerSymbol
{
    Dot,
    Cross,
    Diamond,
    Triangle
};

/** Visual identity of a player: one colour AND one symbol. */
struct PlayerStyle
{
    QColor color;       // disc colour
    QColor symbolColor; // colour of the symbol (high contrast against `color`)
    PlayerSymbol symbol;
    QString colorName;  // translated, e.g. "Red"
    QString symbolName; // translated, e.g. "dot"
};

namespace PlayerStyles
{
    /** Number of distinct styles (protocol maximum is 4 players); higher ids wrap around. */
    constexpr int kCount = 4;

    /** Style of a player id (1-based). Ids <= 0 map to the first style. */
    const PlayerStyle &forPlayer(int playerId);

    /** Human readable description, e.g. "Red dot" (used for tooltips and accessibility). */
    QString describe(int playerId);

    /** Filled outline of a symbol centred in `center`; `radius` is half of its bounding box. */
    QPainterPath symbolPath(PlayerSymbol symbol, const QPointF &center, qreal radius);
}
