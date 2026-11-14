#include <algorithm>

#include <QBrush>
#include <QPainter>
#include <QPen>
#include <QRadialGradient>

#include "gui/disc_painter.hpp"
#include "gui/player_style.hpp"

namespace DiscPainter
{
    void paintDisc(QPainter &painter, const QPointF &center, qreal radius, int playerId, qreal opacity)
    {
        if (radius < 1.0 || opacity <= 0.0)
        {
            return;
        }

        const PlayerStyle &style = PlayerStyles::forPlayer(playerId);
        const qreal outline = std::max<qreal>(1.0, radius * 0.07);

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(painter.opacity() * opacity);

        // Body: soft radial gradient with the highlight towards the top-left corner.
        QRadialGradient body(center + QPointF(-radius * 0.28, -radius * 0.34), radius * 1.45);
        body.setColorAt(0.0, style.color.lighter(128));
        body.setColorAt(0.55, style.color);
        body.setColorAt(1.0, style.color.darker(122));
        painter.setPen(QPen(style.color.darker(150), outline));
        painter.setBrush(body);
        painter.drawEllipse(center, radius, radius);

        // Inner ring, like the moulded plastic discs of the real game.
        if (radius >= 8.0)
        {
            painter.setPen(QPen(style.color.darker(120), outline));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius * 0.76, radius * 0.76);
        }

        // Redundant (colour independent) cue.
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.symbolColor);
        painter.drawPath(PlayerStyles::symbolPath(style.symbol, center, radius * 0.38));

        painter.restore();
    }
}
