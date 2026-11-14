#include <algorithm>
#include <cmath>

#include <QCursor>
#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "gui/board_widget.hpp"
#include "gui/disc_painter.hpp"
#include "gui/ui_utils.hpp"

namespace
{
    constexpr qreal kPi = 3.14159265358979323846;

    // All the sizes below are expressed in "cells" (one cell = one hole of the board).
    constexpr qreal kPad = 0.14;         // frame around the holes
    constexpr qreal kDropZone = 1.05;    // free space above the board where the ghost disc hovers
    constexpr qreal kNumbersBand = 0.62; // column numbers below the board
    constexpr qreal kHoleRadius = 0.42;

    constexpr qreal kMargin = 10.0; // pixels around everything
    constexpr qint64 kOverlayFadeMs = 260;
    constexpr qreal kPulsePeriodMs = 1300.0;

    // Fixed accent colours of the board (the page palette is only used for the surroundings).
    const QColor kFaceTop(0x3B, 0x75, 0xD9);
    const QColor kFaceBottom(0x1D, 0x46, 0x9B);
    const QColor kFaceEdge(0x14, 0x33, 0x73);
}

// ----------------------------------------------------------------------------
// Metrics
// ----------------------------------------------------------------------------

QPointF BoardWidget::Metrics::holeCenter(int row, int col) const
{
    return QPointF(board.left() + (kPad + col + 0.5) * cell, board.top() + (kPad + row + 0.5) * cell);
}

QPointF BoardWidget::Metrics::dropCenter(int col) const
{
    return QPointF(board.left() + (kPad + col + 0.5) * cell, board.top() - kDropZone * 0.5 * cell);
}

qreal BoardWidget::Metrics::dropTop() const
{
    return board.top() - kDropZone * cell;
}

qreal BoardWidget::Metrics::columnLeft(int col) const
{
    return board.left() + (kPad + col) * cell;
}

BoardWidget::Metrics BoardWidget::metrics() const
{
    Metrics m;
    if (rows_ <= 0 || cols_ <= 0)
    {
        return m;
    }

    const qreal availableW = std::max<qreal>(1.0, width() - 2.0 * kMargin);
    const qreal availableH = std::max<qreal>(1.0, height() - 2.0 * kMargin);
    const qreal unitsW = cols_ + 2.0 * kPad;
    const qreal unitsH = kDropZone + rows_ + 2.0 * kPad + kNumbersBand;

    // Everything scales with the cell size, so the aspect ratio of the board is preserved.
    const qreal cell = std::max<qreal>(4.0, std::min(availableW / unitsW, availableH / unitsH));
    const qreal totalW = cell * unitsW;
    const qreal totalH = cell * unitsH;

    // Align the cached board image to the device pixel grid (crisp on HiDPI screens).
    const qreal dpr = devicePixelRatioF();
    auto snap = [dpr](qreal v)
    { return std::round(v * dpr) / dpr; };

    const qreal left = snap((width() - totalW) / 2.0);
    const qreal top = snap((height() - totalH) / 2.0 + kDropZone * cell);

    m.valid = true;
    m.cell = cell;
    m.board = QRectF(left, top, totalW, cell * (rows_ + 2.0 * kPad));
    return m;
}

// ----------------------------------------------------------------------------
// Construction and state
// ----------------------------------------------------------------------------

BoardWidget::BoardWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Game board"));
    setAccessibleDescription(tr("Click a column, or press a number key, to drop a disc."));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    clock_.start();
    timer_.setInterval(16);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &BoardWidget::onTick);
}

void BoardWidget::reset(int rows, int cols)
{
    falling_.active = false;

    rows_ = std::max(0, rows);
    cols_ = std::max(0, cols);
    cells_ = GameLogic::makeEmptyBoard(rows_, cols_);

    lastMove_.reset();
    winningLines_.clear();
    winningCells_.clear();
    overlayTitle_.clear();
    overlaySubtitle_.clear();
    inputEnabled_ = false;
    hoverCol_ = -1;
    keyboardCol_ = -1;
    keyboardActive_ = false;

    updateCursor();
    updateGeometry();
    updateTimer();
    update();
}

