#pragma once

#include <memory>
#include <optional>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTest>

#include "gui/board_widget.hpp"
#include "gui/connect_page.hpp"
#include "gui/game_list_model.hpp"
#include "gui/lobby_page.hpp"
#include "gui/main_window.hpp"

/** Helpers of the tests that drive the whole window like a user would. */
namespace WindowSupport
{
    inline QPushButton *findButton(QWidget *root, const QString &text)
    {
        for (QPushButton *button : root->findChildren<QPushButton *>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    inline QAction *findAction(QWidget *root, const QString &text)
    {
        for (QAction *action : root->findChildren<QAction *>())
        {
            QString plain = action->text();
            plain.remove(QLatin1Char('&'));
            if (plain == text)
            {
                return action;
            }
        }
        return nullptr;
    }

    inline bool hasVisibleLabel(QWidget *root, const QString &text)
    {
        for (QLabel *label : root->findChildren<QLabel *>())
        {
            if (label->isVisibleTo(root) && label->text().contains(text))
            {
                return true;
            }
        }
        return false;
    }

    inline std::optional<GameInfo> findGame(const GameListModel &model, const QString &name)
    {
        for (int row = 0; row < model.rowCount(); ++row)
        {
            const GameInfo *game = model.gameAt(row);
            if (game != nullptr && QString::fromStdString(game->game_name) == name)
            {
                return *game;
            }
        }
        return std::nullopt;
    }

    /** The tests start the window the way main() does, with isolated settings. */
    inline std::unique_ptr<MainWindow> makeWindow(const QString &host, quint16 port, const QString &name)
    {
        auto window = std::make_unique<MainWindow>();
        window->setConfirmationsEnabled(false);
        window->resize(1100, 760);
        window->connectPage()->setValues(host, port, name);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }

    inline bool connectWindow(MainWindow &window, int timeoutMs = 10000)
    {
        window.connectNow();
        return QTest::qWaitFor([&]()
                               { return window.currentPage() == MainWindow::Page::Lobby; },
                               timeoutMs);
    }

    /** Waits until the model of the lobby shows a game with this name. */
    inline bool waitForGame(MainWindow &window, const QString &name, int timeoutMs = 8000)
    {
        return QTest::qWaitFor([&]()
                               { return findGame(*window.lobbyPage()->model(), name).has_value(); },
                               timeoutMs);
    }

    inline void selectAndJoin(MainWindow &window, const QString &name)
    {
        const auto game = findGame(*window.lobbyPage()->model(), name);
        QVERIFY(game.has_value());
        window.lobbyPage()->selectGame(game->game_id);
        QPushButton *join = findButton(window.lobbyPage(), QStringLiteral("Join selected"));
        QVERIFY(join && join->isEnabled());
        QTest::mouseClick(join, Qt::LeftButton);
    }

    /** Presses a key of the number row for a column (0-based). */
    inline void pressColumnKey(BoardWidget *board, int column)
    {
        QTest::keyClick(board, static_cast<Qt::Key>(Qt::Key_1 + column));
    }
}
