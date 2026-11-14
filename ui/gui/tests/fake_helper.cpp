// A stand-in for the helper programs (connectx-ai, connectx-server) used by the tests of the GUI: it does
// what the environment variables tell it to, so that the process handling can be tested without the real
// programs: a program that cannot join a game, one that ignores the request to stop, a "server" that
// listens late, or never, or dies.
//
//   FAKE_HELPER_MODE         run (default: print, then wait to be stopped) | exit | serve
//   FAKE_HELPER_TEXT         lines to print first, separated by '|'
//   FAKE_HELPER_LINES        print "line 1" ... "line N" first
//   FAKE_HELPER_ECHO_ARGS    1: print "ARG: <argument>" for every command line argument first
//   FAKE_HELPER_EXIT         exit mode: the exit code (default 0)
//   FAKE_HELPER_IGNORE_TERM  1: ignore SIGTERM and SIGINT (a program that does not stop when asked)
//   serve mode (the arguments of connectx-server are understood: --port/-p and --bind/-b):
//   FAKE_HELPER_DELAY_MS     wait this long before listening
//   FAKE_HELPER_BANNER       0: do not print the "Server started successfully" banner when listening
//   FAKE_HELPER_DIE_AFTER_MS exit with code 1 this long after the program started listening
//   FAKE_HELPER_NEVER_LISTEN 1: never listen (the program just runs)

#include <csignal>
#include <iostream>
#include <string>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace
{
    void say(const QString &text)
    {
        std::cout << text.toUtf8().constData() << std::endl; // (endl flushes: the reader sees it at once)
    }

    QString variable(const char *name, const QString &fallback = QString())
    {
        return qEnvironmentVariableIsSet(name) ? qEnvironmentVariable(name) : fallback;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList arguments = app.arguments().mid(1);

    if (variable("FAKE_HELPER_IGNORE_TERM") == QLatin1String("1"))
    {
        std::signal(SIGTERM, SIG_IGN);
        std::signal(SIGINT, SIG_IGN);
    }

    if (variable("FAKE_HELPER_ECHO_ARGS") == QLatin1String("1"))
    {
        for (const QString &argument : arguments)
        {
            say(QStringLiteral("ARG: ") + argument);
        }
    }

    const int lines = variable("FAKE_HELPER_LINES", QStringLiteral("0")).toInt();
    for (int i = 1; i <= lines; ++i)
    {
        say(QStringLiteral("line %1").arg(i));
    }

    for (const QString &line : variable("FAKE_HELPER_TEXT").split(QLatin1Char('|'), Qt::SkipEmptyParts))
    {
        say(line);
    }

    const QString mode = variable("FAKE_HELPER_MODE", QStringLiteral("run"));

    if (mode == QLatin1String("exit"))
    {
        return variable("FAKE_HELPER_EXIT", QStringLiteral("0")).toInt();
    }

    if (mode == QLatin1String("serve") && variable("FAKE_HELPER_NEVER_LISTEN") != QLatin1String("1"))
    {
        quint16 port = 0;
        QHostAddress address = QHostAddress::Any;
        for (int i = 0; i + 1 < arguments.size(); ++i)
        {
            if (arguments[i] == QLatin1String("--port") || arguments[i] == QLatin1String("-p"))
            {
                port = static_cast<quint16>(arguments[i + 1].toUInt());
            }
            else if (arguments[i] == QLatin1String("--bind") || arguments[i] == QLatin1String("-b"))
            {
                address = QHostAddress(arguments[i + 1]);
            }
        }

        say(QStringLiteral("CWD: ") + QDir::currentPath());

        // The real server writes its log into the current folder: so does the stand-in, to show that the
        // GUI gives it a folder of its own and removes it afterwards.
        QFile log(QStringLiteral("server.log"));
        if (log.open(QIODevice::WriteOnly))
        {
            log.write("fake server\n");
            log.close();
        }

        QTcpServer server;
        QTimer::singleShot(variable("FAKE_HELPER_DELAY_MS", QStringLiteral("0")).toInt(), &app, [&]()
                           {
            if (!server.listen(address, port))
            {
                say(QStringLiteral("Error creating acceptor: ") + server.errorString());
                QCoreApplication::exit(1);
                return;
            }

            QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]()
                             {
                while (QTcpSocket *socket = server.nextPendingConnection())
                {
                    socket->close();
                    socket->deleteLater();
                } });

            if (variable("FAKE_HELPER_BANNER", QStringLiteral("1")) != QLatin1String("0"))
            {
                say(QStringLiteral("Server started successfully on port %1").arg(server.serverPort()));
            }

            const int dieAfter = variable("FAKE_HELPER_DIE_AFTER_MS", QStringLiteral("0")).toInt();
            if (dieAfter > 0)
            {
                QTimer::singleShot(dieAfter, &app, []()
                                   { QCoreApplication::exit(1); });
            } });

        return app.exec();
    }

    // run: nothing to do but wait until somebody stops us
    return app.exec();
}