void BoardWidget::setBoard(const GameLogic::Board &board, bool animate)
{
    if (board.empty() || board.front().empty())
    {
        return;
    }

    finishFalling(); // an animation still running is completed before the next one starts

    if (!GameLogic::hasShape(board, rows_, cols_))
    {
        // Unexpected size (should not happen): adopt it without animation.
        const int rows = static_cast<int>(board.size());
        const int cols = static_cast<int>(board.front().size());
        if (!GameLogic::hasShape(board, rows, cols))
        {
            return;
        }
        rows_ = rows;
        cols_ = cols;
        cells_ = board;
        lastMove_.reset();
        updateGeometry();
        update();
        return;
    }

    const auto changes = GameLogic::findNewDiscs(cells_, board);
    cells_ = board;

    if (!changes)
    {
        // Not an incremental change: show it as it is.
        lastMove_.reset();
    }
    else if (changes->size() == 1)
    {
        lastMove_ = changes->front().cell;
        if (animate)
        {
            startFalling(changes->front());
        }
    }
    else if (changes->size() > 1)
    {
        lastMove_.reset(); // several discs at once (e.g. first state): no animation, no marker
    }

    refreshHover();
    update();
}

void BoardWidget::setLocalPlayer(int playerId)
{
    localPlayer_ = playerId;
    update();
}

void BoardWidget::setInputEnabled(bool enabled)
{
    if (inputEnabled_ == enabled)
    {
        return;
    }

    inputEnabled_ = enabled;
    if (!enabled)
    {
        hoverCol_ = -1;
        keyboardActive_ = false;
    }
    else
    {
        refreshHover();
    }
    updateCursor();
    update();
}

void BoardWidget::setWinningLines(const std::vector<std::vector<GameLogic::Cell>> &lines)
{
    winningLines_ = lines;

    winningCells_.clear();
    for (const auto &line : winningLines_)
    {
        winningCells_.insert(winningCells_.end(), line.begin(), line.end());
    }
    std::sort(winningCells_.begin(), winningCells_.end());
    winningCells_.erase(std::unique(winningCells_.begin(), winningCells_.end()), winningCells_.end());

    updateTimer();
    update();
}

void BoardWidget::setOverlay(const QString &title, const QString &subtitle, OverlayStyle style)
{
    overlayTitle_ = title;
    overlaySubtitle_ = subtitle;
    overlayStyle_ = style;
    overlayStartMs_ = clock_.elapsed();
    updateTimer();
    update();
}

void BoardWidget::clearOverlay()
{
    overlayTitle_.clear();
    overlaySubtitle_.clear();
    updateTimer();
    update();
}

QPoint BoardWidget::columnPoint(int column) const
{
    const Metrics m = metrics();
    if (!m.valid)
    {
        return QPoint();
    }
    return m.dropCenter(column).toPoint();
}

QPoint BoardWidget::cellPoint(int row, int column) const
{
    const Metrics m = metrics();
    if (!m.valid)
    {
        return QPoint();
    }
    return m.holeCenter(row, column).toPoint();
}

qreal BoardWidget::cellSize() const
{
    return metrics().cell;
}

QSize BoardWidget::sizeHint() const
{
    const int rows = rows_ > 0 ? rows_ : 6;
    const int cols = cols_ > 0 ? cols_ : 7;
    const qreal cell = 56.0;
    return QSize(static_cast<int>(cell * (cols + 2.0 * kPad) + 2.0 * kMargin),
                 static_cast<int>(cell * (kDropZone + rows + 2.0 * kPad + kNumbersBand) + 2.0 * kMargin));
}

