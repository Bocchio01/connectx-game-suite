#include <algorithm>

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "gui/new_game_dialog.hpp"
#include "gui/ui_utils.hpp"

namespace
{
    constexpr int kMinConnectLength = 3; // the game core refuses anything shorter

    const char *const kOpponentKey = "newGame/opponent"; // "person" or "computer"
    const char *const kLevelKey = "newGame/level";
}

NewGameDialog::NewGameDialog(const HelperPrograms &helpers, QWidget *parent)
    : QDialog(parent),
      helpers_(helpers),
      nameEdit_(new QLineEdit(this)),
      rowsSpin_(new QSpinBox(this)),
      colsSpin_(new QSpinBox(this)),
      playersSpin_(new QSpinBox(this)),
      connectSpin_(new QSpinBox(this)),
      personRadio_(new QRadioButton(tr("Another person"), this)),
      computerRadio_(new QRadioButton(tr("The computer"), this)),
      computerBox_(new QWidget(this)),
      levelCombo_(new QComboBox(this)),
      levelLabel_(new QLabel(this)),
      noteLabel_(new QLabel(this)),
      summaryLabel_(new QLabel(this))
{
    setWindowTitle(tr("New game"));
    setModal(true);

    nameEdit_->setPlaceholderText(tr("Optional"));
    nameEdit_->setMaxLength(Protocol::MAX_PLAYER_NAME_LENGTH);
    nameEdit_->setClearButtonEnabled(true);
    nameEdit_->setToolTip(tr("Give the game a name to find it easily in the lobby.\n"
                             "Leave it empty to make it available to \"Quick play\"."));

    rowsSpin_->setRange(Protocol::MIN_BOARD_SIZE, Protocol::MAX_BOARD_SIZE);
    colsSpin_->setRange(Protocol::MIN_BOARD_SIZE, Protocol::MAX_BOARD_SIZE);
    playersSpin_->setRange(2, Protocol::MAX_PLAYERS);
    connectSpin_->setMinimum(kMinConnectLength);

    rowsSpin_->setToolTip(tr("Number of rows (%1 to %2)").arg(Protocol::MIN_BOARD_SIZE).arg(Protocol::MAX_BOARD_SIZE));
    colsSpin_->setToolTip(tr("Number of columns (%1 to %2)").arg(Protocol::MIN_BOARD_SIZE).arg(Protocol::MAX_BOARD_SIZE));
    playersSpin_->setToolTip(tr("Number of players (2 to %1)").arg(Protocol::MAX_PLAYERS));
    connectSpin_->setToolTip(tr("How many discs in a row are needed to win"));

    // ---- opponent ------------------------------------------------------
    personRadio_->setObjectName(QStringLiteral("personRadio"));
    computerRadio_->setObjectName(QStringLiteral("computerRadio"));
    levelCombo_->setObjectName(QStringLiteral("levelCombo"));
    noteLabel_->setObjectName(QStringLiteral("computerNote"));

    auto *opponentGroup = new QButtonGroup(this);
    opponentGroup->addButton(personRadio_);
    opponentGroup->addButton(computerRadio_);
    personRadio_->setChecked(true);
    personRadio_->setToolTip(tr("Wait for other people to join the game from the lobby"));

    auto *opponentRow = new QWidget(this);
    auto *opponentLayout = new QHBoxLayout(opponentRow);
    opponentLayout->setContentsMargins(0, 0, 0, 0);
    opponentLayout->setSpacing(18);
    opponentLayout->addWidget(personRadio_);
    opponentLayout->addWidget(computerRadio_);
    opponentLayout->addStretch(1);

    for (int level = 1; level <= ComputerLevels::kCount; ++level)
    {
        levelCombo_->addItem(ComputerLevels::name(level), level);
    }
    levelCombo_->setCurrentIndex(ComputerLevels::kDefaultLevel - 1);

    levelLabel_->setWordWrap(true);
    levelLabel_->setTextFormat(Qt::PlainText);
    levelLabel_->setForegroundRole(QPalette::PlaceholderText);

    auto *computerForm = new QFormLayout(computerBox_);
    computerForm->setContentsMargins(0, 0, 0, 0);
    computerForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    computerForm->addRow(tr("Strength"), levelCombo_);
    computerForm->addRow(QString(), levelLabel_);

    noteLabel_->setWordWrap(true);
    noteLabel_->setTextFormat(Qt::PlainText);
    noteLabel_->setForegroundRole(QPalette::PlaceholderText);

    // ---- values ----------------------------------------------------------
    setValues(defaultConfig(), QString());

    // Restore the last numeric values.
    QSettings settings;
    GameConfig last = defaultConfig();
    last.rows = static_cast<uint8_t>(settings.value(QStringLiteral("newGame/rows"), last.rows).toInt());
    last.cols = static_cast<uint8_t>(settings.value(QStringLiteral("newGame/cols"), last.cols).toInt());
    last.num_players = static_cast<uint8_t>(settings.value(QStringLiteral("newGame/players"), last.num_players).toInt());
    last.connect_length = static_cast<uint8_t>(settings.value(QStringLiteral("newGame/connect"), last.connect_length).toInt());
    setValues(last, QString());

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("Name"), nameEdit_);
    form->addRow(tr("Rows"), rowsSpin_);
    form->addRow(tr("Columns"), colsSpin_);
    form->addRow(tr("Players"), playersSpin_);
    form->addRow(tr("Connect"), connectSpin_);
    form->addRow(tr("Opponent"), opponentRow);

    summaryLabel_->setWordWrap(true);
    summaryLabel_->setTextFormat(Qt::PlainText);
    summaryLabel_->setForegroundRole(QPalette::PlaceholderText);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create game"));
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewGameDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &NewGameDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->addLayout(form);
    layout->addWidget(computerBox_);
    layout->addWidget(noteLabel_);
    layout->addWidget(summaryLabel_);
    layout->addWidget(buttons);

    connect(rowsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &NewGameDialog::updateConnectRange);
    connect(colsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &NewGameDialog::updateConnectRange);
    for (QSpinBox *spin : {rowsSpin_, colsSpin_, playersSpin_, connectSpin_})
    {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, &NewGameDialog::updateOpponent);
    }
    connect(computerRadio_, &QRadioButton::toggled, this, &NewGameDialog::updateOpponent);
    connect(levelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &NewGameDialog::updateOpponent);

    updateConnectRange();

    // Restore the last opponent (the computer only if it can play this game).
    OpponentChoice previous;
    previous.computer = settings.value(QString::fromLatin1(kOpponentKey)).toString() == QLatin1String("computer");
    previous.level = settings.value(QString::fromLatin1(kLevelKey), ComputerLevels::kDefaultLevel).toInt();
    setOpponent(previous);
    updateOpponent();

    setMinimumWidth(420);
    nameEdit_->setFocus();
}

