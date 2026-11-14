#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTableView>
#include <QVBoxLayout>

#include "gui/game_list_model.hpp"
#include "gui/lobby_page.hpp"
#include "gui/new_game_dialog.hpp"
#include "gui/ui_utils.hpp"

LobbyPage::LobbyPage(QWidget *parent)
    : QWidget(parent),
      model_(new GameListModel(this)),
      table_(new QTableView(this)),
      listStack_(new QStackedWidget(this)),
      subtitleLabel_(new QLabel(this)),
      messageLabel_(new QLabel(this)),
      emptyLabel_(new QLabel(this)),
      refreshButton_(new QPushButton(tr("Refresh"), this)),
      quickPlayButton_(new QPushButton(tr("Quick play"), this)),
      joinButton_(new QPushButton(tr("Join selected"), this)),
      newGameButton_(new QPushButton(tr("New game…"), this)),
      disconnectButton_(new QPushButton(tr("Disconnect"), this))
{
    // ---- header --------------------------------------------------------
    auto *title = new QLabel(tr("Game lobby"), this);
    title->setFont(UiUtils::scaledFont(font(), 1.7, true));

    subtitleLabel_->setForegroundRole(QPalette::PlaceholderText);
    subtitleLabel_->setTextFormat(Qt::PlainText);

    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(2);
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitleLabel_);

    refreshButton_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refreshButton_->setToolTip(tr("Update the list of games now (it also refreshes by itself)"));

    auto *header = new QHBoxLayout;
    header->addLayout(titleColumn, 1);
    header->addWidget(refreshButton_, 0, Qt::AlignTop);

    // ---- game table ----------------------------------------------------
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setFrameShape(QFrame::NoFrame);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(GameListModel::NameColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setMinimumSectionSize(70);
    table_->setAccessibleName(tr("List of games"));

    emptyLabel_->setText(tr("No games yet.\nPress “Quick play” to join or start a standard game, or create your own with “New game…”."));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setWordWrap(true);
    emptyLabel_->setForegroundRole(QPalette::PlaceholderText);
    emptyLabel_->setMargin(24);

    listStack_->addWidget(table_);
    listStack_->addWidget(emptyLabel_);
    listStack_->setCurrentWidget(emptyLabel_);

    auto *card = new CardFrame(this);
    card->setContentsMargins(6, 6, 6, 6);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(6, 6, 6, 6);
    cardLayout->addWidget(listStack_);

    // ---- message line --------------------------------------------------
    messageLabel_->setWordWrap(true);
    messageLabel_->setTextFormat(Qt::PlainText);
    messageLabel_->setVisible(false);
    applyMessageColor();

    // ---- actions -------------------------------------------------------
    quickPlayButton_->setDefault(true);
    quickPlayButton_->setToolTip(tr("Join the first open game, or start a standard 6 × 7 game"));
    newGameButton_->setToolTip(tr("Choose the board size, the number of players and the connect length, and who to play against: "
                                  "other people or the computer"));
    joinButton_->setToolTip(tr("Join the selected game (only games that are still waiting for players)"));
    disconnectButton_->setToolTip(tr("Close the connection and go back to the server selection"));

    for (QPushButton *button : {quickPlayButton_, joinButton_, newGameButton_, disconnectButton_, refreshButton_})
    {
        button->setMinimumHeight(34);
    }

    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    actions->addWidget(quickPlayButton_);
    actions->addWidget(joinButton_);
    actions->addWidget(newGameButton_);
    actions->addStretch(1);
    actions->addWidget(disconnectButton_);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);
    layout->addLayout(header);
    layout->addWidget(card, 1);
    layout->addWidget(messageLabel_);
    layout->addLayout(actions);

    // ---- behaviour -----------------------------------------------------
    refreshTimer_.setInterval(kRefreshIntervalMs);
    messageTimer_.setSingleShot(true);

    connect(&refreshTimer_, &QTimer::timeout, this, [this]()
            {
        if (!busy_)
        {
            emit refreshRequested();
        } });
    connect(&messageTimer_, &QTimer::timeout, this, &LobbyPage::clearMessage);

    connect(refreshButton_, &QPushButton::clicked, this, &LobbyPage::refreshRequested);
    connect(quickPlayButton_, &QPushButton::clicked, this, &LobbyPage::quickPlayRequested);
    connect(joinButton_, &QPushButton::clicked, this, &LobbyPage::joinSelected);
    connect(newGameButton_, &QPushButton::clicked, this, [this]()
            { openNewGameDialog(false); });
    connect(disconnectButton_, &QPushButton::clicked, this, &LobbyPage::disconnectRequested);
    connect(table_, &QTableView::doubleClicked, this, &LobbyPage::onRowActivated);
    table_->installEventFilter(this); // Return/Enter joins the selected game
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LobbyPage::updateButtons);

    // The empty hint replaces the table while there is nothing to list.
    auto updateStack = [this]()
    {
        listStack_->setCurrentWidget(model_->rowCount() == 0 ? static_cast<QWidget *>(emptyLabel_) : static_cast<QWidget *>(table_));
        updateButtons();
    };
    connect(model_, &QAbstractItemModel::rowsInserted, this, updateStack);
    connect(model_, &QAbstractItemModel::rowsRemoved, this, updateStack);
    connect(model_, &QAbstractItemModel::modelReset, this, updateStack);
    connect(model_, &QAbstractItemModel::dataChanged, this, &LobbyPage::updateButtons);

    updateSubtitle();
    updateButtons();
}

