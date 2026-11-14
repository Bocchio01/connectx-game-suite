#pragma once

#include <QList>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "protocol/protocol.hpp"

#include "gui/computer_levels.hpp"
#include "gui/helper_programs.hpp"
#include "gui/metatypes.hpp"

class GameListModel;
class QLabel;
class QModelIndex;
class QPushButton;
class QStackedWidget;
class QTableView;

/**
 * Second page: the live list of games and the ways to join one.
 *
 * While the page is visible it asks for a fresh list every couple of seconds
 * (refreshRequested()); the list is updated in place, so the selection survives.
 * Messages (errors from the server...) are shown inline, never in modal dialogs.
 */
class LobbyPage : public QWidget
{
    Q_OBJECT

public:
    explicit LobbyPage(QWidget *parent = nullptr);

    /** Interval between automatic refreshes while the page is visible. */
    static constexpr int kRefreshIntervalMs = 2000;

    /** `privateServer`: the server is the one started for an offline game, which the page says in its subtitle. */
    void setConnection(const QString &host, quint16 port, const QString &playerName, bool privateServer = false);
    void setGames(const QList<GameInfo> &games);

    /** Tells the New game dialog which helper programs exist (the computer opponent needs one). */
    void setHelperPrograms(const HelperPrograms &helpers);

    /** The last refresh failed (the list may be out of date) or recovered. */
    void setListStale(bool stale);

    /** While busy (joining, creating...) the action buttons are disabled and `message` is shown. */
    void setBusy(bool busy, const QString &message = QString());
    bool isBusy() const { return busy_; }

    void showError(const QString &message);
    void showInfo(const QString &message);
    void clearMessage();
    QString message() const;

    GameListModel *model() const { return model_; }

    /** Id of the selected game, 0 if none. */
    quint32 selectedGameId() const;
    void selectGame(quint32 gameId);

public slots:
    /**
     * Opens the New game dialog (modal) and emits newGameRequested() if it is accepted.
     * `preferComputer` opens it with the computer chosen (offline games).
     */
    void openNewGameDialog(bool preferComputer = false);

signals:
    void refreshRequested();
    void quickPlayRequested();
    void joinRequested(quint32 gameId);
    void newGameRequested(const GameConfig &config, const QString &gameName, const OpponentChoice &opponent);
    void disconnectRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void joinSelected();
    void onRowActivated(const QModelIndex &index);
    void updateButtons();

private:
    int selectedRow() const;
    void showMessage(const QString &message, bool isError, int timeoutMs);
    void applyMessageColor();
    void updateSubtitle();

    GameListModel *model_;
    QTableView *table_;
    QStackedWidget *listStack_;
    QLabel *subtitleLabel_;
    QLabel *messageLabel_;
    QLabel *emptyLabel_;
    QPushButton *refreshButton_;
    QPushButton *quickPlayButton_;
    QPushButton *joinButton_;
    QPushButton *newGameButton_;
    QPushButton *disconnectButton_;
    QTimer refreshTimer_;
    QTimer messageTimer_;

    HelperPrograms helpers_;
    QString connectionText_;
    bool stale_ = false;
    bool busy_ = false;
    bool messageIsError_ = false;
};
