﻿/***************************************************************************
 *   Copyright (C) 2008 by Max-Wilhelm Bruker   *
 *   brukie@gmx.net   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "main.h"

#include "QtNetwork/QNetworkInterface"
#include "applicationinstancemanager.h"
#include "carddatabase.h"
#include "dlg_settings.h"
#include "featureset.h"
#include "logger.h"
#include "pixmapgenerator.h"
#include "qtlocalpeer/qtlocalpeer.h"
#include "rng_sfmt.h"
#include "settingscache.h"
#include "soundengine.h"
#include "spoilerbackgroundupdater.h"
#include "thememanager.h"
#include "version_string.h"
#include "window_main.h"

#include <QApplication>
#include <QAtomicInteger>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLibraryInfo>
#include <QList>
#include <QLocale>
#include <QSystemTrayIcon>
#include <QTextCodec>
#include <QTextStream>
#include <QThread>
#include <QTranslator>
#include <QtPlugin>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

CardDatabase *db;
QTranslator *translator, *qtTranslator;
RNG_Abstract *rng;
SoundEngine *soundEngine;
QSystemTrayIcon *trayIcon;
ThemeManager *themeManager;

const QString translationPrefix = "cockatrice";
QString translationPath;

static void CockatriceLogger(QtMsgType type, const QMessageLogContext &ctx, const QString &message)
{
    Logger::getInstance().log(type, ctx, message);
}

void installNewTranslator()
{
    QString lang = SettingsCache::instance().getLang();

    qtTranslator->load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath));
    qApp->installTranslator(qtTranslator);
    translator->load(translationPrefix + "_" + lang, translationPath);
    qApp->installTranslator(translator);
    qDebug() << "Language changed:" << lang;
}

static QString const getUserIDString()
{
    QString uid = "0";
#ifdef Q_OS_WIN
    const int UNLEN = 256;
    WCHAR buffer[UNLEN + 1] = {0};
    DWORD buffer_len = sizeof(buffer) / sizeof(*buffer);
    if (GetUserNameW(buffer, &buffer_len))
        uid = QString::fromWCharArray(buffer);
#else
    uid = QString::number(getuid());
#endif
    return uid;
}

QString const generateClientID()
{
    QString macList;
    foreach (QNetworkInterface interface, QNetworkInterface::allInterfaces()) {
        if (interface.hardwareAddress() != "")
            if (interface.hardwareAddress() != "00:00:00:00:00:00:00:E0")
                macList += interface.hardwareAddress() + ".";
    }
    QString strClientID = QCryptographicHash::hash(macList.toUtf8(), QCryptographicHash::Sha1).toHex().right(15);
    return strClientID;
}

int main(int argc, char *argv[])
{
    // Start trice client
    QApplication app(argc, argv);

    qDebug("Starting instance manager");

    // Create an instance manager
    const QString appId = QLatin1String("cockatrice-") + getUserIDString() + '-' + VERSION_STRING;
    ApplicationInstanceManager *m_instanceManager = new ApplicationInstanceManager(appId, &app);

    // Check if session zero. If it is then open all of the inputs in this tab
    bool openInNewClient = m_instanceManager->isFirstInstance();
    QList<QString *> replays;
    QList<QString *> decks;
    QString xSchemeHandle;
    bool xSchemeFlag = false;

    // Check for args. argv[0] is the command.
    if (argc > 1) {
        // Check the args for opening decks, replays or game joins
        // See cockatrice-cockatrice.xml for the MIME reference.
        const char *xScheme = "cockatrice://";
        int xSchemeLen = strlen(xScheme);

        for (int i = 1; i < argc; i++) {
            qDebug() << "Processing arg " << argv[i];

            // Check arg matches cockatrice://*
            bool isXSchemeHandle = true;

            for (int j = 0; (isXSchemeHandle = argv[i][j] != 0 && argv[i][j] == xScheme[j]) && j < xSchemeLen; j++)
                ;

            if (!isXSchemeHandle) {
                // Check arg matches *.co(r|d) via a finite state machine
                int state = 0;
                /*
                 * states transitions
                 * 0 -> 1 if .
                 * 1 -> 2 if c else 1 -> 0
                 * 2 -> 3 if o else 2 -> 0
                 * 3 -> 4 if r or d else 3 -> 0
                 * 4 -> 0 if it is not an epsilon transition (accepting state)
                 */

                bool isDeckFile = 1;

                for (int j = 0; argv[i][j] != 0; j++) {
                    switch (argv[i][j]) {
                        // 0 -> 1 if .
                        case '.':
                            state = 1;
                            break;
                        // 1 -> 2 if c
                        case 'c':
                            if (state == 1) {
                                state = 2;
                            } else {
                                state = 0;
                            }
                            break;
                        // 2 -> 3 if o
                        case 'o':
                            if (state == 2) {
                                state = 3;
                            } else {
                                state = 0;
                            }
                            break;
                        // 2 -> 3 if o
                        case 'd':
                            isDeckFile = true; // Dear linter, this statement should fall through
                        case 'r':
                            if (state == 3) {
                                state = 4;
                            } else {
                                state = 0;
                            }
                            break;
                        // Reset state (all else statements from the above definition)
                        default:
                            state = 0;
                            isDeckFile = false;
                            break;
                    }
                }

                // If accepting state
                if (state == 4) {
                    QString path = "";
                    if (!QString(argv[i]).contains('/')) {
                        path = QDir::currentPath() + "/";
                    }

                    if (isDeckFile) {
                        qDebug() << "Deck detected " << argv[i];
                        decks.append(new QString(path + argv[i]));
                    } else {
                        qDebug() << "Replay detected " << argv[i];
                        replays.append(new QString(path + argv[i]));
                    }
                }
            } else {
                if (isXSchemeHandle) {
                    qDebug() << "xSchemeHandle detected " << argv[i];
                    xSchemeHandle = QString(argv[i]);
                    xSchemeFlag = true;
                } else {
                    qWarning("Cockatrice only supports one xScheme-handler uri at a time.");
                }
            }
        }

        // Trigger opening in other instances
        if ((!openInNewClient) && xSchemeFlag) {
            qDebug("Sending xScheme handles");

            QAtomicInteger<int> msgReceived;
            msgReceived.storeRelaxed(false);

            // Create callback to set flag to true
            QObject::connect(m_instanceManager, &ApplicationInstanceManager::messageReceived,
                             [&msgReceived](const QString &msg, QObject *socket) {
                                 if (msg == "connected") {
                                     msgReceived.storeRelaxed(true);
                                     qDebug("xSchemeHandle callback from another instance");
                                 }
                             });

            // Send xScheme-handle. Each instance that has the same URL should respond
            emit(m_instanceManager, SIGNAL(sendMessage), ("xscheme:" + xSchemeHandle), 100);

            // Wait some time for a callback
            QThread::msleep(10);

            // Disconnect the lambda
            m_instanceManager->disconnect(SIGNAL(messageReceived()));

            // If no responses to the xScheme-handle open it in the client
            openInNewClient = !msgReceived.loadRelaxed();
        }

        // Send deck and replay requests. The first instance should then open them
        if (!openInNewClient) {
            qDebug("Sending deck replay handles");

            for (QString *deck : decks) {
                m_instanceManager->sendMessage("deck:" + *deck, 100);
                delete deck;
            }

            for (QString *replay : replays) {
                m_instanceManager->sendMessage("replay:" + *replay, 100);
                delete replay;
            }
        }
    }

    if (!openInNewClient) {
        qDebug("Opening in an another instance.");
    } else {
        qDebug("Opening in a new instance.");
        QObject::connect(&app, SIGNAL(lastWindowClosed()), &app, SLOT(quit()));

        qInstallMessageHandler(CockatriceLogger);

#ifdef Q_OS_WIN
        app.addLibraryPath(app.applicationDirPath() + "/plugins");
#endif

        // These values are only used by the settings loader/saver
        // Wrong or outdated values are kept to not break things
        QCoreApplication::setOrganizationName("Cockatrice");
        QCoreApplication::setOrganizationDomain("cockatrice.de");
        QCoreApplication::setApplicationName("Cockatrice");
        QCoreApplication::setApplicationVersion(VERSION_STRING);

#ifdef Q_OS_MAC
        qApp->setAttribute(Qt::AA_DontShowIconsInMenus, true);
#endif

#ifdef Q_OS_MAC
        translationPath = qApp->applicationDirPath() + "/../Resources/translations";
#elif defined(Q_OS_WIN)
        translationPath = qApp->applicationDirPath() + "/translations";
#else // linux
        translationPath = qApp->applicationDirPath() + "/../share/cockatrice/translations";
#endif

        QCommandLineParser parser;
        parser.setApplicationDescription("Cockatrice");
        parser.addHelpOption();
        parser.addVersionOption();

        parser.addOptions(
            {{{"c", "connect"}, QCoreApplication::translate("main", "Connect on startup"), "user:pass@host:port"},
             {{"d", "debug-output"}, QCoreApplication::translate("main", "Debug to file")}});

        parser.process(app);

        if (parser.isSet("debug-output")) {
            Logger::getInstance().logToFile(true);
        }

        rng = new RNG_SFMT;
        themeManager = new ThemeManager;
        soundEngine = new SoundEngine;
        db = new CardDatabase;

        qtTranslator = new QTranslator;
        translator = new QTranslator;
        installNewTranslator();

        QLocale::setDefault(QLocale::English);
        qDebug("main(): starting main program");

        MainWindow ui(m_instanceManager);
        qDebug("main(): MainWindow constructor finished");

        ui.setWindowIcon(QPixmap("theme:cockatrice"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
        // set name of the app desktop file; used by wayland to load the window icon
        QGuiApplication::setDesktopFileName("cockatrice");
#endif

        SettingsCache::instance().setClientID(generateClientID());

        // If spoiler mode is enabled, we will download the spoilers
        // then reload the DB. otherwise just reload the DB
        SpoilerBackgroundUpdater spoilerBackgroundUpdater;

        ui.show();
        qDebug("main(): ui.show() finished");

        if (xSchemeFlag) {
            ui.processInterProcessCommunication("xscheme:" + xSchemeHandle, nullptr);
        }

        for (QString *deck : decks) {
            ui.processInterProcessCommunication("deck:" + *deck, nullptr);
            delete deck;
        }

        for (QString *replay : replays) {
            ui.processInterProcessCommunication("replay:" + *replay, nullptr);
            delete replay;
        }
        qDebug("main(): Passed file/xScheme handles to ui");

        if (parser.isSet("connect")) {
            ui.setConnectTo(parser.value("connect"));
        }

        app.setAttribute(Qt::AA_UseHighDpiPixmaps);
        app.exec();

        qDebug("Event loop finished, terminating...");
        delete db;
        delete rng;
        PingPixmapGenerator::clear();
        CountryPixmapGenerator::clear();
        UserLevelPixmapGenerator::clear();
    }

    delete m_instanceManager;

    return 0;
}
