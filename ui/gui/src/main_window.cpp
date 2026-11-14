#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>

#include "gui/computer_player.hpp"
#include "gui/connect_page.hpp"
#include "gui/game_page.hpp"
#include "gui/lobby_page.hpp"
#include "gui/main_window.hpp"
#include "gui/new_game_dialog.hpp"
#include "gui/private_server.hpp"
#include "gui/ui_utils.hpp"

namespace
{
    /** Why offline games are not possible, given the programs that were found (empty when they are). */
    QString offlineUnavailableReason(const HelperPrograms &helpers)
    {
        const char *const where = "next to this program or on the PATH";
        if (!helpers.hasServer() && !helpers.hasComputerPlayer())
        {
            return MainWindow::tr("Offline games need the connectx-server and connectx-ai programs, which were not found %1.")
                .arg(QLatin1String(where));
        }
        if (!helpers.hasServer())
        {
            return MainWindow::tr("Offline games need the connectx-server program, which was not found %1.").arg(QLatin1String(where));
        }
        if (!helpers.hasComputerPlayer())
        {
            return MainWindow::tr("Offline games need the connectx-ai program (the computer player), which was not found %1.")
                .arg(QLatin1String(where));
        }
        return QString();
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      controller_(new NetworkController(this)),
      computer_(new ComputerPlayer(this)),
      privateServer_(new PrivateServer(this)),
      stack_(new QStackedWidget(this)),
      connectPage_(new ConnectPage(this)),
      lobbyPage_(new LobbyPage(this)),
      gamePage_(new GamePage(this)),
      connectionLabel_(new QLabel(this)),
      disconnectAction_(nullptr)
{
    setWindowTitle(tr("ConnectX"));
    setWindowIcon(UiUtils::appIcon());
    setMinimumSize(860, 620);

    stack_->addWidget(connectPage_);
    stack_->addWidget(lobbyPage_);
    stack_->addWidget(gamePage_);
    setCentralWidget(stack_);

    createMenus();

    connectionLabel_->setTextFormat(Qt::PlainText);
    connectionLabel_->setContentsMargins(6, 0, 6, 0);
    statusBar()->addPermanentWidget(connectionLabel_);
    updateConnectionStatus();

    // ---- pages -> window / controller -----------------------------------
    connect(connectPage_, &ConnectPage::connectRequested, this, &MainWindow::onConnectRequested);
    connect(connectPage_, &ConnectPage::offlineRequested, this, &MainWindow::onOfflineRequested);
    connect(connectPage_, &ConnectPage::cancelRequested, this, &MainWindow::disconnectFromServer);

    connect(lobbyPage_, &LobbyPage::refreshRequested, controller_, &NetworkController::refreshGames);
    connect(lobbyPage_, &LobbyPage::quickPlayRequested, this, [this]()
            {
        pendingComputer_.reset();
        if (offline_)
        {
            // Nobody else is on a private server: "Quick play" is a standard game against the computer, at
            // the level that the New game dialog remembers.
            OpponentChoice computer;
            computer.computer = true;
            computer.level = QSettings().value(QStringLiteral("newGame/level"), ComputerLevels::kDefaultLevel).toInt();
            onNewGameRequested(NewGameDialog::defaultConfig(), QString(), computer);
            return;
        }
        lobbyPage_->setBusy(true, tr("Looking for a game…"));
        controller_->joinGame(0); });
    connect(lobbyPage_, &LobbyPage::joinRequested, this, [this](quint32 gameId)
            {
        pendingComputer_.reset();
        lobbyPage_->setBusy(true, tr("Joining game #%1…").arg(gameId));
        controller_->joinGame(gameId); });
    connect(lobbyPage_, &LobbyPage::newGameRequested, this, &MainWindow::onNewGameRequested);
    connect(lobbyPage_, &LobbyPage::disconnectRequested, this, &MainWindow::disconnectFromServer);

    connect(gamePage_, &GamePage::moveRequested, controller_, &NetworkController::sendMove);
    connect(gamePage_, &GamePage::leaveRequested, this, &MainWindow::onLeaveRequested);
    connect(gamePage_, &GamePage::playAgainRequested, this, &MainWindow::onPlayAgainRequested);
    connect(gamePage_, &GamePage::retryComputerRequested, this, &MainWindow::onRetryComputer);

    // ---- controller -> pages ---------------------------------------------
    connect(controller_, &NetworkController::connected, this, &MainWindow::onConnected);
    connect(controller_, &NetworkController::connectionFailed, this, &MainWindow::onConnectionFailed);
    connect(controller_, &NetworkController::disconnected, this, &MainWindow::onDisconnected);
    connect(controller_, &NetworkController::gamesListed, lobbyPage_, &LobbyPage::setGames);
    connect(controller_, &NetworkController::refreshFailed, this, [this]()
            { lobbyPage_->setListStale(true); });
    connect(controller_, &NetworkController::joined, this, &MainWindow::onJoined);
    connect(controller_, &NetworkController::joinFailed, this, &MainWindow::onJoinFailed);
    connect(controller_, &NetworkController::createFailed, this, &MainWindow::onJoinFailed);
    connect(controller_, &NetworkController::stateUpdated, gamePage_, &GamePage::applyState);
    connect(controller_, &NetworkController::moveRejected, this, [this](const QString &message)
            {
        gamePage_->showMessage(message);
        showNotice(message); });
    connect(controller_, &NetworkController::serverError, this, &MainWindow::onServerError);

    // ---- helper programs -----------------------------------------------------
    connect(privateServer_, &PrivateServer::ready, this, &MainWindow::onPrivateServerReady);
    connect(privateServer_, &PrivateServer::failed, this, &MainWindow::onPrivateServerFailed);
    connect(privateServer_, &PrivateServer::died, this, &MainWindow::onPrivateServerDied);
    connect(computer_, &ComputerPlayer::failed, this, &MainWindow::onComputerFailed);

    setHelperPrograms(HelperPrograms::locate());

    // ---- geometry --------------------------------------------------------
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (geometry.isEmpty() || !restoreGeometry(geometry))
    {
        resize(1040, 740);
    }

    showPage(Page::Connect);
}

MainWindow::~MainWindow()
{
    // The helpers must be gone before the window is (their destructors would do it too: this is the
    // guarantee that none of them outlives the program).
    computer_->stopAndWait();
    privateServer_->stopAndWait();
}

void MainWindow::setHelperPrograms(const HelperPrograms &helpers)
{
    helpers_ = helpers;
    lobbyPage_->setHelperPrograms(helpers_);
    connectPage_->setOfflineAvailable(helpers_.canPlayOffline(), offlineUnavailableReason(helpers_));
}

void MainWindow::createMenus()
{
    QMenu *gameMenu = menuBar()->addMenu(tr("&Game"));

    disconnectAction_ = gameMenu->addAction(tr("&Disconnect"));
    disconnectAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    disconnectAction_->setStatusTip(tr("Close the connection and go back to the server selection"));
    connect(disconnectAction_, &QAction::triggered, this, &MainWindow::disconnectFromServer);

    gameMenu->addSeparator();

    QAction *quitAction = gameMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    quitAction->setMenuRole(QAction::QuitRole);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));

