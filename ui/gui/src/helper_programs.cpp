#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include "gui/helper_programs.hpp"

namespace
{
    /** Name of the file with the pretrained network for the standard board. */
    const char *const kNetworkFile = "connectx_6x7.cxnn";
}

QString HelperPrograms::find(const QString &baseName, const QString &directory)
{
#ifdef Q_OS_WIN
    const QString fileName = baseName + QStringLiteral(".exe");
#else
    const QString fileName = baseName;
#endif

    const QString folder = directory.isEmpty() ? QCoreApplication::applicationDirPath() : directory;
    const QFileInfo next(QDir(folder).filePath(fileName));
    if (next.isFile() && next.isExecutable())
    {
        return next.absoluteFilePath();
    }

    // (only the PATH: the current folder is not searched, a program dropped there must not be run by surprise)
    return QStandardPaths::findExecutable(baseName);
}

bool HelperPrograms::findsNetwork(const QString &computerPlayer)
{
    if (computerPlayer.isEmpty())
    {
        return false;
    }

    // The places the computer player looks in, in its own order.
    const QDir executableFolder = QFileInfo(computerPlayer).absoluteDir();
    QStringList folders;
    folders << executableFolder.filePath(QStringLiteral("models"));
    {
        QDir parent = executableFolder;
        if (parent.cdUp())
        {
            folders << parent.filePath(QStringLiteral("models"));
        }
    }
    folders << executableFolder.absolutePath();
    folders << QDir::current().filePath(QStringLiteral("models"));

    const QString environment = qEnvironmentVariable("CONNECTX_MODEL_PATH");
    if (!environment.isEmpty())
    {
        folders << environment.split(QDir::listSeparator(), Qt::SkipEmptyParts);
    }

    for (const QString &folder : std::as_const(folders))
    {
        if (QFileInfo(QDir(folder).filePath(QString::fromLatin1(kNetworkFile))).isFile())
        {
            return true;
        }
    }
    return false;
}

HelperPrograms HelperPrograms::locate()
{
    HelperPrograms found;
    found.computerPlayer = find(QStringLiteral("connectx-ai"));
    found.server = find(QStringLiteral("connectx-server"));
    found.network = findsNetwork(found.computerPlayer);
    return found;
}