QSize BoardWidget::minimumSizeHint() const
{
    return QSize(300, 320);
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------

int BoardWidget::columnAt(const QPointF &pos) const
{
    const Metrics m = metrics();
    if (!m.valid)
    {
        return -1;
    }
    if (pos.x() < m.board.left() || pos.x() > m.board.right() || pos.y() < m.dropTop() || pos.y() > m.board.bottom())
    {
        return -1;
    }

    const int col = static_cast<int>(std::floor((pos.x() - m.board.left() - kPad * m.cell) / m.cell));
    return std::clamp(col, 0, cols_ - 1);
}

int BoardWidget::activeColumn() const
{
    if (!inputEnabled_ || cols_ <= 0)
    {
        return -1;
    }
    return keyboardActive_ ? keyboardCol_ : hoverCol_;
}

bool BoardWidget::columnHasRoom(int column) const
{
    return GameLogic::landingRow(cells_, column) >= 0;
}

void BoardWidget::activate(int column)
{
    if (!inputEnabled_ || column < 0 || column >= cols_ || !columnHasRoom(column))
    {
        return;
    }
    emit columnActivated(column);
}

void BoardWidget::moveKeyboardColumn(int delta)
{
    int start = keyboardCol_;
    if (!keyboardActive_)
    {
        start = hoverCol_ >= 0 ? hoverCol_ : (cols_ - 1) / 2;
    }
    keyboardCol_ = std::clamp(start + delta, 0, cols_ - 1);
    keyboardActive_ = true;
    updateCursor();
    update();
}

void BoardWidget::refreshHover()
{
    if (inputEnabled_ && underMouse())
    {
        hoverCol_ = columnAt(mapFromGlobal(QCursor::pos()));
    }
    else
    {
        hoverCol_ = -1;
    }
    updateCursor();
}

void BoardWidget::updateCursor()
{
    const int column = activeColumn();
    if (column < 0)
    {
        setCursor(Qt::ArrowCursor);
    }
    else
    {
        setCursor(columnHasRoom(column) ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
    }
}

void BoardWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int column = inputEnabled_ ? columnAt(event->position()) : -1;
    if (column != hoverCol_)
    {
        hoverCol_ = column;
        keyboardActive_ = false; // the mouse takes over from the keyboard cursor
        updateCursor();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void BoardWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        setFocus(Qt::MouseFocusReason);
        if (inputEnabled_)
        {
            const int column = columnAt(event->position());
            if (column >= 0)
            {
                hoverCol_ = column;
                keyboardActive_ = false;
                activate(column);
                event->accept();
                return;
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void BoardWidget::leaveEvent(QEvent *event)
{
    if (hoverCol_ != -1)
    {
        hoverCol_ = -1;
        updateCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

void BoardWidget::keyPressEvent(QKeyEvent *event)
{
    if (!inputEnabled_ || cols_ <= 0)
    {
        QWidget::keyPressEvent(event);
        return;
    }

    const int key = event->key();

    // Number keys 1..9 drop a disc straight away.
    if (key >= Qt::Key_1 && key <= Qt::Key_9)
    {
        const int column = key - Qt::Key_1;
        if (column < cols_)
        {
            keyboardCol_ = column;
            keyboardActive_ = true;
            activate(column);
        }
        event->accept();
        return;
    }

    switch (key)
    {
    case Qt::Key_Left:
        moveKeyboardColumn(-1);
        break;
    case Qt::Key_Right:
        moveKeyboardColumn(+1);
        break;
    case Qt::Key_Home:
        moveKeyboardColumn(-cols_);
        break;
    case Qt::Key_End:
        moveKeyboardColumn(+cols_);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
    {
        const int column = keyboardActive_ ? keyboardCol_ : hoverCol_;
        if (column >= 0)
        {
            activate(column);
        }
        else
        {
            moveKeyboardColumn(0); // first press only shows the cursor
        }
        break;
    }
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    event->accept();
}

void BoardWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refreshHover();
}

void BoardWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::EnabledChange)
    {
        update();
    }
}

// ----------------------------------------------------------------------------
// Animation
// ----------------------------------------------------------------------------

void BoardWidget::startFalling(const GameLogic::DiscChange &change)
{
    falling_.active = true;
    falling_.cell = change.cell;
    falling_.player = change.player;
    falling_.startMs = clock_.elapsed();

    // Travel from the drop zone to the hole, in cells: longer falls take longer, but not linearly.
    const qreal distance = kDropZone * 0.5 + kPad + change.cell.row + 0.5;
    falling_.durationMs = 230.0 + 115.0 * std::sqrt(distance);

    updateTimer();
}

void BoardWidget::finishFalling()
{
    if (!falling_.active)
    {
        return;
    }

    falling_.active = false;
    if (!overlayTitle_.isEmpty())
    {
        overlayStartMs_ = clock_.elapsed(); // banners wait for the landing, then fade in
    }
    updateTimer();
    update();
}

void BoardWidget::onTick()
{
    const qint64 now = clock_.elapsed();
    if (falling_.active && now >= falling_.startMs + static_cast<qint64>(falling_.durationMs))
    {
        finishFalling();
    }
    update();
    updateTimer();
}

void BoardWidget::updateTimer()
{
    const qint64 now = clock_.elapsed();
    const bool fading = overlayVisible() && (now - overlayStartMs_) < kOverlayFadeMs + 40;
    const bool needed = falling_.active || winningVisible() || fading;

    if (needed && !timer_.isActive())
    {
        timer_.start();
    }
    else if (!needed && timer_.isActive())
    {
        timer_.stop();
    }
}

bool BoardWidget::winningVisible() const
{
    return !winningCells_.empty() && !falling_.active;
}

bool BoardWidget::overlayVisible() const
{
    if (overlayTitle_.isEmpty())
    {
        return false;
    }
    return !(overlayStyle_ == OverlayStyle::Banner && falling_.active);
}

qreal BoardWidget::fallY(const Metrics &m, qint64 now) const
{
    const qreal startY = m.dropCenter(falling_.cell.col).y();
    const qreal endY = m.holeCenter(falling_.cell.row, falling_.cell.col).y();
    const qreal distance = endY - startY;
    const qreal t = std::clamp(static_cast<qreal>(now - falling_.startMs) / falling_.durationMs, 0.0, 1.0);

    // Gravity (quadratic ease-in) until the disc hits the stack, then two damped bounces.
    constexpr qreal impact = 0.72;
    constexpr qreal secondBounce = 0.90;
    const qreal bounce = std::min(distance * 0.10, m.cell * 0.30);

    if (t < impact)
    {
        const qreal u = t / impact;
        return startY + distance * u * u;
    }
    if (t < secondBounce)
    {
        const qreal u = (t - impact) / (secondBounce - impact);
        return endY - bounce * std::sin(u * kPi);
    }
    const qreal u = (t - secondBounce) / (1.0 - secondBounce);
    return endY - bounce * 0.28 * std::sin(u * kPi);
}

// ----------------------------------------------------------------------------
// Painting
// ----------------------------------------------------------------------------

void BoardWidget::ensureFaceCache(const Metrics &m)
{
    const qreal dpr = devicePixelRatioF();
    const QSize size(static_cast<int>(std::ceil(m.board.width() * dpr)),
                     static_cast<int>(std::ceil(m.board.height() * dpr)));

    if (!face_.isNull() && size == faceSize_ && dpr == faceDpr_ && rows_ == faceRows_ && cols_ == faceCols_)
    {
        return;
    }

    faceSize_ = size;
    faceDpr_ = dpr;
    faceRows_ = rows_;
    faceCols_ = cols_;

    QPixmap pixmap(size);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal cell = m.cell;
    const QRectF local(0.0, 0.0, size.width() / dpr, size.height() / dpr);
    const qreal holeR = kHoleRadius * cell;
    const qreal edgeWidth = std::max<qreal>(1.0, cell * 0.035);

    // The face is one path with the holes as extra sub-paths (odd-even fill): real see-through holes.
    // The outline is centred on the path: keep it inside the image, or half of it would be clipped.
    QPainterPath face;
    face.setFillRule(Qt::OddEvenFill);
    face.addRoundedRect(local.adjusted(edgeWidth / 2.0, edgeWidth / 2.0, -edgeWidth / 2.0, -edgeWidth / 2.0), cell * 0.28, cell * 0.28);
    for (int r = 0; r < rows_; ++r)
    {
        for (int c = 0; c < cols_; ++c)
        {
            face.addEllipse(QPointF((kPad + c + 0.5) * cell, (kPad + r + 0.5) * cell), holeR, holeR);
        }
    }

    QLinearGradient gradient(local.topLeft(), local.bottomLeft());
    gradient.setColorAt(0.0, kFaceTop);
    gradient.setColorAt(1.0, kFaceBottom);
    p.setPen(QPen(kFaceEdge, edgeWidth));
    p.setBrush(gradient);
    p.drawPath(face);

    // Rim around every hole: a dark edge and a soft light ring give the moulded look.
    p.setBrush(Qt::NoBrush);
    for (int r = 0; r < rows_; ++r)
    {
        for (int c = 0; c < cols_; ++c)
        {
            const QPointF center((kPad + c + 0.5) * cell, (kPad + r + 0.5) * cell);
            p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF, 34), cell * 0.035));
            p.drawEllipse(center, holeR + cell * 0.06, holeR + cell * 0.06);
            p.setPen(QPen(QColor(0x0E, 0x25, 0x5A, 190), std::max<qreal>(1.0, cell * 0.035)));
            p.drawEllipse(center, holeR + cell * 0.012, holeR + cell * 0.012);
        }
    }

    p.end();
    face_ = pixmap;
}

void BoardWidget::paintColumnHighlight(QPainter &p, const Metrics &m, int column) const
{
    QColor color = palette().color(QPalette::Highlight);
    color.setAlpha(UiUtils::isDark(palette()) ? 80 : 60);

    const qreal top = m.dropTop() + m.cell * 0.05;
    const QRectF rect(m.columnLeft(column) + m.cell * 0.04, top, m.cell * 0.92, m.board.bottom() - top - m.cell * 0.04);

    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(rect, m.cell * 0.3, m.cell * 0.3);
}

void BoardWidget::paintDiscs(QPainter &p, const Metrics &m, qint64 now) const
{
    // Discs are slightly larger than the holes: their edge hides behind the face (no seam).
    const qreal discRadius = kHoleRadius * m.cell + 0.6;
    const bool dimOthers = winningVisible();

    for (int r = 0; r < rows_; ++r)
    {
        for (int c = 0; c < cols_; ++c)
        {
            const int player = cells_[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (player == 0)
            {
                continue;
            }
            if (falling_.active && falling_.cell.row == r && falling_.cell.col == c)
            {
                continue; // drawn below, while it falls
            }

            const bool winning = dimOthers && std::binary_search(winningCells_.begin(), winningCells_.end(), GameLogic::Cell{r, c});
            DiscPainter::paintDisc(p, m.holeCenter(r, c), discRadius, player, dimOthers && !winning ? 0.30 : 1.0);
        }
    }

    // Faint preview of where the disc would land.
    const int active = activeColumn();
    if (active >= 0 && !falling_.active)
    {
        const int row = GameLogic::landingRow(cells_, active);
        if (row >= 0)
        {
            DiscPainter::paintDisc(p, m.holeCenter(row, active), discRadius, localPlayer_, 0.26);
        }
    }

    if (falling_.active)
    {
        const QPointF target = m.holeCenter(falling_.cell.row, falling_.cell.col);
        DiscPainter::paintDisc(p, QPointF(target.x(), fallY(m, now)), discRadius, falling_.player);
    }
}

void BoardWidget::paintMarkers(QPainter &p, const Metrics &m, qint64 now) const
{
    const qreal holeR = kHoleRadius * m.cell;

    p.setBrush(Qt::NoBrush);

    if (winningVisible())
    {
        const qreal pulse = 0.5 + 0.5 * std::sin(2.0 * kPi * static_cast<qreal>(now) / kPulsePeriodMs);

        QPen line(QColor(0xFF, 0xFF, 0xFF, static_cast<int>(110 + 70 * pulse)), m.cell * 0.07, Qt::SolidLine, Qt::RoundCap);
        p.setPen(line);
        for (const auto &run : winningLines_)
        {
            if (run.size() >= 2)
            {
                p.drawLine(m.holeCenter(run.front().row, run.front().col), m.holeCenter(run.back().row, run.back().col));
            }
        }

        p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF, static_cast<int>(175 + 80 * pulse)), m.cell * (0.05 + 0.025 * pulse)));
        for (const GameLogic::Cell &cell : winningCells_)
        {
            p.drawEllipse(m.holeCenter(cell.row, cell.col), holeR + m.cell * 0.045, holeR + m.cell * 0.045);
        }
    }
    else if (lastMove_.has_value() && !falling_.active)
    {
        p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF, 215), std::max<qreal>(1.5, m.cell * 0.045)));
        p.drawEllipse(m.holeCenter(lastMove_->row, lastMove_->col), holeR + m.cell * 0.035, holeR + m.cell * 0.035);
    }
}