    QAction *aboutAction = helpMenu->addAction(tr("&About ConnectX"));
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

    QAction *aboutQtAction = helpMenu->addAction(tr("About &Qt"));
    aboutQtAction->setMenuRole(QAction::AboutQtRole);
    connect(aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
}

MainWindow::Page MainWindow::currentPage() const
{
    return static_cast<Page>(stack_->currentIndex());
}

void MainWindow::showPage(Page page)
{
    stack_->setCurrentIndex(static_cast<int>(page));
    updateConnectionStatus();
}

void MainWindow::connectNow()
{
    connectPage_->submit();
}

void MainWindow::offlineNow()
{
    connectPage_->submitOffline();
}

void MainWindow::showNotice(const QString &message, int timeoutMs)
{
    statusBar()->showMessage(message, timeoutMs);
}

void MainWindow::updateConnectionStatus()
{
    const ConnectionInfo &info = controller_->connectionInfo();
    QString text;
    QColor color = UiUtils::mutedColor(palette());

    // An offline game says so, always: a private server must never be mistaken for a remote one.
    switch (controller_->state())
    {
    case NetworkController::State::Idle:
        text = offlineStarting_ ? tr("Offline: starting a private server on this computer…") : tr("Not connected");
        break;
    case NetworkController::State::Connecting:
        if (offline_)
        {
            text = tr("Offline: connecting to the private server on this computer…");
        }
        else
        {
            text = reconnecting_ ? tr("Reconnecting to %1:%2…").arg(info.host).arg(info.port)
                                 : tr("Connecting to %1:%2…").arg(info.host).arg(info.port);
        }
        break;
    case NetworkController::State::Connected:
        text = offline_ ? tr("Offline: private server on this computer (port %1), playing as %2").arg(info.port).arg(info.playerName)
                        : tr("Connected to %1:%2 as %3").arg(info.host).arg(info.port).arg(info.playerName);
        color = UiUtils::successColor(palette());
        break;
    }

    connectionLabel_->setText(QStringLiteral("● ") + text);
    QPalette pal = connectionLabel_->palette();
    pal.setColor(QPalette::WindowText, color);
    connectionLabel_->setPalette(pal);

    disconnectAction_->setEnabled(controller_->state() != NetworkController::State::Idle || currentPage() != Page::Connect || offlineStarting_);
}

// ---- connection --------------------------------------------------------------

void MainWindow::onConnectRequested(const QString &host, quint16 port, const QString &playerName)
{
    leaveOffline(); // (a remote server is not the private one)
    reconnecting_ = false;
    connectPage_->setBusy(true, tr("Connecting to %1:%2…").arg(host).arg(port));
    controller_->connectToServer(host, port, playerName);
    updateConnectionStatus();
}

void MainWindow::onConnected(const ConnectionInfo &info)
{
    reconnecting_ = false;
    connectPage_->setBusy(false);

    lobbyPage_->setConnection(info.host, info.port, info.playerName, offline_);

    if (rematch_)
    {
        // "Play again": the new connection is only a means, the game is created right away (the lobby page
        // shows what is going on meanwhile, and its list is of no interest).
        const ComputerGame game = *rematch_;
        rematch_.reset();
        updateConnectionStatus();
        startComputerGame(game);
        return;
    }

    lobbyPage_->setBusy(false);
    lobbyPage_->setListStale(false);
    showPage(Page::Lobby);

    controller_->refreshGames();
    showNotice(offline_ ? tr("Offline game: the private server on this computer is ready")
                        : tr("Connected to %1:%2").arg(info.host).arg(info.port),
               3000);

    if (offline_ && openNewGameAfterConnect_)
    {
        // The only thing to do offline is to start a game against the computer: ask for it right away.
        openNewGameAfterConnect_ = false;
        QTimer::singleShot(0, this, [this]()
                           {
            if (offline_ && currentPage() == Page::Lobby)
            {
                lobbyPage_->openNewGameDialog(true);
            } });
    }
}

void MainWindow::onConnectionFailed(const QString &message)
{
    reconnecting_ = false;
    rematch_.reset();
    forgetComputerGame();
    leaveOffline();
    lobbyPage_->setBusy(false);
    connectPage_->setBusy(false);
    connectPage_->setError(message);
    showPage(Page::Connect);
    showNotice(message);
}

void MainWindow::onDisconnected(const QString &reason)
{
    computer_->stop(); // (it has nobody to play against any more)

    if (currentPage() == Page::Game)
    {
        // Keep the board on screen (it may show the final result) and offer the way back.
        gamePage_->setConnectionLost();
        updateConnectionStatus();
        showNotice(reason, 8000);
        return;
    }

    rematch_.reset();
    forgetComputerGame();
    leaveOffline();
    lobbyPage_->setBusy(false);
    connectPage_->setBusy(false);
    connectPage_->setError(reason);
    showPage(Page::Connect);
}

void MainWindow::disconnectFromServer()
{
    if (confirmations_ && currentPage() == Page::Game && gamePage_->needsLeaveConfirmation())
    {
        const auto answer = QMessageBox::question(this, tr("Disconnect"),
                                                  tr("A game is in progress. If you disconnect now, the other players will continue without you.\n\nDisconnect anyway?"),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
        {
            return;
        }
    }

    reconnecting_ = false;
    rematch_.reset();
    forgetComputerGame();
    leaveOffline(); // (the offline session ends with the connection: the private server goes too)
    controller_->disconnectFromServer();
    lobbyPage_->setBusy(false);
    lobbyPage_->clearMessage();
    connectPage_->setBusy(false);
    connectPage_->setError(QString());
    showPage(Page::Connect);
}

// ---- lobby and game -----------------------------------------------------------

void MainWindow::onNewGameRequested(const GameConfig &config, const QString &gameName, const OpponentChoice &opponent)
{
    pendingComputer_.reset();
    if (opponent.computer)
    {
        ComputerGame game;
        game.config = config;
        game.gameName = gameName;
        game.level = ComputerLevels::isValid(opponent.level) ? opponent.level : ComputerLevels::kDefaultLevel;
        startComputerGame(game);
        return;
    }

    lobbyPage_->setBusy(true, tr("Creating the game…"));
    controller_->createGame(config, gameName);
}

void MainWindow::onJoined(const GameInfo &info, int playerId)
{
    lobbyPage_->setBusy(false);

    // The game was asked for against the computer: the human has the first seat, the computer comes next.
    QString computerName;
    if (pendingComputer_.has_value())
    {
        computerGame_ = pendingComputer_;
        computerGame_->gameId = info.game_id;
        pendingComputer_.reset();
        computerName = ComputerLevels::playerName(computerGame_->level);
    }
    else
    {
        computer_->stop();
        computerGame_.reset();
    }

    gamePage_->startGame(info, playerId, controller_->connectionInfo().playerName, computerName);
    showPage(Page::Game);

    const QString name = QString::fromStdString(info.game_name);
    showNotice(name.isEmpty() ? tr("Joined game #%1").arg(info.game_id) : tr("Joined game “%1”").arg(name), 3000);

    if (computerGame_.has_value())
    {
        startComputer(info.game_id);
    }
}

void MainWindow::onJoinFailed(const QString &message)
{
    pendingComputer_.reset();
    lobbyPage_->setBusy(false);
    lobbyPage_->showError(message);
    showNotice(message);

    // The list that led to the failure is probably out of date.
    controller_->refreshGames();
}

void MainWindow::onServerError(quint16 code, const QString &message)
{
    Q_UNUSED(code);

    showNotice(message);

    switch (currentPage())
    {
    case Page::Game:
        gamePage_->showMessage(message);
        break;
    case Page::Lobby:
        if (lobbyPage_->message() != message)
        {
            lobbyPage_->showError(message);
        }
        break;
    case Page::Connect:
        break;
    }
}

void MainWindow::onLeaveRequested()
{
    if (confirmations_ && gamePage_->needsLeaveConfirmation())
    {
        const auto answer = QMessageBox::question(this, tr("Leave game"),
                                                  tr("The game is still in progress. If you leave now, the other players will continue without you.\n\nLeave anyway?"),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
        {
            return;
        }
    }

    forgetComputerGame();

    // One game per connection: going back to the lobby means opening a fresh connection.
    reconnecting_ = true;
    lobbyPage_->setGames({});
    lobbyPage_->setBusy(true, tr("Going back to the lobby…"));
    showPage(Page::Lobby);
    controller_->reconnect();
    updateConnectionStatus();
}

// ---- the computer opponent --------------------------------------------------------

void MainWindow::startComputerGame(const ComputerGame &game)
{
    pendingComputer_ = game;
    lobbyPage_->setBusy(true, tr("Creating the game…"));
    controller_->createGame(game.config, game.gameName);
}

void MainWindow::startComputer(quint32 gameId)
{
    if (!computerGame_.has_value())
    {
        return;
    }
    computerGame_->gameId = gameId;

    const ConnectionInfo &connection = controller_->connectionInfo();
    ComputerPlayer::Options options;
    options.program = helpers_.computerPlayer;
    options.host = connection.host;
    options.port = connection.port;
    options.gameId = gameId;
    options.agent = ComputerLevels::agentSpec(computerGame_->level, computerGame_->config, helpers_.network);
    options.playerName = ComputerLevels::playerName(computerGame_->level);
    options.delayMs = computerDelayMs_;

    gamePage_->clearComputerProblem();
    computer_->start(options);
}

void MainWindow::forgetComputerGame()
{
    computer_->stop();
    pendingComputer_.reset();
    computerGame_.reset();
}

void MainWindow::onComputerFailed(const QString &message, const QString &output)
{
    if (!computerGame_.has_value() || currentPage() != Page::Game)
    {
        return; // (late news of a game that has been left)
    }

    if (gamePage_->model().phase() == GameModel::Phase::Waiting)
    {
        // The game cannot start: say why, and offer another try or the way out.
        gamePage_->showComputerProblem(message, output);
    }
    else
    {
        // The game had begun: the game page will show that the opponent is gone.
        showNotice(message, 8000);
    }
}

void MainWindow::onRetryComputer()
{
    if (computerGame_.has_value() && currentPage() == Page::Game)
    {
        startComputer(computerGame_->gameId);
    }
}

void MainWindow::onPlayAgainRequested()
{
    if (!computerGame_.has_value())
    {
        return;
    }

    // Same settings, a new game, a new connection (one game per connection, as when going back to the lobby).
    rematch_ = computerGame_;
    rematch_->gameId = 0;
    computer_->stop();
    pendingComputer_.reset();
    computerGame_.reset();

    reconnecting_ = true;
    lobbyPage_->setGames({});
    lobbyPage_->setBusy(true, tr("Starting a new game against the computer…"));
    showPage(Page::Lobby);
    controller_->reconnect();
    updateConnectionStatus();
}

// ---- offline games --------------------------------------------------------------------

void MainWindow::onOfflineRequested(const QString &playerName)
{
    if (!helpers_.canPlayOffline())
    {
        connectPage_->setError(offlineUnavailableReason(helpers_));
        return;
    }

    leaveOffline();
    controller_->disconnectFromServer();
    reconnecting_ = false;
    offlineName_ = playerName;
    offlineStarting_ = true;

    connectPage_->setBusy(true, tr("Starting a private server on this computer…"));
    privateServer_->start(helpers_.server);
    updateConnectionStatus();
}

void MainWindow::onPrivateServerReady(quint16 port)
{
    offlineStarting_ = false;
    offline_ = true;
    openNewGameAfterConnect_ = true;

    connectPage_->setBusy(true, tr("Connecting to the private server…"));
    controller_->connectToServer(QStringLiteral("127.0.0.1"), port, offlineName_);
    updateConnectionStatus();
}

void MainWindow::onPrivateServerFailed(const QString &message)
{
    offlineStarting_ = false;
    offline_ = false;
    connectPage_->setBusy(false);
    connectPage_->setError(tr("The private server for the offline game could not be started.\n%1").arg(message));
    showPage(Page::Connect);
    showNotice(tr("The private server could not be started"));
}

void MainWindow::onPrivateServerDied(const QString &message)
{
    Q_UNUSED(message);

    // The connection to it is gone too: the controller reports that, and the pages react to it.
    showNotice(tr("The private server of this computer stopped"), 8000);
}

void MainWindow::leaveOffline()
{
    offlineStarting_ = false;
    offline_ = false;
    openNewGameAfterConnect_ = false;
    privateServer_->stop();
    updateConnectionStatus();
}

// ---- misc ---------------------------------------------------------------------

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About ConnectX"),
                       tr("<h3>ConnectX Game Suite</h3>"
                          "<p>Version %1</p>"
                          "<p>Connect 4 with a variable board size, up to four players and a "
                          "configurable connect length, played over the network or against the computer.</p>"
                          "<p>Graphical client built with Qt %2.</p>")
                           .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.sync();

    // The helper programs end with the window; the controller (a child of this window) disconnects and
    // stops its threads when it is destroyed.
    computer_->stopAndWait();
    privateServer_->stopAndWait();

    QMainWindow::closeEvent(event);
}
