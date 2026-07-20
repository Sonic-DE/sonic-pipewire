/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "RecordMe.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

#include "xdp_dbus_screencast_interface.h"

Q_DECLARE_METATYPE(Stream)

namespace {
QString optionKeys(const QVariantMap& options)
{
    QStringList keys;
    keys.reserve(options.size());
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        keys.push_back(it.key());
    }
    keys.sort();
    return keys.join(QLatin1Char(','));
}
}

QDebug operator<<(QDebug debug, const Stream& plug)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "Stream(id: " << plug.id << ", opts: " << plug.opts << ')';
    return debug;
}


const QDBusArgument &operator<<(const QDBusArgument &argument, const Stream &/*stream*/)
{
    argument.beginStructure();
//     argument << stream.id << stream.opts;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Stream &stream)
{
    argument.beginStructure();
    argument >> stream.id >> stream.opts;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, QList<Stream> &stream)
{
    argument.beginArray();
    while ( !argument.atEnd() ) {
        Stream element;
        argument >> element;
        stream.append( element );
    }
    argument.endArray();
    return argument;
}

RecordMe::RecordMe(QObject* parent)
    : QObject(parent)
    , iface(new OrgFreedesktopPortalScreenCastInterface(
          QLatin1String("org.freedesktop.portal.Desktop"), QLatin1String("/org/freedesktop/portal/desktop"), QDBusConnection::sessionBus(), this))
    , m_durationTimer(new QTimer(this))
    , m_handleToken(QStringLiteral("Preview%1").arg(QRandomGenerator::global()->generate()))
    , m_engine(new QQmlApplicationEngine(this))
{
    m_engine->rootContext()->setContextProperty(QStringLiteral("app"), this);
    m_engine->load(QUrl(QStringLiteral("qrc:/main.qml")));
    if (m_engine->rootObjects().isEmpty()) {
        qWarning() << "Could not load portal preview UI";
        abortWithError(1);
        return;
    }

    // create session
    const auto sessionParameters = QVariantMap {
        { QLatin1String("session_handle_token"), m_handleToken },
        { QLatin1String("handle_token"), m_handleToken }
    };
    auto sessionReply = iface->CreateSession(sessionParameters);
    sessionReply.waitForFinished();
    if (sessionReply.isError()) {
        logReplyError("CreateSession", sessionReply.error());
        abortWithError(1);
        return;
    }

    const bool ret = QDBusConnection::sessionBus().connect(QString(),
                                                           sessionReply.value().path(),
                                                           QLatin1String("org.freedesktop.portal.Request"),
                                                           QLatin1String("Response"),
                                                           this,
                                                           SLOT(response(uint, QVariantMap)));
    if (!ret) {
        qWarning() << "Failed to connect portal preview request response" << sessionReply.value().path();
        abortWithError(2);
        return;
    }

    qDBusRegisterMetaType<Stream>();
    qDBusRegisterMetaType<QList<Stream>>();

    m_durationTimer->setSingleShot(true);
    connect(m_durationTimer, &QTimer::timeout, this, &RecordMe::closeSessionAndQuit);
}

RecordMe::~RecordMe() = default;

int RecordMe::exitCode() const
{
    return m_exitCode;
}

void RecordMe::init(const QDBusObjectPath& path)
{
    m_path = path;

    const uint availableSourceTypes = iface->availableSourceTypes();
    const uint availableCursorModes = iface->availableCursorModes();
    const uint version = iface->version();
    if (!validateCapabilities(availableSourceTypes,
            availableCursorModes,
            version,
            iface->service(),
            QGuiApplication::platformName(),
            qEnvironmentVariable("XDG_CURRENT_DESKTOP"))) {
        abortWithError(1);
        return;
    }

    const uint cursorMode = selectCursorMode(availableCursorModes);
    QVariantMap sourcesParameters = {{QLatin1String("handle_token"), m_handleToken},
        {QLatin1String("types"), availableSourceTypes},
        {QLatin1String("multiple"), false},
        {QLatin1String("cursor_mode"), cursorMode},
        {QLatin1String("persist_mode"), uint(m_persistMode)}};

    if (!m_restoreToken.isEmpty()) {
        sourcesParameters[QLatin1String("restore_token")] = m_restoreToken;
    }

    auto reply = iface->SelectSources(m_path, sourcesParameters);
    reply.waitForFinished();

    if (reply.isError()) {
        logReplyError("SelectSources", reply.error(), {}, m_path);
        abortWithError(1);
        return;
    }
    qDebug() << "Portal preview source selection requested" << reply.value().path() << "cursorMode" << cursorMode;
}

void RecordMe::response(uint code, const QVariantMap& results)
{
    if (code > 0) {
        qWarning() << "Portal preview request failed" << code << results;
        abortWithError(1);
        return;
    }

    if (results.contains(QLatin1String("restore_token"))) {
        qDebug() << "Restore token:" << results[QLatin1String("restore_token")].toString();
    }

    const auto streamsIt = results.constFind(QStringLiteral("streams"));
    if (streamsIt != results.constEnd()) {
        QList<Stream> streams;
        streamsIt->value<QDBusArgument>() >> streams;

        handleStreams(streams);
        return;
    }

    const auto handleIt = results.constFind(QStringLiteral("session_handle"));
    if (handleIt != results.constEnd()) {
        init(QDBusObjectPath(handleIt->toString()));
        return;
    }

    qDebug() << "params" << results << code;
    if (results.isEmpty()) {
        start();
        return;
    }
}

void RecordMe::start()
{
    const QVariantMap startParameters = {
        { QLatin1String("handle_token"), m_handleToken }
    };

    auto reply = iface->Start(m_path, QStringLiteral("org.freedesktop.RecordMe"), startParameters);
    reply.waitForFinished();

    if (reply.isError()) {
        logReplyError("Start", reply.error(), {}, m_path);
        abortWithError(1);
        return;
    }
    qDebug() << "Portal preview started" << reply.value().path();
}

void RecordMe::handleStreams(const QList<Stream> &streams)
{
    if (!validateStreams(streams)) {
        abortWithError(1);
        return;
    }

    const QVariantMap startParameters = {
        { QLatin1String("handle_token"), m_handleToken }
    };

    auto reply = iface->OpenPipeWireRemote(m_path, startParameters);
    reply.waitForFinished();

    if (reply.isError()) {
        logReplyError("OpenPipeWireRemote", reply.error(), {}, m_path);
        abortWithError(1);
        return;
    }

    PortalUniqueFd originalFd(reply.value().takeFileDescriptor());
    auto duplicatedFds = portalDuplicateFdForStreams(std::move(originalFd), streams.size(), m_duplicateFdFunction);
    if (!duplicatedFds) {
        qWarning() << "Could not duplicate PipeWire remote descriptor for portal preview streams" << streams.size();
        abortWithError(1);
        return;
    }

    const auto roots = m_engine->rootObjects();
    for (qsizetype i = 0; i < streams.size(); ++i) {
        const auto& stream = streams.at(i);
        qDebug() << "Installing portal preview stream" << stream.id << "optionKeys" << optionKeys(stream.opts);
        for (auto root : roots) {
            auto mo = root->metaObject();
            mo->invokeMethod(root,
                "addStream",
                Q_ARG(QVariant, QVariant::fromValue<quint32>(stream.id)),
                Q_ARG(QVariant, QStringLiteral("Portal stream %1").arg(stream.id)),
                Q_ARG(QVariant, duplicatedFds->at(static_cast<size_t>(i)).release()),
                Q_ARG(QVariant, true));
        }
    }

    m_streamInstalled = true;
    if (m_durationTimer->interval() > 0) {
        m_durationTimer->start();
    }
}

void RecordMe::setPersistMode(PersistMode persistMode)
{
    m_persistMode = persistMode;
}

void RecordMe::setRestoreToken(const QString &restoreToken)
{
    m_restoreToken = restoreToken;
}

void RecordMe::setDuration(int duration)
{
    m_durationTimer->setInterval(duration);
}

void RecordMe::setDuplicateFdFunction(PortalDuplicateFdFunction duplicateFdFunction)
{
    m_duplicateFdFunction = std::move(duplicateFdFunction);
}

void RecordMe::startDurationTimerForTest()
{
    if (m_durationTimer->interval() > 0 && m_streamInstalled) {
        m_durationTimer->start();
    }
}

bool RecordMe::durationTimerIsActiveForTest() const
{
    return m_durationTimer->isActive();
}

uint RecordMe::selectCursorMode(uint availableCursorModes)
{
    if (availableCursorModes & Metadata) {
        return Metadata;
    }
    if (availableCursorModes & Embedded) {
        return Embedded;
    }
    return Hidden;
}

bool RecordMe::validateCapabilities(uint availableSourceTypes, uint availableCursorModes, uint version, const QString& service, const QString& platform, const QString& desktop)
{
    qInfo() << "Portal preview capabilities"
            << "service" << service << "platform" << platform << "desktop" << desktop << "version" << version << "sourceModes" << availableSourceTypes
            << "cursorModes" << availableCursorModes;

    if (availableSourceTypes == 0) {
        qWarning() << "Portal preview cannot select sources because AvailableSourceTypes is zero";
        return false;
    }
    if (availableCursorModes == 0) {
        qWarning() << "Portal preview cannot select sources because AvailableCursorModes is zero";
        return false;
    }
    return true;
}

bool RecordMe::validateStreams(const QList<Stream>& streams)
{
    if (streams.isEmpty()) {
        qWarning() << "Portal preview received no streams";
        return false;
    }

    for (const auto& stream : streams) {
        if (stream.id == 0) {
            qWarning() << "Portal preview received invalid zero PipeWire node" << "optionKeys" << optionKeys(stream.opts);
            return false;
        }
        qDebug() << "Portal preview stream response" << stream.id << "optionKeys" << optionKeys(stream.opts);
    }
    return true;
}

void RecordMe::closeSessionAndQuit()
{
    if (!m_path.path().isEmpty()) {
        QDBusMessage closeMessage = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
            m_path.path(),
            QStringLiteral("org.freedesktop.portal.Session"),
            QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(closeMessage);
    }

    QCoreApplication::quit();
}

void RecordMe::abortWithError(int exitCode)
{
    if (m_exitCode != 0) {
        return;
    }
    m_exitCode = exitCode;
    Q_EMIT failed(exitCode);
}

bool RecordMe::logReplyError(const char* action, const QDBusError& error, const QDBusObjectPath& requestPath, const QDBusObjectPath& sessionPath) const
{
    qWarning() << "Portal preview D-Bus error" << action << "name" << error.name() << "message" << error.message() << "requestPath" << requestPath.path()
               << "sessionPath" << sessionPath.path();
    return true;
}

#include "moc_RecordMe.cpp"