void BoardWidget::paintNumbers(QPainter &p, const Metrics &m, int active) const
{
    if (m.cell < 22.0)
    {
        return;
    }

    QFont normal = font();
    normal.setPixelSize(std::clamp(static_cast<int>(m.cell * 0.32), 9, 18));
    QFont bold = normal;
    bold.setBold(true);

    const QColor muted = UiUtils::mutedColor(palette());
    const QColor strong = palette().color(QPalette::WindowText);

    for (int c = 0; c < cols_; ++c)
    {
        const bool isActive = (c == active);
        p.setFont(isActive ? bold : normal);
        p.setPen(isActive ? strong : muted);
        const QRectF rect(m.columnLeft(c), m.board.bottom() + m.cell * 0.04, m.cell, kNumbersBand * m.cell);
        p.drawText(rect, Qt::AlignHCenter | Qt::AlignTop, QString::number(c + 1));
    }
}

void BoardWidget::paintOverlay(QPainter &p, const Metrics &m, qint64 now) const
{
    if (!overlayVisible())
    {
        return;
    }

    const qreal progress = std::clamp(static_cast<qreal>(now - overlayStartMs_) / static_cast<qreal>(kOverlayFadeMs), 0.0, 1.0);
    const qreal opacity = progress * progress * (3.0 - 2.0 * progress);

    const qreal maxWidth = std::max<qreal>(80.0, width() - 2.0 * kMargin - 16.0);
    int titlePx = std::clamp(static_cast<int>(m.cell * 0.44), 16, 40);
    int subtitlePx = std::clamp(static_cast<int>(m.cell * 0.24), 11, 18);

    QFont titleFont = font();
    titleFont.setBold(true);
    QFont subtitleFont = font();

    qreal padX = 0.0;
    qreal padY = 0.0;
    qreal textWidth = 0.0;
    QFontMetricsF titleMetrics(titleFont);
    QFontMetricsF subtitleMetrics(subtitleFont);

    // Shrink the text until the panel fits in the widget.
    for (;;)
    {
        titleFont.setPixelSize(titlePx);
        subtitleFont.setPixelSize(subtitlePx);
        titleMetrics = QFontMetricsF(titleFont);
        subtitleMetrics = QFontMetricsF(subtitleFont);

        textWidth = titleMetrics.horizontalAdvance(overlayTitle_);
        if (!overlaySubtitle_.isEmpty())
        {
            textWidth = std::max(textWidth, subtitleMetrics.horizontalAdvance(overlaySubtitle_));
        }
        padX = std::max<qreal>(16.0, titlePx * 0.85);
        padY = std::max<qreal>(8.0, titlePx * 0.30);

        if (textWidth + 2.0 * padX <= maxWidth || titlePx <= 10)
        {
            break;
        }
        titlePx = std::max(10, titlePx - 2);
        subtitlePx = std::max(8, subtitlePx - 1);
    }

    const qreal subtitleHeight = overlaySubtitle_.isEmpty() ? 0.0 : subtitleMetrics.height() * 1.05;
    const qreal panelWidth = textWidth + 2.0 * padX;
    const qreal panelHeight = titleMetrics.height() + subtitleHeight + 2.0 * padY;

    QRectF panel(0.0, 0.0, panelWidth, panelHeight);
    if (overlayStyle_ == OverlayStyle::Centered)
    {
        panel.moveCenter(m.board.center());
    }
    else
    {
        panel.moveCenter(QPointF(m.board.center().x(), m.board.top() - kDropZone * 0.5 * m.cell));
        if (panel.top() < 4.0)
        {
            panel.moveTop(4.0);
        }
    }

    const qreal radius = std::min(panelHeight * 0.28, 22.0);

    p.save();
    p.setOpacity(opacity);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawRoundedRect(panel.translated(0.0, 3.0), radius, radius);
    p.setBrush(QColor(0x11, 0x16, 0x23, 232));
    p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF, 50), 1.0));
    p.drawRoundedRect(panel, radius, radius);

    p.setPen(QColor(0xFF, 0xFF, 0xFF));
    p.setFont(titleFont);
    p.drawText(QRectF(panel.left(), panel.top() + padY, panelWidth, titleMetrics.height()), Qt::AlignHCenter | Qt::AlignVCenter, overlayTitle_);

    if (!overlaySubtitle_.isEmpty())
    {
        p.setPen(QColor(0xFF, 0xFF, 0xFF, 190));
        p.setFont(subtitleFont);
        p.drawText(QRectF(panel.left(), panel.top() + padY + titleMetrics.height(), panelWidth, subtitleHeight), Qt::AlignHCenter | Qt::AlignVCenter, overlaySubtitle_);
    }
    p.restore();
}

void BoardWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const Metrics m = metrics();
    if (!m.valid)
    {
        return; // nothing to show before the first reset()
    }

    ensureFaceCache(m);

    const qint64 now = clock_.elapsed();
    const int active = activeColumn();

    // Back to front: column highlight, discs (behind the face), the face with its holes, markers, ghost, banner.
    if (active >= 0)
    {
        paintColumnHighlight(p, m, active);
    }
    paintDiscs(p, m, now);
    p.drawPixmap(m.board.topLeft(), face_);
    paintMarkers(p, m, now);
    paintNumbers(p, m, active);

    if (active >= 0 && !falling_.active && columnHasRoom(active))
    {
        DiscPainter::paintDisc(p, m.dropCenter(active), kHoleRadius * m.cell + 0.6, localPlayer_, 0.62);
    }

    paintOverlay(p, m, now);
}
