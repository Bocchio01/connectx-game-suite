#include <array>
#include <cmath>

#include <QCoreApplication>
#include <QPolygonF>
#include <QTransform>

#include "gui/player_style.hpp"

namespace
{
    constexpr qreal kPi = 3.14159265358979323846;

    // Colours chosen to stay distinguishable on the blue board and for the most common
    // forms of colour blindness; the symbol is the redundant second cue.
    const std::array<PlayerStyle, PlayerStyles::kCount> &styles()
    {
        static const std::array<PlayerStyle, PlayerStyles::kCount> table = {{
            {QColor(0xE5, 0x48, 0x4D), QColor(0xFF, 0xFF, 0xFF), PlayerSymbol::Dot,
             QCoreApplication::translate("PlayerStyles", "Red"),
             QCoreApplication::translate("PlayerStyles", "dot")},
            {QColor(0xF7, 0xC9, 0x48), QColor(0x4A, 0x3A, 0x00), PlayerSymbol::Cross,
             QCoreApplication::translate("PlayerStyles", "Yellow"),
             QCoreApplication::translate("PlayerStyles", "cross")},
            {QColor(0x2C, 0xC4, 0xD9), QColor(0x06, 0x32, 0x3A), PlayerSymbol::Diamond,
             QCoreApplication::translate("PlayerStyles", "Cyan"),
             QCoreApplication::translate("PlayerStyles", "diamond")},
            {QColor(0x9B, 0x5D, 0xE5), QColor(0xFF, 0xFF, 0xFF), PlayerSymbol::Triangle,
             QCoreApplication::translate("PlayerStyles", "Violet"),
             QCoreApplication::translate("PlayerStyles", "triangle")},
        }};
        return table;
    }
}

namespace PlayerStyles
{
    const PlayerStyle &forPlayer(int playerId)
    {
        const int index = playerId <= 0 ? 0 : (playerId - 1) % kCount;
        return styles()[static_cast<std::size_t>(index)];
    }

    QString describe(int playerId)
    {
        const PlayerStyle &style = forPlayer(playerId);
        return QCoreApplication::translate("PlayerStyles", "%1 %2").arg(style.colorName, style.symbolName);
    }

    QPainterPath symbolPath(PlayerSymbol symbol, const QPointF &center, qreal radius)
    {
        QPainterPath path;

        switch (symbol)
        {
        case PlayerSymbol::Dot:
        {
            path.addEllipse(center, radius * 0.92, radius * 0.92);
            break;
        }

        case PlayerSymbol::Cross:
        {
            const qreal arm = radius * 1.05;
            const qreal half = radius * 0.34;
            QPainterPath bar;
            bar.addRoundedRect(QRectF(center.x() - arm, center.y() - half, arm * 2.0, half * 2.0), half * 0.55, half * 0.55);

            QTransform rotatePlus;
            rotatePlus.translate(center.x(), center.y());
            rotatePlus.rotate(45.0);
            rotatePlus.translate(-center.x(), -center.y());

            QTransform rotateMinus;
            rotateMinus.translate(center.x(), center.y());
            rotateMinus.rotate(-45.0);
            rotateMinus.translate(-center.x(), -center.y());

            path = rotatePlus.map(bar).united(rotateMinus.map(bar));
            break;
        }

        case PlayerSymbol::Diamond:
        {
            const qreal r = radius * 1.12;
            QPolygonF diamond;
            diamond << QPointF(center.x(), center.y() - r)
                    << QPointF(center.x() + r, center.y())
                    << QPointF(center.x(), center.y() + r)
                    << QPointF(center.x() - r, center.y());
            path.addPolygon(diamond);
            path.closeSubpath();
            break;
        }

        case PlayerSymbol::Triangle:
        {
            // Equilateral triangle with its centroid on `center`.
            const qreal r = radius * 1.18;
            QPolygonF triangle;
            for (int i = 0; i < 3; ++i)
            {
                const qreal angle = (-90.0 + 120.0 * i) * kPi / 180.0;
                triangle << QPointF(center.x() + r * std::cos(angle), center.y() + r * std::sin(angle));
            }
            path.addPolygon(triangle);
            path.closeSubpath();
            break;
        }
        }

        return path;
    }
}