GameConfig NewGameDialog::defaultConfig()
{
    return GameConfig(6, 7, 2, 4);
}

GameConfig NewGameDialog::config() const
{
    return GameConfig(static_cast<uint8_t>(rowsSpin_->value()),
                      static_cast<uint8_t>(colsSpin_->value()),
                      static_cast<uint8_t>(playersSpin_->value()),
                      static_cast<uint8_t>(connectSpin_->value()));
}

QString NewGameDialog::gameName() const
{
    return nameEdit_->text().trimmed();
}

OpponentChoice NewGameDialog::opponent() const
{
    OpponentChoice choice;
    choice.computer = computerRadio_->isChecked() && computerRadio_->isEnabled();
    choice.level = choice.computer ? levelCombo_->currentData().toInt() : 0;
    return choice;
}

void NewGameDialog::setValues(const GameConfig &config, const QString &gameName)
{
    rowsSpin_->setValue(std::clamp<int>(config.rows, Protocol::MIN_BOARD_SIZE, Protocol::MAX_BOARD_SIZE));
    colsSpin_->setValue(std::clamp<int>(config.cols, Protocol::MIN_BOARD_SIZE, Protocol::MAX_BOARD_SIZE));
    playersSpin_->setValue(std::clamp<int>(config.num_players, 2, Protocol::MAX_PLAYERS));

    // The connect length is bounded by the board: refresh its range before setting it.
    connectSpin_->setMaximum(std::max(rowsSpin_->value(), colsSpin_->value()));
    connectSpin_->setValue(config.connect_length);

    nameEdit_->setText(gameName);
}

