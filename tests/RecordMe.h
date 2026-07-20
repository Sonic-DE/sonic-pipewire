/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "portalfdutils.h"

#include <QObject>
#include <QDBusObjectPath>

class OrgFreedesktopPortalScreenCastInterface;
class QDBusError;
class QTimer;
class QQmlApplicationEngine;

struct Stream {
    uint id;
    QVariantMap opts;
};

class OrgFreedesktopPortalScreenCastInterface;

class RecordMe : public QObject
{
    Q_OBJECT
public:
    RecordMe(QObject* parent = nullptr);
    ~RecordMe() override;

    enum CursorModes {
        Hidden = 1,
        Embedded = 2,
        Metadata = 4
    };
    Q_ENUM(CursorModes);

    enum SourceTypes {
        Monitor = 1,
        Window = 2
    };
    Q_ENUM(SourceTypes);

    enum PersistMode {
        NoPersist = 0,
        PersistWhileRunning = 1,
        PersistUntilRevoked = 2,
    };
    Q_ENUM(PersistMode)

    void setDuration(int duration);
    void setPersistMode(PersistMode persistMode);
    void setRestoreToken(const QString &restoreToken);
    int exitCode() const;

Q_SIGNALS:
    void failed(int exitCode);

public:
    void setDuplicateFdFunction(PortalDuplicateFdFunction duplicateFdFunction);
    void startDurationTimerForTest();
    bool durationTimerIsActiveForTest() const;

    static uint selectCursorMode(uint availableCursorModes);
    static bool validateCapabilities(uint availableSourceTypes, uint availableCursorModes, uint version, const QString& service, const QString& platform, const QString& desktop);
    static bool validateStreams(const QList<Stream>& streams);

public Q_SLOTS:
    void response(uint code, const QVariantMap &results);

private:
    void init(const QDBusObjectPath &path);
    void handleStreams(const QList<Stream> &streams);
    void start();
    void closeSessionAndQuit();
    void abortWithError(int exitCode);
    bool logReplyError(const char* action, const QDBusError& error, const QDBusObjectPath& requestPath = {}, const QDBusObjectPath& sessionPath = {}) const;

    OrgFreedesktopPortalScreenCastInterface *iface;
    QDBusObjectPath m_path;
    QTimer* const m_durationTimer;
    const QString m_handleToken;
    QQmlApplicationEngine* m_engine;
    PersistMode m_persistMode = NoPersist;
    QString m_restoreToken;
    int m_exitCode = 0;
    PortalDuplicateFdFunction m_duplicateFdFunction = portalDuplicateFd;
    bool m_streamInstalled = false;
};