void LobbyPage::setConnection(const QString &host, quint16 port, const QString &playerName, bool privateServer)
{
    // A private server must never be mistaken for a remote one: say it right under the title.
    connectionText_ = privateServer ? tr("Offline: playing on a private server of this computer (port %1) as %2").arg(port).arg(playerName)
                                    : tr("Connected to %1:%2 as %3").arg(host).arg(port).arg(playerName);
    updateSubtitle();

    // (nobody else is on a private server: the quick game is one against the computer, see MainWindow)
    quickPlayButton_->setToolTip(privateServer ? tr("Start a standard 6 × 7 game against the computer")
                                               : tr("Join the first open game, or start a standard 6 × 7 game"));
    emptyLabel_->setText(privateServer ? tr("No games yet.\nPress “Quick play” to start a standard game against the computer, "
                                            "or choose the game yourself with “New game…”.")
                                       : tr("No games yet.\nPress “Quick play” to join or start a standard game, or create your own with “New game…”."));
}

void LobbyPage::setHelperPrograms(const HelperPrograms &helpers)
{
    helpers_ = helpers;
}

void LobbyPage::updateSubtitle()
{
    QString text = connectionText_;
    if (stale_)
    {
        text += tr("  ·  could not refresh the list");
    }
    subtitleLabel_->setText(text);
}

void LobbyPage::setGames(const QList<GameInfo> &games)
{
    const quint32 selected = selectedGameId();

    model_->setGames(games);

    // The model updates the rows in place, so the selection normally survives. When the selected game
    // is gone the view would silently select its neighbour: that could make the user join a game
    // they did not choose, so the selection is cleared instead.
    if (selected != 0)
    {
        const int row = model_->rowForId(selected);
        if (row < 0)
        {
            table_->clearSelection();
            table_->setCurrentIndex(QModelIndex());
        }
        else if (selectedGameId() != selected)
        {
            table_->selectRow(row);
        }
    }

    setListStale(false);
}

void LobbyPage::setListStale(bool stale)
{
    if (stale_ != stale)
    {
        stale_ = stale;
        updateSubtitle();
    }
}

void LobbyPage::setBusy(bool busy, const QString &message)
{
    busy_ = busy;
    if (busy)
    {
        showMessage(message, false, 0);
    }
    else if (!messageIsError_)
    {
        clearMessage();
    }
    updateButtons();
}

void LobbyPage::updateButtons()
{
    const GameInfo *selected = model_->gameAt(selectedRow());

    quickPlayButton_->setEnabled(!busy_);
    newGameButton_->setEnabled(!busy_);
    joinButton_->setEnabled(!busy_ && selected != nullptr && GameListModel::isJoinable(*selected));
}

int LobbyPage::selectedRow() const
{
    const QModelIndexList rows = table_->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

bool LobbyPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == table_ && event->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
        {
            joinSelected();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

quint32 LobbyPage::selectedGameId() const
{
    const GameInfo *game = model_->gameAt(selectedRow());
    return game != nullptr ? game->game_id : 0;
}

void LobbyPage::selectGame(quint32 gameId)
{
    const int row = model_->rowForId(gameId);
    if (row >= 0)
    {
        table_->selectRow(row);
    }
}

void LobbyPage::joinSelected()
{
    const quint32 id = selectedGameId();
    if (id == 0 || busy_)
    {
        return;
    }
    const GameInfo *game = model_->gameAt(model_->rowForId(id));
    if (game != nullptr && GameListModel::isJoinable(*game))
    {
        emit joinRequested(id);
    }
}

void LobbyPage::onRowActivated(const QModelIndex &index)
{
    if (busy_ || !index.isValid())
    {
        return;
    }
    const GameInfo *game = model_->gameAt(index.row());
    if (game == nullptr)
    {
        return;
    }
    if (GameListModel::isJoinable(*game))
    {
        emit joinRequested(game->game_id);
    }
    else
    {
        showError(game->status == ProtocolGameStatus::NOT_STARTED ? tr("That game is already full.") : tr("That game has already started."));
    }
}

void LobbyPage::openNewGameDialog(bool preferComputer)
{
    if (busy_)
    {
        return;
    }

    NewGameDialog dialog(helpers_, this);
    if (preferComputer)
    {
        dialog.setOpponent(OpponentChoice{true, 0}); // (level 0: keep the one remembered by the dialog)
    }
    if (dialog.exec() == QDialog::Accepted)
    {
        emit newGameRequested(dialog.config(), dialog.gameName(), dialog.opponent());
    }
}

void LobbyPage::showError(const QString &message)
{
    showMessage(message, true, 9000);
}

void LobbyPage::showInfo(const QString &message)
{
    showMessage(message, false, 6000);
}

void LobbyPage::showMessage(const QString &message, bool isError, int timeoutMs)
{
    messageIsError_ = isError && !message.isEmpty();
    messageLabel_->setText(message);
    messageLabel_->setVisible(!message.isEmpty());
    applyMessageColor();

    messageTimer_.stop();
    if (timeoutMs > 0 && !message.isEmpty())
    {
        messageTimer_.start(timeoutMs);
    }
}

void LobbyPage::clearMessage()
{
    messageIsError_ = false;
    messageLabel_->clear();
    messageLabel_->setVisible(false);
    messageTimer_.stop();
}

QString LobbyPage::message() const
{
    return messageLabel_->text();
}

void LobbyPage::applyMessageColor()
{
    QPalette pal = messageLabel_->palette();
    pal.setColor(QPalette::WindowText, messageIsError_ ? UiUtils::errorColor(palette()) : UiUtils::mutedColor(palette()));
    messageLabel_->setPalette(pal);
}

void LobbyPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshTimer_.start();
    if (!busy_)
    {
        emit refreshRequested(); // do not wait for the first tick
    }
    quickPlayButton_->setFocus();
}

void LobbyPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    refreshTimer_.stop();
}

void LobbyPage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        applyMessageColor();
    }
}
