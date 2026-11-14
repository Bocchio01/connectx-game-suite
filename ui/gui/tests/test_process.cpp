// Tests of the helper processes of the GUI (the computer opponent, the private server) with a stand-in
// program (fake_helper.cpp): start failures, early exits, programs that must be stopped or killed, and
// that nothing is left behind (checked by process id, never by name).

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>

#include "gui/computer_player.hpp"
#include "gui/private_server.hpp"

#include "offscreen_support.hpp"
#include "process_support.hpp"

using ProcessSupport::EnvironmentGuard;
using ProcessSupport::isAlive;
using ProcessSupport::waitUntilGone;

namespace
{
    ComputerPlayer::Options optionsFor(const QString &program)
    {
        ComputerPlayer::Options options;
        options.program = program;
        options.host = QStringLiteral("127.0.0.1");
        options.port = 4242;
        options.gameId = 7;
        options.agent = QStringLiteral("alphabeta:depth=4");
        options.playerName = QStringLiteral("Computer (Easy)");
        options.delayMs = 500;
        return options;
    }

    /** The folder that the stand-in server reports as its working directory. */
    QString reportedFolder(const QString &output)
    {
        for (const QString &line : output.split(QLatin1Char('\n')))
        {
            if (line.startsWith(QLatin1String("CWD: ")))
            {
                return line.mid(5);
            }
        }
        return QString();
    }
}

class ProcessTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(QFileInfo::exists(ProcessSupport::fakeHelper()), "the stand-in helper (gui-fake-helper) was not built");
    }

    // =====================================================================================
    // ComputerPlayer
    // =====================================================================================
    void buildsTheCommandLine()
    {
        const QStringList arguments = ComputerPlayer::arguments(optionsFor(QStringLiteral("connectx-ai")));
        const QStringList expected = {"--host", "127.0.0.1", "--port", "4242", "-g", "7", "--agent", "alphabeta:depth=4",
                                      "-n", "Computer (Easy)", "--games", "1", "--delay", "500"};
        QCOMPARE(arguments, expected);
    }

    void reportsAProgramThatDoesNotExist()
    {
        ComputerPlayer player;
        QSignalSpy failed(&player, &ComputerPlayer::failed);
        QSignalSpy started(&player, &ComputerPlayer::started);

        player.start(optionsFor(QDir::temp().filePath(QStringLiteral("gui-surely-not-here/connectx-ai"))));

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QVERIFY2(failed.first().at(0).toString().contains(QLatin1String("Could not start")), qPrintable(failed.first().at(0).toString()));
        QCOMPARE(started.count(), 0);
        QVERIFY(!player.isRunning());
    }

    void reportsAMissingProgramName()
    {
        ComputerPlayer player;
        QSignalSpy failed(&player, &ComputerPlayer::failed);
        player.start(optionsFor(QString()));
        QCOMPARE(failed.count(), 1);
        QVERIFY(!player.isRunning());
    }

    void passesTheArgumentsToTheProgram()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");
        EnvironmentGuard echo("FAKE_HELPER_ECHO_ARGS", "1");

        ComputerPlayer player;
        QSignalSpy finished(&player, &ComputerPlayer::finished);
        const ComputerPlayer::Options options = optionsFor(ProcessSupport::fakeHelper());
        player.start(options);

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QStringList expected;
        for (const QString &argument : ComputerPlayer::arguments(options))
        {
            expected << QStringLiteral("ARG: ") + argument;
        }
        QCOMPARE(player.output().split(QLatin1Char('\n')), expected);
    }

    void anExitWithCodeZeroIsAPlayedGame()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");

        ComputerPlayer player;
        QSignalSpy finished(&player, &ComputerPlayer::finished);
        QSignalSpy failed(&player, &ComputerPlayer::failed);
        QSignalSpy stopped(&player, &ComputerPlayer::stopped);
        player.start(optionsFor(ProcessSupport::fakeHelper()));

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(stopped.count(), 0);
        QCOMPARE(player.exitCode(), 0);
        QVERIFY(!player.isRunning());
    }

    void anErrorExitIsReportedWithTheLastLines()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");
        EnvironmentGuard code("FAKE_HELPER_EXIT", "1");
        EnvironmentGuard text("FAKE_HELPER_TEXT", "Joining game 7...|Could not join the game: Game is full");

        ComputerPlayer player;
        QSignalSpy failed(&player, &ComputerPlayer::failed);
        QSignalSpy finished(&player, &ComputerPlayer::finished);
        player.start(optionsFor(ProcessSupport::fakeHelper()));

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        const QString message = failed.first().at(0).toString();
        const QString output = failed.first().at(1).toString();
        QVERIFY2(message.contains(QLatin1String("exit code 1")), qPrintable(message));
        QVERIFY2(output.contains(QLatin1String("Could not join the game: Game is full")), qPrintable(output));
        QVERIFY(output.contains(QLatin1String("Joining game 7...")));
        QCOMPARE(finished.count(), 0);
        QCOMPARE(player.exitCode(), 1);
    }

    void keepsTheLastLinesOfOutputOnly()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");
        EnvironmentGuard lines("FAKE_HELPER_LINES", "100");

        ComputerPlayer player;
        QSignalSpy finished(&player, &ComputerPlayer::finished);
        QSignalSpy lineSeen(&player, &ComputerPlayer::outputLine);
        player.start(optionsFor(ProcessSupport::fakeHelper()));

        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QCOMPARE(lineSeen.count(), 100); // everybody is told...
        const QStringList kept = player.output().split(QLatin1Char('\n'));
        QCOMPARE(kept.size(), int(HelperProcess::kMaxOutputLines)); // ...but only the end is kept
        QCOMPARE(kept.first(), QStringLiteral("line 61"));
        QCOMPARE(kept.last(), QStringLiteral("line 100"));
    }

    void stopEndsARunningProgram()
    {
        ComputerPlayer player;
        QSignalSpy started(&player, &ComputerPlayer::started);
        QSignalSpy stopped(&player, &ComputerPlayer::stopped);
        QSignalSpy failed(&player, &ComputerPlayer::failed);
        player.start(optionsFor(ProcessSupport::fakeHelper()));

        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        QVERIFY(player.isRunning());
        const qint64 pid = player.processId();
        QVERIFY(pid > 0);
        QVERIFY(isAlive(pid));

        player.stop();

        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(waitUntilGone(pid));
        QCOMPARE(failed.count(), 0); // an end that was asked for is not a failure
        QVERIFY(!player.isRunning());
    }

    void aProgramThatIgnoresTheRequestIsKilled()
    {
        EnvironmentGuard ignore("FAKE_HELPER_IGNORE_TERM", "1");

        ComputerPlayer player;
        QSignalSpy started(&player, &ComputerPlayer::started);
        QSignalSpy stopped(&player, &ComputerPlayer::stopped);
        player.start(optionsFor(ProcessSupport::fakeHelper()));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        QTest::qWait(400); // (let it install its signal handlers)
        const qint64 pid = player.processId();

        QElapsedTimer clock;
        clock.start();
        player.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(waitUntilGone(pid));
        QVERIFY2(clock.elapsed() < HelperProcess::kGracePeriodMs + 2000, qPrintable(QString::number(clock.elapsed())));
    }

    void destroyingThePlayerKillsTheProgram()
    {
        auto player = std::make_unique<ComputerPlayer>();
        QSignalSpy started(player.get(), &ComputerPlayer::started);
        player->start(optionsFor(ProcessSupport::fakeHelper()));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 pid = player->processId();
        QVERIFY(isAlive(pid));

        player.reset();

        QVERIFY(!isAlive(pid)); // at once: nothing is left running when the player is gone
    }

    void startingAgainStopsThePreviousProgram()
    {
        ComputerPlayer player;
        QSignalSpy started(&player, &ComputerPlayer::started);
        player.start(optionsFor(ProcessSupport::fakeHelper()));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 10000);
        const qint64 first = player.processId();

        player.start(optionsFor(ProcessSupport::fakeHelper()));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, 10000);
        const qint64 second = player.processId();

        QVERIFY(first != second);
        QVERIFY(!isAlive(first));
        QVERIFY(isAlive(second));
    }

    // =====================================================================================
    // PrivateServer
    // =====================================================================================
    void aServerBecomesReadyWhenItListens()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");
        EnvironmentGuard echo("FAKE_HELPER_ECHO_ARGS", "1");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        QSignalSpy failed(&server, &PrivateServer::failed);
        server.start(ProcessSupport::fakeHelper());
        QVERIFY(server.isRunning());
        QVERIFY(!server.isReady());

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(server.isReady());
        const quint16 port = server.port();
        QCOMPARE(ready.first().at(0).toUInt(), uint(port));
        QVERIFY(port != 0);

        // It listens on that port...
        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), port);
        QVERIFY2(client.waitForConnected(3000), qPrintable(client.errorString()));
        client.close();

        // ...and was asked to listen on this computer only.
        const QString output = server.output();
        QVERIFY2(output.contains(QStringLiteral("ARG: --port\nARG: %1\nARG: --bind\nARG: 127.0.0.1").arg(port)), qPrintable(output));

        // It runs in a folder of its own (its log goes there), which disappears with it.
        const QString folder = reportedFolder(output);
        QVERIFY2(!folder.isEmpty() && QDir(folder).exists(), qPrintable(output));
        QVERIFY(QFileInfo::exists(QDir(folder).filePath(QStringLiteral("server.log"))));
        const qint64 pid = server.processId();
        QVERIFY(isAlive(pid));

        server.stopAndWait();

        QVERIFY(!isAlive(pid));
        QVERIFY(!server.isRunning());
        QVERIFY(!server.isReady());
        QVERIFY2(!QDir(folder).exists(), qPrintable(folder));
    }

    void withoutTheBannerThePortIsProbed()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");
        EnvironmentGuard banner("FAKE_HELPER_BANNER", "0");
        EnvironmentGuard delay("FAKE_HELPER_DELAY_MS", "500");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        QElapsedTimer clock;
        clock.start();
        server.start(ProcessSupport::fakeHelper());

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QVERIFY2(clock.elapsed() >= 450, qPrintable(QString::number(clock.elapsed()))); // not before it listens
        QVERIFY(!server.output().contains(QLatin1String("Server started successfully")));
    }

    void givesUpWhenNothingListens()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");
        EnvironmentGuard never("FAKE_HELPER_NEVER_LISTEN", "1");

        PrivateServer server;
        server.setStartTimeout(700);
        QSignalSpy ready(&server, &PrivateServer::ready);
        QSignalSpy failed(&server, &PrivateServer::failed);
        server.start(ProcessSupport::fakeHelper());
        QTRY_VERIFY_WITH_TIMEOUT(server.processId() != 0, 10000);
        const qint64 pid = server.processId();

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY2(failed.first().at(0).toString().contains(QLatin1String("did not start listening in time")),
                 qPrintable(failed.first().at(0).toString()));
        QCOMPARE(ready.count(), 0);
        QVERIFY(waitUntilGone(pid)); // the program that never listened is not left running
        QVERIFY(!server.isRunning());
    }

    void reportsAProgramThatExitsWhileStarting()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "exit");
        EnvironmentGuard code("FAKE_HELPER_EXIT", "1");
        EnvironmentGuard text("FAKE_HELPER_TEXT", "Unknown argument: --bind");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        QSignalSpy failed(&server, &PrivateServer::failed);
        server.start(ProcessSupport::fakeHelper());

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        const QString message = failed.first().at(0).toString();
        QVERIFY2(message.contains(QLatin1String("exit code 1")), qPrintable(message));
        QVERIFY2(message.contains(QLatin1String("Unknown argument: --bind")), qPrintable(message));
        QCOMPARE(ready.count(), 0);
    }

    void reportsAServerThatCannotBeStarted()
    {
        PrivateServer server;
        QSignalSpy failed(&server, &PrivateServer::failed);
        server.start(QDir::temp().filePath(QStringLiteral("gui-surely-not-here/connectx-server")));

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QVERIFY2(failed.first().at(0).toString().contains(QLatin1String("Could not start")), qPrintable(failed.first().at(0).toString()));
        QVERIFY(!server.isRunning());

        PrivateServer none;
        QSignalSpy noneFailed(&none, &PrivateServer::failed);
        none.start(QString());
        QCOMPARE(noneFailed.count(), 1);
    }

    void tellsWhenTheServerDiesAfterItWasReady()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");
        EnvironmentGuard die("FAKE_HELPER_DIE_AFTER_MS", "300");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        QSignalSpy failed(&server, &PrivateServer::failed);
        QSignalSpy died(&server, &PrivateServer::died);
        server.start(ProcessSupport::fakeHelper());

        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(died.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!server.isReady());
        QVERIFY(!server.isRunning());
    }

    void stopEndsTheServerQuietly()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        QSignalSpy failed(&server, &PrivateServer::failed);
        QSignalSpy died(&server, &PrivateServer::died);
        server.start(ProcessSupport::fakeHelper());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        const qint64 pid = server.processId();

        server.stop();

        QVERIFY(waitUntilGone(pid, 5000));
        QTRY_VERIFY_WITH_TIMEOUT(!server.isRunning(), 3000);
        QCOMPARE(failed.count(), 0); // an end that was asked for is neither a failure nor a death
        QCOMPARE(died.count(), 0);
        QVERIFY(!server.isReady());
    }

    void stopDuringTheStartLeavesNothing()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");
        EnvironmentGuard delay("FAKE_HELPER_DELAY_MS", "3000");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        QSignalSpy failed(&server, &PrivateServer::failed);
        server.start(ProcessSupport::fakeHelper());
        QTRY_VERIFY_WITH_TIMEOUT(server.processId() != 0, 10000);
        const qint64 pid = server.processId();

        server.stop(); // (the "Cancel" of the Connect page)

        QVERIFY(waitUntilGone(pid, 5000));
        QTest::qWait(300);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!server.isRunning());
    }

    void destroyingTheServerKillsIt()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");

        auto server = std::make_unique<PrivateServer>();
        QSignalSpy ready(server.get(), &PrivateServer::ready);
        server->start(ProcessSupport::fakeHelper());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        const qint64 pid = server->processId();
        QVERIFY(isAlive(pid));

        server.reset();

        QVERIFY(!isAlive(pid));
    }

    void startingAgainReplacesTheServer()
    {
        EnvironmentGuard mode("FAKE_HELPER_MODE", "serve");

        PrivateServer server;
        QSignalSpy ready(&server, &PrivateServer::ready);
        server.start(ProcessSupport::fakeHelper());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        const qint64 first = server.processId();

        server.start(ProcessSupport::fakeHelper());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        const qint64 second = server.processId();

        QVERIFY(first != second);
        QVERIFY(!isAlive(first));
        QVERIFY(isAlive(second));
    }
};

int main(int argc, char *argv[])
{
    OffscreenSupport::prepare();
    QCoreApplication app(argc, argv);

    ProcessTest test;
    return OffscreenSupport::runTest(test, argc, argv);
}

#include "test_process.moc"
