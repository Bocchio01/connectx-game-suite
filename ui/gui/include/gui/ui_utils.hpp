#pragma once

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QIcon>
#include <QPalette>

/** Small helpers shared by the pages. Colours are derived from the palette, so that dark mode just works. */
namespace UiUtils
{
    /** Copy of `base` scaled by `factor` (works for both point and pixel sized fonts). */
    QFont scaledFont(const QFont &base, qreal factor, bool bold = false);

    /** True if the palette has a dark window background. */
    bool isDark(const QPalette &palette);

    /** Colour for error messages, readable on both light and dark backgrounds. */
    QColor errorColor(const QPalette &palette);

    /** Colour for positive feedback (connected, you win...). */
    QColor successColor(const QPalette &palette);

    /** Colour for secondary text: halfway between the text and the window colour. */
    QColor mutedColor(const QPalette &palette);

    /**
     * Background of a CardFrame: "paper" (the base colour) on a light palette; on a dark one the window
     * colour, lifted a little, because there the base colour is darker than the window and is also what
     * the text fields are filled with, which would make them melt into the card.
     */
    QColor cardColor(const QPalette &palette);

    /** Application icon, painted at runtime (no image files needed). */
    QIcon appIcon();
}

/** A rounded panel that follows the palette (see UiUtils::cardColor, mid-tone border). */
class CardFrame : public QFrame
{
    Q_OBJECT

public:
    explicit CardFrame(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};
