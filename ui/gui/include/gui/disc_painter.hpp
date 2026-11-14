#pragma once

#include <QPointF>

class QPainter;

/** Shared drawing code for discs, used by the board and by the player swatches. */
namespace DiscPainter
{
    /**
     * Paints the disc of a player: body, inner ring and the player's symbol.
     * @param opacity multiplied with the current painter opacity (e.g. for ghost discs)
     */
    void paintDisc(QPainter &painter, const QPointF &center, qreal radius, int playerId, qreal opacity = 1.0);
}