void NewGameDialog::setOpponent(const OpponentChoice &choice)
{
    if (ComputerLevels::isValid(choice.level))
    {
        levelCombo_->setCurrentIndex(choice.level - 1);
    }

    if (choice.computer && isComputerAvailable())
    {
        computerRadio_->setChecked(true);
    }
    else
    {
        personRadio_->setChecked(true);
    }
    updateOpponent();
}

bool NewGameDialog::isComputerAvailable(QString *reason) const
{
    if (!helpers_.hasComputerPlayer())
    {
        if (reason != nullptr)
        {
            *reason = tr("The computer player (connectx-ai) was not found, next to this program or on the PATH.");
        }
        return false;
    }
    return ComputerLevels::isSupported(config(), reason);
}

void NewGameDialog::updateConnectRange()
{
    // Four in a row cannot fit on a board smaller than that: the longest useful line is the longest side.
    connectSpin_->setMaximum(std::max(rowsSpin_->value(), colsSpin_->value()));
}

void NewGameDialog::updateOpponent()
{
    QString reason;
    const bool available = isComputerAvailable(&reason);

    // The computer is disabled, with the reason as tooltip, whenever it cannot play this game.
    computerRadio_->setEnabled(available);
    computerRadio_->setToolTip(available ? tr("Play against the computer: a helper program that runs on this computer")
                                         : reason);
    noteLabel_->setText(available ? QString() : reason); // (the tooltip is easy to miss: say it in the dialog too)
    noteLabel_->setVisible(!available);
    if (!available && computerRadio_->isChecked())
    {
        personRadio_->setChecked(true);
    }

    personRadio_->setText(playersSpin_->value() > 2 ? tr("Other people") : tr("Another person"));

    const bool computer = computerRadio_->isChecked();
    computerBox_->setVisible(computer);
    if (computer)
    {
        const int level = levelCombo_->currentData().toInt();
        const GameConfig current = config();
        for (int i = 0; i < levelCombo_->count(); ++i)
        {
            levelCombo_->setItemData(i, ComputerLevels::details(i + 1, current, helpers_.network), Qt::ToolTipRole);
        }
        levelCombo_->setToolTip(ComputerLevels::details(level, current, helpers_.network));
        levelLabel_->setText(ComputerLevels::summary(level));
    }

    updateSummary();
}

void NewGameDialog::updateSummary()
{
    if (computerRadio_->isChecked())
    {
        summaryLabel_->setText(tr("You play against the computer (%1) on a %2 × %3 board. The first to line up %4 discs wins.")
                                   .arg(ComputerLevels::name(levelCombo_->currentData().toInt()))
                                   .arg(rowsSpin_->value())
                                   .arg(colsSpin_->value())
                                   .arg(connectSpin_->value()));
        return;
    }

    summaryLabel_->setText(tr("%1 players take turns on a %2 × %3 board. The first to line up %4 discs wins.")
                               .arg(playersSpin_->value())
                               .arg(rowsSpin_->value())
                               .arg(colsSpin_->value())
                               .arg(connectSpin_->value()));
}

void NewGameDialog::accept()
{
    QSettings settings;
    settings.setValue(QStringLiteral("newGame/rows"), rowsSpin_->value());
    settings.setValue(QStringLiteral("newGame/cols"), colsSpin_->value());
    settings.setValue(QStringLiteral("newGame/players"), playersSpin_->value());
    settings.setValue(QStringLiteral("newGame/connect"), connectSpin_->value());

    // Remember the kind of opponent too, but only a deliberate choice: when the computer cannot play the
    // game just configured, the previous choice must not be overwritten by the fallback.
    if (computerRadio_->isEnabled())
    {
        settings.setValue(QString::fromLatin1(kOpponentKey), computerRadio_->isChecked() ? QStringLiteral("computer") : QStringLiteral("person"));
        settings.setValue(QString::fromLatin1(kLevelKey), levelCombo_->currentData().toInt());
    }
    QDialog::accept();
}
