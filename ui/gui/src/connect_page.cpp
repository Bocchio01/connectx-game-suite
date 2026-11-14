#include <QDir>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QVBoxLayout>

#include "protocol/protocol.hpp"

#include "gui/connect_page.hpp"
#include "gui/ui_utils.hpp"

ConnectPage::ConnectPage(QWidget *parent)
    : QWidget(parent),
      hostEdit_(new QLineEdit(this)),
      portSpin_(new QSpinBox(this)),
      nameEdit_(new QLineEdit(this)),
      errorLabel_(new QLabel(this)),
      hintLabel_(new QLabel(this)),
      busyLabel_(new QLabel(this)),
      busyBar_(new QProgressBar(this)),
      busyBox_(new QWidget(this)),
      connectButton_(new QPushButton(tr("Connect"), this)),
      cancelButton_(new QPushButton(tr("Cancel"), this)),
      offlineButton_(new QPushButton(tr("Play offline against the computer"), this))
{
    // ---- header --------------------------------------------------------
    auto *iconLabel = new QLabel(this);
    iconLabel->setPixmap(UiUtils::appIcon().pixmap(64, 64));
    iconLabel->setFixedSize(64, 64);

    auto *title = new QLabel(tr("ConnectX"), this);
    title->setFont(UiUtils::scaledFont(font(), 2.1, true));

    auto *subtitle = new QLabel(tr("Connect 4, and then some. Pick a server and a name to play."), this);
    subtitle->setWordWrap(true);
    subtitle->setForegroundRole(QPalette::PlaceholderText);

    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(2);
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);

    auto *header = new QHBoxLayout;
    header->setSpacing(14);
    header->addWidget(iconLabel, 0, Qt::AlignTop);
    header->addLayout(titleColumn, 1);

    // ---- form ----------------------------------------------------------
    hostEdit_->setPlaceholderText(tr("localhost"));
    hostEdit_->setClearButtonEnabled(true);
    portSpin_->setRange(1, 65535);
    portSpin_->setToolTip(tr("TCP port of the server (default %1)").arg(Protocol::DEFAULT_PORT));
    nameEdit_->setMaxLength(Protocol::MAX_PLAYER_NAME_LENGTH);
    nameEdit_->setPlaceholderText(tr("Your name"));
    nameEdit_->setClearButtonEnabled(true);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->addRow(tr("Server"), hostEdit_);
    form->addRow(tr("Port"), portSpin_);
    form->addRow(tr("Player name"), nameEdit_);

    errorLabel_->setWordWrap(true);
    errorLabel_->setTextFormat(Qt::PlainText);
    errorLabel_->setVisible(false);
    applyErrorColor();

    busyBar_->setRange(0, 0); // indeterminate
    busyBar_->setTextVisible(false);
    busyBar_->setFixedHeight(6);
    busyLabel_->setTextFormat(Qt::PlainText);
    busyLabel_->setWordWrap(true);

    connectButton_->setDefault(true);
    connectButton_->setMinimumHeight(34);
    cancelButton_->setMinimumHeight(34);

    auto *busyRow = new QHBoxLayout;
    busyRow->setSpacing(12);
    auto *busyColumn = new QVBoxLayout;
    busyColumn->setSpacing(6);
    busyColumn->addWidget(busyLabel_);
    busyColumn->addWidget(busyBar_);
    busyRow->addLayout(busyColumn, 1);
    busyRow->addWidget(cancelButton_, 0, Qt::AlignVCenter);

    busyRow->setContentsMargins(0, 0, 0, 0);
    busyBox_->setLayout(busyRow);
    busyBox_->setVisible(false);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(connectButton_);

    auto *card = new CardFrame(this);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(14);
    cardLayout->addLayout(form);
    cardLayout->addWidget(errorLabel_);
    cardLayout->addWidget(busyBox_);
    cardLayout->addLayout(buttons);

    // The way in that needs no server at all (and no network): a private server and the computer as opponent.
    offlineButton_->setMinimumHeight(34);
    offlineButton_->setObjectName(QStringLiteral("offlineButton"));
    offlineButton_->setEnabled(false); // until the window says that the helper programs exist
    auto *offlineRow = new QHBoxLayout;
    offlineRow->addStretch(1);
    offlineRow->addWidget(offlineButton_);
    offlineRow->addStretch(1);

    hintLabel_->setText(tr("No server yet? Run connectx-server on this machine and connect to localhost."));
    hintLabel_->setWordWrap(true);
    hintLabel_->setForegroundRole(QPalette::PlaceholderText);
    hintLabel_->setAlignment(Qt::AlignCenter);

    // ---- page ----------------------------------------------------------
    auto *column = new QVBoxLayout;
    column->setSpacing(22);
    column->addLayout(header);
    column->addWidget(card);
    column->addLayout(offlineRow);
    column->addWidget(hintLabel_);

    auto *centered = new QWidget(this);
    centered->setLayout(column);
    centered->setMaximumWidth(500);
    centered->setMinimumWidth(380);

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->addStretch(1);
    auto *middle = new QVBoxLayout;
    middle->addStretch(1);
    middle->addWidget(centered);
    middle->addStretch(1);
    outer->addLayout(middle, 0);
    outer->addStretch(1);

    // ---- initial values ------------------------------------------------
    QSettings settings;
    hostEdit_->setText(settings.value(QStringLiteral("connection/host"), QStringLiteral("localhost")).toString());
    portSpin_->setValue(settings.value(QStringLiteral("connection/port"), static_cast<int>(Protocol::DEFAULT_PORT)).toInt());
    nameEdit_->setText(settings.value(QStringLiteral("connection/name"), defaultPlayerName()).toString());

    connect(connectButton_, &QPushButton::clicked, this, &ConnectPage::submit);
    connect(cancelButton_, &QPushButton::clicked, this, &ConnectPage::cancelRequested);
    connect(offlineButton_, &QPushButton::clicked, this, &ConnectPage::submitOffline);
    connect(hostEdit_, &QLineEdit::returnPressed, this, &ConnectPage::submit);
    connect(nameEdit_, &QLineEdit::returnPressed, this, &ConnectPage::submit);
}

