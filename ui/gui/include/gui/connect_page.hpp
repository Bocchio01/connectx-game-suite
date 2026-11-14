#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

/**
 * First page: server address, port and player name.
 * The last values are remembered with QSettings. While a connection is being established the
 * form is locked and a busy indicator with a Cancel button is shown.
 */
class ConnectPage : public QWidget
{
    Q_OBJECT

public:
    explicit ConnectPage(QWidget *parent = nullptr);

    QString host() const;
    quint16 port() const;
    QString playerName() const;
    void setValues(const QString &host, quint16 port, const QString &playerName);

    /** Name of the operating system user, used as the initial player name. */
    static QString defaultPlayerName();

    /** Locks the form and shows `message` with a progress bar (busy) or unlocks it. */
    void setBusy(bool busy, const QString &message = QString());

    /** Shows an error under the form (empty text hides it). */
    void setError(const QString &message);

    /** Same as pressing the Connect button (validates, remembers the values, emits connectRequested()). */
    void submit();

    /**
     * Same as pressing "Play offline against the computer" (validates the name, emits offlineRequested()).
     * When offline games are not available the reason is shown under the form instead (this is how
     * `--offline` explains itself).
     */
    void submitOffline();

    /**
     * Whether offline games can be played at all (they need the helper programs). When they cannot, the
     * button is disabled and `reason` becomes its tooltip.
     */
    void setOfflineAvailable(bool available, const QString &reason = QString());
    bool isOfflineAvailable() const { return offlineAvailable_; }

    bool isBusy() const { return busy_; }

signals:
    void connectRequested(const QString &host, quint16 port, const QString &playerName);
    void cancelRequested();

    /** The user wants to play against the computer on a private server of this computer. */
    void offlineRequested(const QString &playerName);

protected:
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void saveSettings() const;
    void applyErrorColor();

    QLineEdit *hostEdit_;
    QSpinBox *portSpin_;
    QLineEdit *nameEdit_;
    QLabel *errorLabel_;
    QLabel *hintLabel_;
    QLabel *busyLabel_;
    QProgressBar *busyBar_;
    QWidget *busyBox_;
    QPushButton *connectButton_;
    QPushButton *cancelButton_;
    QPushButton *offlineButton_;
    bool busy_ = false;
    bool offlineAvailable_ = false;
    QString offlineReason_; // why not, while offline games are not available
};
