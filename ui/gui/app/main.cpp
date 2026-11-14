#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QSettings>
#include <QTimer>

#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

#include "protocol/protocol.hpp"

#include "gui/connect_page.hpp"
#include "gui/main_window.hpp"
#include "gui/network_controller.hpp"
#include "gui/ui_utils.hpp"

#ifndef CONNECTX_GUI_VERSION
#define CONNECTX_GUI_VERSION "3.0"
#endif

int main(int argc, char *argv[])
{
    // The Client library logs through spdlog: keep the console quiet like the CLI does
    // (set SPDLOG_LEVEL=debug in the environment to see everything).
    spdlog::set_level(spdlog::level::err);
    spdlog::cfg::load_env_levels();

    try
    {
        QApplication app(argc, argv);
        QCoreApplication::setOrganizationName(QStringLiteral("Bocchio01"));
        QCoreApplication::setApplicationName(QStringLiteral("ConnectX"));
        QCoreApplication::setApplicationVersion(QStringLiteral(CONNECTX_GUI_VERSION));
        QApplication::setWindowIcon(UiUtils::appIcon());

        QCommandLineParser parser;
        parser.setApplicationDescription(QCoreApplication::translate("main", "ConnectX graphical client"));
        parser.addHelpOption();
        parser.addVersionOption();

        const QCommandLineOption hostOption(QStringLiteral("host"),
                                            QCoreApplication::translate("main", "Server address to prefill (default: the last one used)."),
                                            QStringLiteral("host"));
        const QCommandLineOption portOption({QStringLiteral("p"), QStringLiteral("port")},
                                            QCoreApplication::translate("main", "Server port to prefill (default: the last one used, or %1).").arg(Protocol::DEFAULT_PORT),
                                            QStringLiteral("port"));
        const QCommandLineOption nameOption({QStringLiteral("n"), QStringLiteral("name")},
                                            QCoreApplication::translate("main", "Player name to prefill (default: the last one used)."),
                                            QStringLiteral("name"));
        const QCommandLineOption connectOption(QStringLiteral("connect"),
                                               QCoreApplication::translate("main", "Connect right away, without waiting for the Connect button."));
        const QCommandLineOption offlineOption(QStringLiteral("offline"),
                                               QCoreApplication::translate("main", "Start an offline game against the computer right away (private server)."));
        const QCommandLineOption settingsOption(QStringLiteral("settings-dir"),
                                                QCoreApplication::translate("main", "Keep the settings in an .ini file inside <dir> instead of the system settings store."),
                                                QStringLiteral("dir"));
        const QCommandLineOption quitOption(QStringLiteral("quit-after"),
                                            QCoreApplication::translate("main", "Developer option: quit after <ms> milliseconds (used by smoke tests)."),
                                            QStringLiteral("ms"));
        parser.addOption(hostOption);
        parser.addOption(portOption);
        parser.addOption(nameOption);
        parser.addOption(connectOption);
        parser.addOption(offlineOption);
        parser.addOption(settingsOption);
        parser.addOption(quitOption);
        parser.process(app);

        // Must happen before the first QSettings is created.
        if (parser.isSet(settingsOption))
        {
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, parser.value(settingsOption));
        }

        int exitCode = 0;
        {
            MainWindow window;

            ConnectPage *page = window.connectPage();
            const QString host = parser.isSet(hostOption) ? parser.value(hostOption) : page->host();
            const quint16 port = parser.isSet(portOption) ? static_cast<quint16>(parser.value(portOption).toUInt()) : page->port();
            const QString name = parser.isSet(nameOption) ? parser.value(nameOption) : page->playerName();
            page->setValues(host, port == 0 ? Protocol::DEFAULT_PORT : port, name);

            window.show();

            if (parser.isSet(connectOption))
            {
                QTimer::singleShot(0, &window, &MainWindow::connectNow);
            }
            else if (parser.isSet(offlineOption))
            {
                QTimer::singleShot(0, &window, &MainWindow::offlineNow);
            }
            if (parser.isSet(quitOption))
            {
                QTimer::singleShot(parser.value(quitOption).toInt(), &app, &QCoreApplication::quit);
            }

            exitCode = app.exec();
        }

        // A worker that is stuck in a blocking network call (e.g. connecting to an unreachable host) cannot
        // be interrupted: leave without running the static destructors that thread might still be using.
        if (NetworkController::stuckThreadCount() > 0)
        {
            std::cout.flush();
            std::fflush(nullptr); // _Exit() does not flush the C streams (spdlog writes to stdout)
            std::_Exit(exitCode);
        }
        return exitCode;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "Unknown fatal error" << std::endl;
        return 1;
    }
}