QString ConnectPage::defaultPlayerName()
{
    QString name = qEnvironmentVariable("USERNAME"); // Windows
    if (name.isEmpty())
    {
        name = qEnvironmentVariable("USER"); // Unix
    }
    if (name.isEmpty())
    {
        name = QDir::home().dirName();
    }
    if (name.isEmpty())
    {
        name = tr("Player");
    }
    return name.left(Protocol::MAX_PLAYER_NAME_LENGTH);
}

QString ConnectPage::host() const
{
    return hostEdit_->text().trimmed();
}

quint16 ConnectPage::port() const
{
    return static_cast<quint16>(portSpin_->value());
}

QString ConnectPage::playerName() const
{
    return nameEdit_->text().trimmed();
}

void ConnectPage::setValues(const QString &host, quint16 port, const QString &playerName)
{
    hostEdit_->setText(host);
    portSpin_->setValue(port);
    nameEdit_->setText(playerName);
}

void ConnectPage::setBusy(bool busy, const QString &message)
{
    busy_ = busy;

    hostEdit_->setEnabled(!busy);
    portSpin_->setEnabled(!busy);
    nameEdit_->setEnabled(!busy);
    connectButton_->setVisible(!busy); // while busy, only Cancel is offered
    offlineButton_->setEnabled(offlineAvailable_ && !busy);

    busyBox_->setVisible(busy);
    busyLabel_->setText(message);

    if (busy)
    {
        setError(QString());
    }
    else
    {
        connectButton_->setFocus();
    }
}

void ConnectPage::setError(const QString &message)
{
    errorLabel_->setText(message);
    errorLabel_->setVisible(!message.isEmpty());
}

void ConnectPage::submit()
{
    if (busy_)
    {
        return;
    }

    if (host().isEmpty())
    {
        setError(tr("Please enter the address of the server."));
        hostEdit_->setFocus();
        return;
    }
    if (playerName().isEmpty())
    {
        setError(tr("Please enter a player name."));
        nameEdit_->setFocus();
        return;
    }

    setError(QString());
    saveSettings();
    emit connectRequested(host(), port(), playerName());
}

void ConnectPage::setOfflineAvailable(bool available, const QString &reason)
{
    offlineAvailable_ = available;
    offlineReason_ = available ? QString() : reason;
    offlineButton_->setEnabled(available && !busy_);
    hintLabel_->setText(available ? tr("No server yet? Play offline against the computer, or run connectx-server on this machine "
                                       "and connect to localhost.")
                                  : tr("No server yet? Run connectx-server on this machine and connect to localhost."));
    offlineButton_->setToolTip(available ? tr("Start a private server on this computer and play against the computer there: "
                                              "no network and no other server needed.")
                                         : reason);
}

void ConnectPage::submitOffline()
{
    if (busy_)
    {
        return;
    }

    if (!offlineAvailable_)
    {
        // (the button is disabled, so this is the command line: say why nothing happens)
        setError(offlineReason_.isEmpty() ? tr("Offline games are not available.") : offlineReason_);
        return;
    }

    if (playerName().isEmpty())
    {
        setError(tr("Please enter a player name."));
        nameEdit_->setFocus();
        return;
    }

    setError(QString());

    // Only the name is remembered: the server and the port of an offline game are not the ones of the form.
    QSettings settings;
    settings.setValue(QStringLiteral("connection/name"), playerName());
    emit offlineRequested(playerName());
}

void ConnectPage::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("connection/host"), host());
    settings.setValue(QStringLiteral("connection/port"), static_cast<int>(port()));
    settings.setValue(QStringLiteral("connection/name"), playerName());
}

void ConnectPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!busy_)
    {
        connectButton_->setFocus();
    }
}

void ConnectPage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        applyErrorColor();
    }
}

void ConnectPage::applyErrorColor()
{
    QPalette pal = errorLabel_->palette();
    pal.setColor(QPalette::WindowText, UiUtils::errorColor(palette()));
    if (pal != errorLabel_->palette())
    {
        errorLabel_->setPalette(pal);
    }
}
