/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "RecordMe.h"
#include <QCommandLineParser>
#include <QGuiApplication>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    {
        QCommandLineParser parser;
        QCommandLineOption duration(QStringLiteral("duration"), QStringLiteral("milliseconds length of the preview"), QStringLiteral("duration"));
        parser.addOption(duration);

        QCommandLineOption persistMode(QStringLiteral("persistMode"),
                                       QStringLiteral("persistMode. Integer value as per the DBus specification"),
                                       QStringLiteral("persistMode"));
        parser.addOption(persistMode);

        QCommandLineOption restoreToken(QStringLiteral("restoreToken"), QStringLiteral("Restore token"), QStringLiteral("restoreToken"));
        parser.addOption(restoreToken);

        parser.addHelpOption();
        parser.process(app);

        RecordMe *me = new RecordMe(&app);
        QObject::connect(me, &RecordMe::failed, &app, [&app](int exitCode) {
            app.exit(exitCode);
        });
        if (parser.isSet(duration)) {
            me->setDuration(parser.value(duration).toInt());
        }
        if (parser.isSet(persistMode)) {
            int rawPersitValue = parser.value(persistMode).toInt();
            auto mode = static_cast<RecordMe::PersistMode>(rawPersitValue);
            me->setPersistMode(mode);
        }
        if (parser.isSet(restoreToken)) {
            me->setRestoreToken(parser.value(restoreToken));
        }
        if (me->exitCode() != 0) {
            return me->exitCode();
        }
    }

    return app.exec();
}
