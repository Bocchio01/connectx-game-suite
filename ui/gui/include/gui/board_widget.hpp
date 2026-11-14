#pragma once

#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "gui/game_logic.hpp"

/**
 * Custom painted game board for any size (4..20 x 4..20) and up to 4 players.
 *
 * The board scales with the widget keeping its aspect ratio. Discs sit behind the board face,
 * so that the holes really are holes and falling discs slide behind the frame. Features:
 * hover/keyboard "ghost" disc, drop animation of the newly appeared discs only, last move
 * marker, winning line highlight and a banner overlay (waiting / result).
 *
 * The widget never talks to the network: it emits columnActivated() and the owner decides.
 */
class BoardWidget : public QWidget
{
    Q_OBJECT

public:
    enum class OverlayStyle
    {
        Centered, // card in the middle of the board (e.g. waiting for players)
        Banner    // ribbon above the board (e.g. the result)
    };

    explicit BoardWidget(QWidget *parent = nullptr);

    /** Starts a new, empty board; forgets discs, markers, highlights and overlays. */
    void reset(int rows, int cols);

    /**
     * Shows a new state. The difference from the board currently shown decides what is animated:
     * exactly one new disc falls and becomes the "last move"; anything else (first state after
     * reset(), several new discs, a different shape) is shown at once. `animate = false` never animates.
     */
    void setBoard(const GameLogic::Board &board, bool animate = true);

    /** Player whose colour the ghost disc uses (the local player). */
    void setLocalPlayer(int playerId);

    /** Enables hover preview, clicks and keys (it is the local player's turn). */
    void setInputEnabled(bool enabled);

    /** Highlights winning runs (shown once the last disc has landed); empty to clear. */
    void setWinningLines(const std::vector<std::vector<GameLogic::Cell>> &lines);

    void setOverlay(const QString &title, const QString &subtitle = QString(), OverlayStyle style = OverlayStyle::Banner);
    void clearOverlay();

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    const GameLogic::Board &board() const { return cells_; }
    bool isInputEnabled() const { return inputEnabled_; }
    bool isAnimating() const { return falling_.active; }
    std::optional<GameLogic::Cell> lastMove() const { return lastMove_; }
    bool hasWinningLine() const { return !winningLines_.empty(); }

    /** Centre of the drop zone above a column, in widget coordinates (a point that clicks that column). */
    QPoint columnPoint(int column) const;

    /** Centre of a hole (row 0 is the top row), in widget coordinates. */
    QPoint cellPoint(int row, int column) const;

    /** Size of one cell in pixels (0 before the first reset()). */
    qreal cellSize() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /** The user chose a column (mouse click or keyboard); only emitted while input is enabled. */
    void columnActivated(int column);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    /** Geometry derived from the widget size and the board shape. */
    struct Metrics
    {
        bool valid = false;
        qreal cell = 0.0;
        QRectF board; // the face of the board, holes included

        QPointF holeCenter(int row, int col) const;
        QPointF dropCenter(int col) const;
        qreal dropTop() const;
        qreal columnLeft(int col) const;
    };

    struct Falling
    {
        bool active = false;
        GameLogic::Cell cell;
        int player = 0;
        qint64 startMs = 0;
        qreal durationMs = 0.0;
    };

    Metrics metrics() const;
    int columnAt(const QPointF &pos) const;
    int activeColumn() const;
    bool columnHasRoom(int column) const;
    void activate(int column);
    void moveKeyboardColumn(int delta);
    void refreshHover();
    void updateCursor();

    void startFalling(const GameLogic::DiscChange &change);
    void finishFalling();
    void onTick();
    void updateTimer();
    bool winningVisible() const;
    bool overlayVisible() const;
    qreal fallY(const Metrics &m, qint64 now) const;

    void ensureFaceCache(const Metrics &m);
    void paintColumnHighlight(QPainter &p, const Metrics &m, int column) const;
    void paintDiscs(QPainter &p, const Metrics &m, qint64 now) const;
    void paintMarkers(QPainter &p, const Metrics &m, qint64 now) const;
    void paintNumbers(QPainter &p, const Metrics &m, int active) const;
    void paintOverlay(QPainter &p, const Metrics &m, qint64 now) const;

    int rows_ = 0;
    int cols_ = 0;
    GameLogic::Board cells_;
    int localPlayer_ = 1;
    bool inputEnabled_ = false;

    int hoverCol_ = -1;
    int keyboardCol_ = -1;
    bool keyboardActive_ = false;

    std::optional<GameLogic::Cell> lastMove_;
    std::vector<std::vector<GameLogic::Cell>> winningLines_;
    std::vector<GameLogic::Cell> winningCells_;

    Falling falling_;
    QElapsedTimer clock_;
    QTimer timer_;

    QString overlayTitle_;
    QString overlaySubtitle_;
    OverlayStyle overlayStyle_ = OverlayStyle::Banner;
    qint64 overlayStartMs_ = 0;

    // Cached image of the board face (rebuilt on resize / DPI / shape change).
    QPixmap face_;
    QSize faceSize_;
    qreal faceDpr_ = 0.0;
    int faceRows_ = 0;
    int faceCols_ = 0;
};
