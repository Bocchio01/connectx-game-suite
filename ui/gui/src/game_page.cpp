#include <algorithm>

#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include "gui/board_widget.hpp"
#include "gui/disc_painter.hpp"
#include "gui/game_page.hpp"
#include "gui/player_style.hpp"
#include "gui/ui_utils.hpp"

namespace
{
    /**
     * A fixed-width font for the output of the computer player. The family that the platform calls "fixed"
     * is not always a sensible one (a bare platform may answer with a barcode font), so the usual ones are tried first.
     */
    QFont monospaceFont(const QFont &base)
    {
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Menlo"), QStringLiteral("DejaVu Sans Mono"),
                          QStringLiteral("Liberation Mono"), QStringLiteral("Courier New"), font.family()});
        font.setStyleHint(QFont::Monospace);
        if (base.pointSizeF() > 0)
        {
            font.setPointSizeF(base.pointSizeF() * 0.95);
        }
        return font;
    }
}

// ============================================================================
// One line of the player list: swatch (colour + symbol), name, tags, turn marker
// ============================================================================

class GamePage::PlayerRow : public QWidget
{
public:
    explicit PlayerRow(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(44);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setEntry(const PlayerEntry &entry)
    {
        entry_ = entry;
        setToolTip(PlayerStyles::describe(entry.id));
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(220, 44);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF area = QRectF(rect()).adjusted(1.0, 2.0, -1.0, -2.0);
        const bool dark = UiUtils::isDark(palette());
        const QColor text = palette().color(QPalette::Text);
        const QColor base = UiUtils::cardColor(palette()); // the rows sit on a card
        const QColor muted((text.red() * 55 + base.red() * 45) / 100,
                           (text.green() * 55 + base.green() * 45) / 100,
                           (text.blue() * 55 + base.blue() * 45) / 100);

        // Background of the player who is to move (or of the winner).
        if (entry_.isWinner || entry_.isCurrent)
        {
            QColor fill = entry_.isWinner ? QColor(0xF7, 0xC9, 0x48) : palette().color(QPalette::Highlight);
            fill.setAlpha(dark ? 70 : 48);
            QColor edge = fill;
            edge.setAlpha(150);
            p.setPen(QPen(edge, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(area, 8.0, 8.0);
        }

        // Swatch.
        const qreal radius = 14.0;
        const QPointF center(area.left() + 10.0 + radius, area.center().y());
        DiscPainter::paintDisc(p, center, radius, entry_.id, entry_.present ? 1.0 : 0.35);

        // Marker on the right: a pointer for the current player, a label for the winner.
        qreal right = area.right() - 10.0;
        if (entry_.isWinner)
        {
            QFont small = UiUtils::scaledFont(font(), 0.85, true);
            p.setFont(small);
            p.setPen(text);
            const QString label = GamePage::tr("winner");
            const qreal w = QFontMetricsF(small).horizontalAdvance(label);
            p.drawText(QRectF(right - w, area.top(), w, area.height()), Qt::AlignRight | Qt::AlignVCenter, label);
            right -= w + 8.0;
        }
        else if (entry_.isCurrent)
        {
            QPainterPath arrow;
            const qreal h = 7.0;
            arrow.moveTo(right, area.center().y() - h);
            arrow.lineTo(right - h * 1.2, area.center().y());
            arrow.lineTo(right, area.center().y() + h);
            arrow.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(palette().color(QPalette::Highlight));
            p.drawPath(arrow);
            right -= h * 1.2 + 8.0;
        }

        // Name and tags.
        const qreal textLeft = center.x() + radius + 12.0;
        QFont nameFont = font();
        nameFont.setBold(entry_.isCurrent || entry_.isWinner);
        QFont tagFont = font();

        QString tag;
        if (entry_.isMe)
        {
            tag = GamePage::tr("(you)");
        }
        else if (entry_.left)
        {
            tag = GamePage::tr("(left)");
        }

        const QFontMetricsF tagMetrics(tagFont);
        const qreal tagWidth = tag.isEmpty() ? 0.0 : tagMetrics.horizontalAdvance(tag) + 6.0;

        QString name = entry_.name;
        const bool placeholder = name.isEmpty();
        if (placeholder)
        {
            name = entry_.placeholder.isEmpty() ? GamePage::tr("Waiting for player…") : entry_.placeholder;
            nameFont.setItalic(true);
        }

        const QFontMetricsF nameMetrics(nameFont);
        const qreal available = std::max<qreal>(20.0, right - textLeft - tagWidth);
        const QString elided = nameMetrics.elidedText(name, Qt::ElideRight, available);
        const qreal nameWidth = nameMetrics.horizontalAdvance(elided);

        p.setFont(nameFont);
        p.setPen((placeholder || (!entry_.present && !entry_.isMe)) ? muted : text);
        p.drawText(QRectF(textLeft, area.top(), nameWidth + 2.0, area.height()), Qt::AlignLeft | Qt::AlignVCenter, elided);

        if (!tag.isEmpty())
        {
            p.setFont(tagFont);
            p.setPen(muted);
            p.drawText(QRectF(textLeft + nameWidth + 6.0, area.top(), tagWidth, area.height()), Qt::AlignLeft | Qt::AlignVCenter, tag);
        }
    }

private:
    PlayerEntry entry_;
};

// ============================================================================
// GamePage
// ============================================================================

GamePage::GamePage(QWidget *parent)
    : QWidget(parent),
      board_(new BoardWidget(this)),
      titleLabel_(new QLabel(this)),
      detailsLabel_(new QLabel(this)),
      statusLabel_(new QLabel(this)),
      messageLabel_(new QLabel(this)),
      hintLabel_(new QLabel(this)),
      playersLayout_(new QVBoxLayout),
      leaveButton_(new QPushButton(tr("Leave game"), this)),
      lobbyButton_(new QPushButton(tr("Back to lobby"), this)),
      playAgainButton_(new QPushButton(tr("Play again"), this)),
      problemBox_(new QWidget(this)),
      problemDetailsLabel_(new QLabel(this)),
      problemOutput_(new QPlainTextEdit(this)),
      retryButton_(new QPushButton(tr("Retry"), this))
{
    // ---- side panel ----------------------------------------------------
    titleLabel_->setFont(UiUtils::scaledFont(font(), 1.4, true));
    titleLabel_->setWordWrap(true);
    titleLabel_->setTextFormat(Qt::PlainText);

    detailsLabel_->setForegroundRole(QPalette::PlaceholderText);
    detailsLabel_->setWordWrap(true);
    detailsLabel_->setTextFormat(Qt::PlainText);

    statusLabel_->setFont(UiUtils::scaledFont(font(), 1.25, true));
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setMinimumHeight(statusLabel_->fontMetrics().lineSpacing() * 2 + 4);
    statusLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    messageLabel_->setWordWrap(true);
    messageLabel_->setTextFormat(Qt::PlainText);
    messageLabel_->setVisible(false);

    auto *playersTitle = new QLabel(tr("Players"), this);
    playersTitle->setFont(UiUtils::scaledFont(font(), 0.9, true));
    playersTitle->setForegroundRole(QPalette::PlaceholderText);

    playersLayout_->setSpacing(2);
    playersLayout_->setContentsMargins(0, 0, 0, 0);

    hintLabel_->setWordWrap(true);
    hintLabel_->setTextFormat(Qt::PlainText);
    hintLabel_->setForegroundRole(QPalette::PlaceholderText);
    hintLabel_->setFont(UiUtils::scaledFont(font(), 0.9));

    leaveButton_->setMinimumHeight(36);
    lobbyButton_->setMinimumHeight(36);
    playAgainButton_->setMinimumHeight(36);
    retryButton_->setMinimumHeight(32);
    lobbyButton_->setDefault(true);
    lobbyButton_->setVisible(false);
    playAgainButton_->setVisible(false);
    playAgainButton_->setToolTip(tr("Start another game against the computer, with the same settings"));
    retryButton_->setToolTip(tr("Start the computer opponent again"));
    leaveButton_->setToolTip(tr("Leave this game and go back to the lobby"));

    // ---- what to show when the computer opponent has a problem -------------------------------
    problemDetailsLabel_->setWordWrap(true);
    problemDetailsLabel_->setTextFormat(Qt::PlainText);
    problemOutput_->setObjectName(QStringLiteral("computerOutput"));
    problemOutput_->setReadOnly(true);
    problemOutput_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    problemOutput_->setFont(monospaceFont(font()));
    problemOutput_->setFixedHeight(96);
    problemOutput_->setAccessibleName(tr("Last lines printed by the computer player"));

    auto *problemLayout = new QVBoxLayout(problemBox_);
    problemLayout->setContentsMargins(0, 0, 0, 0);
    problemLayout->setSpacing(6);
    problemLayout->addWidget(problemDetailsLabel_);
    problemLayout->addWidget(problemOutput_);
    problemLayout->addWidget(retryButton_);
    problemBox_->setVisible(false);

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setForegroundRole(QPalette::Mid);

    auto *panel = new CardFrame(this);
    panel->setFixedWidth(290);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setSpacing(8);
    panelLayout->addWidget(titleLabel_);
    panelLayout->addWidget(detailsLabel_);
    panelLayout->addSpacing(2);
    panelLayout->addWidget(separator);
    panelLayout->addSpacing(2);
    panelLayout->addWidget(statusLabel_);
    panelLayout->addWidget(messageLabel_);
    panelLayout->addWidget(problemBox_);
    panelLayout->addSpacing(6);
    panelLayout->addWidget(playersTitle);
    panelLayout->addLayout(playersLayout_);
    panelLayout->addStretch(1);
    panelLayout->addWidget(hintLabel_);
    panelLayout->addSpacing(4);
    panelLayout->addWidget(leaveButton_);
    panelLayout->addWidget(playAgainButton_);
    panelLayout->addWidget(lobbyButton_);

    // ---- page ----------------------------------------------------------
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(20);
    layout->addWidget(board_, 1);
    layout->addWidget(panel, 0);

    messageTimer_.setSingleShot(true);
    connect(&messageTimer_, &QTimer::timeout, this, [this]()
            { messageLabel_->setVisible(false); });

    connect(board_, &BoardWidget::columnActivated, this, &GamePage::onColumnActivated);
    connect(leaveButton_, &QPushButton::clicked, this, &GamePage::leaveRequested);
    connect(lobbyButton_, &QPushButton::clicked, this, &GamePage::leaveRequested);
    connect(playAgainButton_, &QPushButton::clicked, this, &GamePage::playAgainRequested);
    connect(retryButton_, &QPushButton::clicked, this, &GamePage::retryComputerRequested);

    applyColors();
}

void GamePage::startGame(const GameInfo &info, int playerId, const QString &playerName, const QString &computerName)
{
    model_.begin(info, playerId, playerName);
    model_.setComputerOpponent(computerName);
    computerProblem_.clear();
    movePending_ = false;
    wasMyTurn_ = false;
    overlayKey_.clear();
    messageLabel_->setVisible(false);

    board_->reset(model_.rows(), model_.cols());
    board_->setLocalPlayer(playerId);

    const QString name = QString::fromStdString(info.game_name);
    const QString shape = tr("%1 × %2  ·  connect %3  ·  %4 players")
                              .arg(static_cast<int>(info.config.rows))
                              .arg(static_cast<int>(info.config.cols))
                              .arg(static_cast<int>(info.config.connect_length))
                              .arg(static_cast<int>(info.config.num_players));
    if (name.isEmpty())
    {
        titleLabel_->setText(tr("Game #%1").arg(info.game_id));
        detailsLabel_->setText(shape);
    }
    else
    {
        titleLabel_->setText(name);
        detailsLabel_->setText(tr("Game #%1  ·  %2").arg(info.game_id).arg(shape));
    }

    // States that arrived before the join notification were replayed by begin(): show them without animation.
    refresh(false);
}

void GamePage::applyState(const GameStateUpdate &state)
{
    const bool wasActive = model_.isActive();
    if (!model_.applyState(state))
    {
        return;
    }
    if (!wasActive)
    {
        return; // kept by the model, shown after startGame()
    }

    movePending_ = false;
    refresh(!model_.isFirstState());
}

void GamePage::setConnectionLost()
{
    model_.markDisconnected();
    movePending_ = false;
    refresh(false);
}

void GamePage::showMessage(const QString &message)
{
    movePending_ = false;
    messageLabel_->setText(message);
    messageLabel_->setVisible(!message.isEmpty());
    messageTimer_.start(4500);
    refresh(false);
}

void GamePage::showComputerProblem(const QString &message, const QString &output)
{
    computerProblem_ = message.isEmpty() ? tr("The computer could not join the game.") : message;

    problemDetailsLabel_->setText(computerProblem_);
    problemOutput_->setPlainText(output);
    problemOutput_->setVisible(!output.trimmed().isEmpty());
    refresh(false);
}

void GamePage::clearComputerProblem()
{
    if (computerProblem_.isEmpty())
    {
        return;
    }
    computerProblem_.clear();
    refresh(false);
}

QString GamePage::statusText() const
{
    return statusLabel_->text();
}

void GamePage::onColumnActivated(int column)
{
    if (!model_.canMove() || movePending_)
    {
        return;
    }

    movePending_ = true;
    board_->setInputEnabled(false); // one move per turn, even on a double click
    messageLabel_->setVisible(false);
    emit moveRequested(column);
}

void GamePage::refresh(bool animate)
{
    using Phase = GameModel::Phase;

    const Phase phase = model_.phase();

    // ---- board ----------------------------------------------------------
    if (model_.rows() > 0 && model_.cols() > 0)
    {
        board_->setBoard(model_.board(), animate);
    }
    board_->setWinningLines(model_.winningLines());

    const bool myTurn = model_.canMove() && !movePending_;
    board_->setInputEnabled(myTurn);
    if (myTurn && !wasMyTurn_)
    {
        board_->setFocus(Qt::OtherFocusReason); // number keys and arrows work without clicking first
    }
    wasMyTurn_ = myTurn;

    // A problem with the computer opponent only matters while the game waits for it.
    const bool stuck = !computerProblem_.isEmpty() && phase == Phase::Waiting;

    // Only touch the overlay when it changes (a new one fades in).
    const QString title = stuck ? tr("The computer did not join") : model_.bannerTitle();
    const QString subtitle = stuck ? tr("The details are on the right.") : model_.bannerSubtitle();
    const bool centered = (phase == Phase::Waiting);
    const QString key = title + QLatin1Char('|') + subtitle + QLatin1Char('|') + (centered ? QLatin1Char('c') : QLatin1Char('b'));
    if (key != overlayKey_)
    {
        overlayKey_ = key;
        if (title.isEmpty())
        {
            board_->clearOverlay();
        }
        else
        {
            board_->setOverlay(title, subtitle, centered ? BoardWidget::OverlayStyle::Centered : BoardWidget::OverlayStyle::Banner);
        }
    }

    // ---- side panel ------------------------------------------------------
    statusLabel_->setText(stuck ? tr("The computer could not join") : model_.statusText());
    applyColors();

    QList<PlayerEntry> entries = model_.players();
    if (stuck)
    {
        for (PlayerEntry &entry : entries)
        {
            if (!entry.placeholder.isEmpty())
            {
                entry.placeholder = tr("The computer did not join"); // (not "waiting for the computer" any more)
            }
        }
    }
    rebuildPlayerRows(static_cast<int>(entries.size()));
    for (int i = 0; i < entries.size(); ++i)
    {
        rows_[i]->setEntry(entries[i]);
    }

    hintLabel_->setText(hintText());

    // "Leave game" while the game is on, "Back to lobby" once it is over (or cannot start because the
    // computer has a problem), with "Play again" next to it after a game against the computer.
    const bool over = (phase == Phase::Won || phase == Phase::Drawn || phase == Phase::Abandoned || phase == Phase::Disconnected);
    const bool rematch = over && model_.hasComputerOpponent();
    leaveButton_->setVisible(!over && !stuck);
    lobbyButton_->setVisible(over || stuck);
    playAgainButton_->setVisible(rematch);
    playAgainButton_->setDefault(rematch); // (the main button is the one that plays on)
    lobbyButton_->setDefault(!rematch);
    problemBox_->setVisible(stuck);
}

void GamePage::rebuildPlayerRows(int count)
{
    while (rows_.size() < count)
    {
        auto *row = new PlayerRow(this);
        playersLayout_->addWidget(row);
        rows_.push_back(row);
    }
    while (rows_.size() > count)
    {
        PlayerRow *row = rows_.takeLast();
        playersLayout_->removeWidget(row);
        row->deleteLater();
    }
}

QString GamePage::hintText() const
{
    using Phase = GameModel::Phase;

    if (model_.hasComputerOpponent() && !computerProblem_.isEmpty() && model_.phase() == Phase::Waiting)
    {
        return tr("Try again, or go back to the lobby.");
    }

    switch (model_.phase())
    {
    case Phase::Waiting:
        if (model_.hasComputerOpponent())
        {
            return tr("The game starts as soon as the computer has joined: this takes a moment.");
        }
        return tr("The game starts as soon as all the players have joined.");
    case Phase::Playing:
        if (model_.canMove())
        {
            const int lastKey = std::min(model_.cols(), 9);
            return tr("Click a column, or press 1–%1, to drop a disc. The arrow keys with Enter work too.").arg(lastKey);
        }
        return tr("Not your turn: the board unlocks as soon as the other players have moved.");
    case Phase::Won:
    case Phase::Drawn:
        if (model_.hasComputerOpponent())
        {
            return tr("Game over. Play again against the computer, or go back to the lobby.");
        }
        return tr("Game over. Go back to the lobby to play again.");
    case Phase::Abandoned:
    case Phase::Disconnected:
        if (model_.hasComputerOpponent())
        {
            return tr("This game cannot continue. Play again against the computer, or go back to the lobby.");
        }
        return tr("This game cannot continue. Go back to the lobby.");
    case Phase::Idle:
        break;
    }
    return QString();
}

void GamePage::applyColors()
{
    using Phase = GameModel::Phase;

    const Phase phase = model_.phase();

    QPalette status = statusLabel_->palette();
    QColor color = palette().color(QPalette::WindowText);
    if (phase == Phase::Won && model_.winnerId() == model_.myPlayerId())
    {
        color = UiUtils::successColor(palette());
    }
    else if (phase == Phase::Abandoned || phase == Phase::Disconnected || (!computerProblem_.isEmpty() && phase == Phase::Waiting))
    {
        color = UiUtils::errorColor(palette());
    }
    status.setColor(QPalette::WindowText, color);
    statusLabel_->setPalette(status);

    QPalette message = messageLabel_->palette();
    message.setColor(QPalette::WindowText, UiUtils::errorColor(palette()));
    messageLabel_->setPalette(message);

}

void GamePage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    board_->setFocus(Qt::OtherFocusReason);
}

void GamePage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        applyColors();
    }
}
