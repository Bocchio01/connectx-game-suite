#pragma once

#include <cstdio>

#ifdef _WIN32
#include <windows.h> // RegDeleteTreeW (WIN32_LEAN_AND_MEAN is defined by the build: no winsock.h here)
#endif

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QSettings>
#include <QStringList>
#include <QTest>
#include <QtGlobal>

/**
 * Helpers to run (and take pictures of) widgets on the "offscreen" Qt platform, which has no
 * desktop to take the fonts and the settings from.
 */
namespace OffscreenSupport
{
    /** Call before creating the QApplication: selects the offscreen platform (unless told otherwise). */
    inline void prepare()
    {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
#ifdef Q_OS_WIN
        // On Windows the offscreen platform only knows the fonts of this directory when asked to.
        if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        {
            qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
        }
#endif
    }

    /**
     * Call after creating the QApplication. Without a desktop Qt may pick any installed font as the
     * default one (even a barcode font): choose a readable one, so that pictures can be read.
     */
    inline void chooseFont()
    {
        const QStringList wanted = {QStringLiteral("Segoe UI"), QStringLiteral("Arial"), QStringLiteral("Tahoma"),
                                    QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Sans"),
                                    QStringLiteral("Noto Sans"), QStringLiteral("Helvetica")};
        const QStringList families = QFontDatabase::families();
        for (const QString &name : wanted)
        {
            if (families.contains(name))
            {
                QFont font(name);
                font.setPointSize(10);
                QApplication::setFont(font);
                return;
            }
        }
    }

    /** A dark palette (a typical "dark mode"), to check that the interface follows the system colours. */
    inline QPalette darkPalette()
    {
        QPalette p;
        p.setColor(QPalette::Window, QColor(0x2B, 0x2D, 0x31));
        p.setColor(QPalette::WindowText, QColor(0xE6, 0xE6, 0xE6));
        p.setColor(QPalette::Base, QColor(0x1F, 0x21, 0x24));
        p.setColor(QPalette::AlternateBase, QColor(0x27, 0x29, 0x2D));
        p.setColor(QPalette::ToolTipBase, QColor(0x1F, 0x21, 0x24));
        p.setColor(QPalette::ToolTipText, QColor(0xE6, 0xE6, 0xE6));
        p.setColor(QPalette::Text, QColor(0xE6, 0xE6, 0xE6));
        p.setColor(QPalette::PlaceholderText, QColor(0x9A, 0x9C, 0xA0));
        p.setColor(QPalette::Button, QColor(0x3A, 0x3C, 0x41));
        p.setColor(QPalette::ButtonText, QColor(0xE6, 0xE6, 0xE6));
        p.setColor(QPalette::BrightText, QColor(0xFF, 0x55, 0x55));
        p.setColor(QPalette::Mid, QColor(0x55, 0x58, 0x5E));
        p.setColor(QPalette::Highlight, QColor(0x3D, 0x8B, 0xFD));
        p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x7A, 0x7C, 0x80));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x7A, 0x7C, 0x80));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x7A, 0x7C, 0x80));
        return p;
    }

    /**
     * Keeps the QSettings of the test away from the real ones (other organisation name).
     * On Windows the native store (the registry) is used: the .ini files of QSettings occasionally
     * take seconds to be written there (file locking, virus scanners), which makes the timing of the
     * tests unpredictable. Elsewhere an .ini file in `directory` is used.
     * Each test program gets a key of its own (the process id is part of the name): programs that run at
     * the same time (ctest -j, two sessions on one machine) must not read each other's settings.
     * Call after creating the application, and removeIsolatedSettings() before the program ends.
     */
    inline void useIsolatedSettings(const QString &directory)
    {
        QCoreApplication::setOrganizationName(QStringLiteral("Bocchio01-test"));
#ifdef Q_OS_WIN
        Q_UNUSED(directory);
        QCoreApplication::setApplicationName(QStringLiteral("ConnectX-test-%1").arg(QCoreApplication::applicationPid()));
        QSettings::setDefaultFormat(QSettings::NativeFormat);
        QSettings().clear();
#else
        QCoreApplication::setApplicationName(QStringLiteral("ConnectX-test"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory);
#endif
    }

    /** Removes what useIsolatedSettings() left in the registry (nothing to do for .ini files in a temporary directory). */
    inline void removeIsolatedSettings()
    {
#ifdef Q_OS_WIN
        {
            QSettings settings;
            settings.clear();
            settings.sync();
        }
        // (native separators: no backslash has to be written in the source)
        const QString application = QDir::toNativeSeparators(QStringLiteral("Software/Bocchio01-test/") + QCoreApplication::applicationName());
        RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(application.utf16()));
        // The key of the organisation goes too when it is the last one in it (it is refused while others are in use).
        const QString organisation = QDir::toNativeSeparators(QStringLiteral("Software/Bocchio01-test"));
        RegDeleteKeyW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(organisation.utf16()));
#endif
    }

    /**
     * Runs a test object. On some Windows setups QtTest sends its report to the debugger instead of
     * stdout, which leaves the console (and the ctest log) empty: the report is written to a file
     * and copied to stdout from here. Unless -o is given explicitly.
     */
    inline int runTest(QObject &test, int argc, char *argv[])
    {
        QStringList arguments;
        for (int i = 0; i < argc; ++i)
        {
            arguments << QString::fromLocal8Bit(argv[i]);
        }

        QString reportPath;
        if (!arguments.contains(QStringLiteral("-o")))
        {
            reportPath = QDir::temp().filePath(QStringLiteral("gui-test-%1.txt").arg(QCoreApplication::applicationPid()));
            arguments << QStringLiteral("-o") << reportPath + QStringLiteral(",txt");
        }

        const int result = QTest::qExec(&test, arguments);

        if (!reportPath.isEmpty())
        {
            QFile report(reportPath);
            if (report.open(QIODevice::ReadOnly))
            {
                const QByteArray text = report.readAll();
                std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
                std::fflush(stdout);
                report.close();
                report.remove();
            }
        }
        return result;
    }
}
