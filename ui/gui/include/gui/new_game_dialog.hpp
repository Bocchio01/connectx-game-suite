#pragma once

#include <QDialog>
#include <QString>

#include "protocol/protocol.hpp"

#include "gui/computer_levels.hpp"
#include "gui/helper_programs.hpp"
#include "gui/metatypes.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QWidget;

/**
 * Dialog to configure a new game: optional name, board size, number of players, connect length and the
 * opponent: other people (who join from the lobby) or the computer, with a strength level.
 *
 * The ranges are the ones accepted by the server (the connect length is limited by the board). The
 * computer can be chosen only when the helper program was found and plays this kind of game (two players,
 * a board that fits its engine): otherwise its button is disabled, with a tooltip that says why. The last
 * values are remembered with QSettings.
 */
class NewGameDialog : public QDialog
{
    Q_OBJECT

public:
    /** `helpers` tells whether the computer player exists (see HelperPrograms::locate()). */
    explicit NewGameDialog(const HelperPrograms &helpers, QWidget *parent = nullptr);

    GameConfig config() const;
    QString gameName() const;

    /** The chosen opponent (level 0 for other people). */
    OpponentChoice opponent() const;

    void setValues(const GameConfig &config, const QString &gameName);

    /** Chooses the opponent; the computer is ignored when it is not available for the current game. */
    void setOpponent(const OpponentChoice &choice);

    /** Whether the computer can be chosen now; if not, `reason` tells why (the text of its tooltip). */
    bool isComputerAvailable(QString *reason = nullptr) const;

    /** Values that open the dialog by default: 6 x 7, 2 players, connect 4. */
    static GameConfig defaultConfig();

public slots:
    void accept() override;

private slots:
    void updateConnectRange();
    void updateSummary();
    void updateOpponent();

private:
    HelperPrograms helpers_;
    QLineEdit *nameEdit_;
    QSpinBox *rowsSpin_;
    QSpinBox *colsSpin_;
    QSpinBox *playersSpin_;
    QSpinBox *connectSpin_;
    QRadioButton *personRadio_;
    QRadioButton *computerRadio_;
    QWidget *computerBox_;
    QComboBox *levelCombo_;
    QLabel *levelLabel_;
    QLabel *noteLabel_;
    QLabel *summaryLabel_;
};
