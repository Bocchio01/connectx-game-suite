#include <algorithm>

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include "gui/ui_utils.hpp"

namespace UiUtils
{
    QFont scaledFont(const QFont &base, qreal factor, bool bold)
    {
        QFont font(base);
        if (font.pointSizeF() > 0)
        {
            font.setPointSizeF(font.pointSizeF() * factor);
        }
        else if (font.pixelSize() > 0)
        {
            font.setPixelSize(static_cast<int>(font.pixelSize() * factor + 0.5));
        }
        font.setBold(bold);
        return font;
    }

    bool isDark(const QPalette &palette)
    {
        return palette.color(QPalette::Window).lightness() < 128;
    }

    QColor errorColor(const QPalette &palette)
    {
        return isDark(palette) ? QColor(0xFF, 0x7B, 0x7B) : QColor(0xC6, 0x28, 0x28);
    }

    QColor successColor(const QPalette &palette)
    {
        return isDark(palette) ? QColor(0x5F, 0xD3, 0x8D) : QColor(0x1E, 0x7B, 0x45);
    }

    QColor mutedColor(const QPalette &palette)
    {
        const QColor text = palette.color(QPalette::WindowText);
        const QColor window = palette.color(QPalette::Window);
        auto mix = [](int a, int b)
        { return (a * 62 + b * 38) / 100; };
        return QColor(mix(text.red(), window.red()), mix(text.green(), window.green()), mix(text.blue(), window.blue()));
    }

    QColor cardColor(const QPalette &palette)
    {
        return isDark(palette) ? palette.color(QPalette::Window).lighter(112) : palette.color(QPalette::Base);
    }

    namespace
    {
        QPixmap paintIcon(int size)
        {
            QPixmap pixmap(size, size);
            pixmap.fill(Qt::transparent);

            QPainter p(&pixmap);
            p.setRenderHint(QPainter::Antialiasing, true);

            const qreal s = size;
            const QRectF board(s * 0.04, s * 0.04, s * 0.92, s * 0.92);

            // 4x4 holes: 0 = empty, 1 = red disc, 2 = yellow disc (a red diagonal wins).
            static const int cells[4][4] = {
                {0, 0, 0, 1},
                {0, 0, 1, 2},
                {0, 1, 2, 2},
                {1, 2, 1, 2},
            };
            const qreal pad = board.width() * 0.08;
            const qreal cell = (board.width() - 2.0 * pad) / 4.0;
            const qreal hole = cell * 0.40;

            QPainterPath face;
            face.setFillRule(Qt::OddEvenFill);
            face.addRoundedRect(board, s * 0.14, s * 0.14);
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    face.addEllipse(QPointF(board.left() + pad + (c + 0.5) * cell, board.top() + pad + (r + 0.5) * cell), hole, hole);
                }
            }

            // Discs first (they sit behind the board), then the board face with its holes.
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    if (cells[r][c] == 0)
                    {
                        continue;
                    }
                    p.setPen(Qt::NoPen);
                    p.setBrush(cells[r][c] == 1 ? QColor(0xE5, 0x48, 0x4D) : QColor(0xF7, 0xC9, 0x48));
                    p.drawEllipse(QPointF(board.left() + pad + (c + 0.5) * cell, board.top() + pad + (r + 0.5) * cell), hole + 0.6, hole + 0.6);
                }
            }

            QLinearGradient gradient(board.topLeft(), board.bottomLeft());
            gradient.setColorAt(0.0, QColor(0x3A, 0x72, 0xD8));
            gradient.setColorAt(1.0, QColor(0x1F, 0x4A, 0xA0));
            p.setPen(QPen(QColor(0x16, 0x3A, 0x80), std::max<qreal>(1.0, s * 0.02)));
            p.setBrush(gradient);
            p.drawPath(face);

            return pixmap;
        }
    }

    QIcon appIcon()
    {
        QIcon icon;
        for (int size : {16, 24, 32, 48, 64, 128, 256})
        {
            icon.addPixmap(paintIcon(size));
        }
        return icon;
    }
}

CardFrame::CardFrame(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setContentsMargins(18, 16, 18, 16);
}

void CardFrame::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QColor border = palette().color(QPalette::Mid);
    border.setAlpha(150);

    p.setPen(QPen(border, 1.0));
    p.setBrush(UiUtils::cardColor(palette()));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10.0, 10.0);
}
