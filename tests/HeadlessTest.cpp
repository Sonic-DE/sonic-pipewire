/*
    SPDX-FileCopyrightText: 2022 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <KSignalHandler>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusError>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>

#include "headlessportalutils.h"
#include "portalfdutils.h"
#include "xdp_dbus_remotedesktop_interface.h"
#include "xdp_dbus_screencast_interface.h"

#include <DmaBufHandler>
#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>

using namespace Qt::StringLiterals;

namespace {
bool s_encodedStream = false;
std::optional<Fraction> s_framerate;
std::optional<QByteArray> s_encoder;
std::optional<uint> s_requestedCursorMode;
std::optional<int> s_duration;
QList<QPointer<PipeWireEncodedStream>> s_encodedStreams;
QList<QPointer<PipeWireSourceStream>> s_sourceStreams;
bool s_shutdownStarted = false;

QString createHandleToken()
{
    return QStringLiteral("kpipewireheadlesstest%1").arg(QRandomGenerator::global()->generate());
}

void failHeadless(const QString& message, const QDBusError& error = {})
{
    if (error.isValid()) {
        qWarning() << message << error.name() << error.message();
    } else {
        qWarning() << message;
    }
    QCoreApplication::exit(1);
}

bool connectPortalRequest(const QDBusObjectPath& requestPath, QObject* receiver, const char* slot, const QString& operation, const QDBusObjectPath& sessionPath = {})
{
    const bool connected = QDBusConnection::sessionBus().connect(QString(),
        requestPath.path(),
        QStringLiteral("org.freedesktop.portal.Request"),
        QStringLiteral("Response"),
        receiver,
        slot);
    if (!connected) {
        qWarning() << "Failed to connect portal request response" << operation << "request" << requestPath.path() << "session" << sessionPath.path();
    }
    return connected;
}

bool streamsStillStopping()
{
    for (const auto& stream : std::as_const(s_encodedStreams)) {
        if (stream && stream->state() != PipeWireEncodedStream::Idle) {
            return true;
        }
    }
    return false;
}

void maybeQuitAfterShutdown()
{
    if (s_shutdownStarted && !streamsStillStopping()) {
        QCoreApplication::quit();
    }
}

void beginOrderlyShutdown()
{
    if (s_shutdownStarted) {
        return;
    }
    s_shutdownStarted = true;

    for (const auto& stream : std::as_const(s_encodedStreams)) {
        if (stream && stream->state() != PipeWireEncodedStream::Idle) {
            stream->stop();
        }
    }
    for (const auto& stream : std::as_const(s_sourceStreams)) {
        if (stream) {
            stream->setActive(false);
        }
    }

    QTimer::singleShot(5000, qGuiApp, [] {
        qWarning() << "Timed out waiting for PipeWire streams to stop; quitting headless portal test";
        QCoreApplication::quit();
    });
    maybeQuitAfterShutdown();
}

void startDurationTimerIfRequested()
{
    if (!s_duration || *s_duration <= 0) {
        return;
    }

    auto timer = new QTimer(qGuiApp);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, qGuiApp, &beginOrderlyShutdown);
    timer->start(*s_duration);
    qInfo() << "Headless portal duration timer started" << *s_duration << "milliseconds";
}

PipeWireBaseEncodedStream::Encoder encoderFromOption(const QByteArray& encoderName)
{
    if (encoderName == QByteArray("H264Main")) {
        return PipeWireBaseEncodedStream::H264Main;
    }
    if (encoderName == QByteArray("H264Baseline")) {
        return PipeWireBaseEncodedStream::H264Baseline;
    }
    if (encoderName == QByteArray("VP8")) {
        return PipeWireBaseEncodedStream::VP8;
    }
    if (encoderName == QByteArray("VP9")) {
        return PipeWireBaseEncodedStream::VP9;
    }
    return PipeWireBaseEncodedStream::NoEncoder;
}

bool createPipeWireStream(uint nodeId, PortalUniqueFd fd = {})
{
    if (nodeId == 0) {
        qWarning() << "Refusing to create a PipeWire stream for zero node id";
        return false;
    }

    if (s_encodedStream) {
        auto encoded = new PipeWireEncodedStream(qGuiApp);
        encoded->setNodeId(nodeId);
        if (fd.isValid()) {
            encoded->setFd(uint(fd.release()));
        }
        if (s_framerate) {
            encoded->setMaxFramerate(*s_framerate);
        }
        if (s_encoder) {
            encoded->setEncoder(encoderFromOption(*s_encoder));
        }

        QObject::connect(encoded, &PipeWireEncodedStream::newPacket, qGuiApp, [](const PipeWireEncodedStream::Packet &packet) {
            qDebug() << "packet received" << packet.data().size() << "key:" << packet.isKeyFrame();
        });
        QObject::connect(encoded, &PipeWireEncodedStream::cursorChanged, qGuiApp, [](const PipeWireCursor &cursor) {
            qDebug() << "cursor received. position:" << cursor.position << "hotspot:" << cursor.hotspot << "image:" << cursor.texture;
        });
        QObject::connect(encoded, &PipeWireEncodedStream::stateChanged, qGuiApp, [encoded]() {
            switch (encoded->state()) {
            case PipeWireEncodedStream::Recording:
                qDebug() << "Started headless portal encoding";
                break;
            case PipeWireEncodedStream::Rendering:
                qDebug() << "Stopped headless portal encoding, flushing remaining frames";
                break;
            case PipeWireEncodedStream::Idle:
                qDebug() << "Headless portal encoding idle";
                maybeQuitAfterShutdown();
                break;
            }
        });
        QObject::connect(encoded, &PipeWireEncodedStream::errorFound, qGuiApp, [](const QString& error) {
            qWarning() << "Headless portal encoded stream error" << error;
            QCoreApplication::exit(1);
        });

        s_encodedStreams.push_back(encoded);
        encoded->start();
        return true;
    }

    auto pwStream = new PipeWireSourceStream(qGuiApp);
    pwStream->setAllowDmaBuf(false);
    if (s_framerate) {
        pwStream->setMaxFramerate(*s_framerate);
    }
    if (!pwStream->createStream(nodeId, fd.isValid() ? fd.get() : 0)) {
        qWarning() << "failed to create headless portal PipeWire stream" << nodeId << pwStream->error();
        delete pwStream;
        return false;
    }
    if (fd.isValid()) {
        fd.release();
    }

    auto handler = std::make_shared<DmaBufHandler>();
    QObject::connect(pwStream, &PipeWireSourceStream::frameReceived, qGuiApp, [handler, pwStream](const PipeWireFrame &frame) {
        if (frame.dmabuf) {
            QImage qimage(pwStream->size(), QImage::Format_RGBA8888);
            if (!handler->downloadFrame(qimage, frame)) {
                qDebug() << "failed to download frame";
                pwStream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            } else {
                qDebug() << "dmabuf" << frame.format;
            }
        } else if (frame.dataFrame) {
            qDebug() << "image" << frame.dataFrame->format << frame.format;
        } else {
            qDebug() << "no-frame";
        }
    });
    QObject::connect(pwStream, &PipeWireSourceStream::stateChanged, qGuiApp, [](pw_stream_state state, pw_stream_state) {
        if (state == PW_STREAM_STATE_UNCONNECTED) {
            maybeQuitAfterShutdown();
        }
    });

    s_sourceStreams.push_back(pwStream);
    return true;
}

bool installPortalStreams(const HeadlessPortalStreams& streams, QDBusUnixFileDescriptor pipewireFd)
{
    QString validationError;
    if (!validateHeadlessPortalStreams(streams, &validationError)) {
        qWarning() << "Invalid portal stream response" << validationError;
        return false;
    }
    if (!pipewireFd.isValid()) {
        qWarning() << "Portal did not return a valid PipeWire remote descriptor";
        return false;
    }

    PortalUniqueFd originalFd(pipewireFd.takeFileDescriptor());
    auto duplicatedFds = portalDuplicateFdForStreams(std::move(originalFd), streams.size(), portalDuplicateFd);
    if (!duplicatedFds) {
        qWarning() << "Could not duplicate PipeWire remote descriptor for headless portal streams" << streams.size();
        return false;
    }

    for (qsizetype i = 0; i < streams.size(); ++i) {
        const auto& stream = streams.at(i);
        qInfo() << "Installing headless portal stream" << stream.nodeId << "optionKeys" << headlessPortalOptionKeys(stream.map);
        if (!createPipeWireStream(stream.nodeId, std::move(duplicatedFds->at(static_cast<size_t>(i))))) {
            return false;
        }
    }

    startDurationTimerIfRequested();
    return true;
}
}

Q_DECLARE_METATYPE(HeadlessPortalStream)
Q_DECLARE_METATYPE(HeadlessPortalStreams)

QDBusArgument& operator<<(QDBusArgument& arg, const HeadlessPortalStream& stream)
{
    arg.beginStructure();
    arg << stream.nodeId << stream.map;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, HeadlessPortalStream& stream)
{
    arg.beginStructure();
    arg >> stream.nodeId;

    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QVariant map;
        arg.beginMapEntry();
        arg >> key >> map;
        arg.endMapEntry();
        stream.map.insert(key, map);
    }
    arg.endMap();
    arg.endStructure();

    return arg;
}

class XdpScreenCast : public QObject
{
    Q_OBJECT
public:
    explicit XdpScreenCast(QObject* parent)
        : QObject(parent)
    {
        initDbus();
    }

    void initDbus()
    {
        m_screenCastInterface.reset(new OrgFreedesktopPortalScreenCastInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QDBusConnection::sessionBus()));

        qInfo() << "Initializing headless D-Bus connectivity with XDG Desktop Portal"
                << "service" << m_screenCastInterface->service() << "version" << m_screenCastInterface->version() << "sourceModes"
                << m_screenCastInterface->availableSourceTypes() << "cursorModes" << m_screenCastInterface->availableCursorModes();
        if (!m_screenCastInterface->isValid()) {
            failHeadless(QStringLiteral("XDG ScreenCast portal interface is not valid"), m_screenCastInterface->lastError());
            return;
        }
        if (m_screenCastInterface->availableSourceTypes() == 0) {
            failHeadless(QStringLiteral("XDG ScreenCast portal advertises no source types"));
            return;
        }

        auto sessionReply = m_screenCastInterface->CreateSession({{QStringLiteral("session_handle_token"), createHandleToken()},
            {QStringLiteral("handle_token"), createHandleToken()}});
        sessionReply.waitForFinished();
        if (sessionReply.isError()) {
            failHeadless(QStringLiteral("Could not initialize XDG ScreenCast session"), sessionReply.error());
            return;
        }

        qInfo() << "D-Bus ScreenCast session request created" << sessionReply.value().path();
        if (!connectPortalRequest(sessionReply.value(), this, SLOT(handleSessionCreated(uint, QVariantMap)), QStringLiteral("CreateSession"))) {
            QCoreApplication::exit(1);
        }
    }

public Q_SLOTS:
    void handleSessionCreated(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to create ScreenCast session" << code << results;
            QCoreApplication::exit(1);
            return;
        }

        m_sessionPath = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());
        if (m_sessionPath.path().isEmpty()) {
            qWarning() << "ScreenCast session response did not contain a session handle" << results;
            QCoreApplication::exit(1);
            return;
        }

        QString cursorError;
        const auto cursorMode = selectHeadlessPortalCursorMode(m_screenCastInterface->availableCursorModes(), s_requestedCursorMode, &cursorError);
        if (!cursorMode) {
            qWarning() << "Could not select ScreenCast cursor mode" << cursorError;
            QCoreApplication::exit(1);
            return;
        }

        auto sourcesParameters = buildHeadlessScreenCastSourceOptions(m_screenCastInterface->availableSourceTypes(), *cursorMode);
        sourcesParameters.insert(QStringLiteral("handle_token"), createHandleToken());
        auto selectorReply = m_screenCastInterface->SelectSources(m_sessionPath, sourcesParameters);
        selectorReply.waitForFinished();
        if (selectorReply.isError()) {
            failHeadless(QStringLiteral("Could not select ScreenCast sources for session %1").arg(m_sessionPath.path()), selectorReply.error());
            return;
        }
        if (!connectPortalRequest(selectorReply.value(), this, SLOT(handleSourcesSelected(uint, QVariantMap)), QStringLiteral("SelectSources"), m_sessionPath)) {
            QCoreApplication::exit(1);
        }
    }

    void handleSourcesSelected(quint32 code, const QVariantMap& results)
    {
        if (code != 0) {
            qWarning() << "Failed to select ScreenCast sources" << code << results << "session" << m_sessionPath.path();
            QCoreApplication::exit(1);
            return;
        }

        auto startReply = m_screenCastInterface->Start(m_sessionPath, QString(), {{QStringLiteral("handle_token"), createHandleToken()}});
        startReply.waitForFinished();
        if (startReply.isError()) {
            failHeadless(QStringLiteral("Could not start ScreenCast session %1").arg(m_sessionPath.path()), startReply.error());
            return;
        }
        if (!connectPortalRequest(startReply.value(), this, SLOT(handleStarted(uint, QVariantMap)), QStringLiteral("Start"), m_sessionPath)) {
            QCoreApplication::exit(1);
        }
    }

    void handleStarted(quint32 code, const QVariantMap& results)
    {
        if (code != 0) {
            qWarning() << "Failed to start ScreenCast session" << code << results << "session" << m_sessionPath.path();
            QCoreApplication::exit(1);
            return;
        }

        const HeadlessPortalStreams streams = qdbus_cast<HeadlessPortalStreams>(results.value(QStringLiteral("streams")));
        auto streamReply = m_screenCastInterface->OpenPipeWireRemote(m_sessionPath, {{QStringLiteral("handle_token"), createHandleToken()}});
        streamReply.waitForFinished();
        if (streamReply.isError()) {
            failHeadless(QStringLiteral("Could not open PipeWire remote for ScreenCast session %1").arg(m_sessionPath.path()), streamReply.error());
            return;
        }
        if (!installPortalStreams(streams, streamReply.value())) {
            QCoreApplication::exit(1);
        }
    }

private:
    QScopedPointer<OrgFreedesktopPortalScreenCastInterface> m_screenCastInterface;
    QDBusObjectPath m_sessionPath;
};

class XdpRemoteDesktop : public QObject
{
    Q_OBJECT
public:
    explicit XdpRemoteDesktop(QObject* parent)
        : QObject(parent)
    {
        initDbus();
    }

    void initDbus()
    {
        m_screenCastInterface.reset(new OrgFreedesktopPortalScreenCastInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QDBusConnection::sessionBus()));
        m_remoteDesktopInterface.reset(new OrgFreedesktopPortalRemoteDesktopInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QDBusConnection::sessionBus()));

        qInfo() << "Initializing headless RemoteDesktop D-Bus connectivity with XDG Desktop Portal"
                << "service" << m_remoteDesktopInterface->service() << "version" << m_remoteDesktopInterface->version() << "sourceModes"
                << m_screenCastInterface->availableSourceTypes() << "cursorModes" << m_screenCastInterface->availableCursorModes();
        if (!m_screenCastInterface->isValid()) {
            failHeadless(QStringLiteral("XDG ScreenCast portal interface is not valid"), m_screenCastInterface->lastError());
            return;
        }
        if (!m_remoteDesktopInterface->isValid()) {
            failHeadless(QStringLiteral("XDG RemoteDesktop portal interface is not valid"), m_remoteDesktopInterface->lastError());
            return;
        }
        if (m_screenCastInterface->availableSourceTypes() == 0) {
            failHeadless(QStringLiteral("XDG ScreenCast portal advertises no source types"));
            return;
        }

        auto sessionReply = m_remoteDesktopInterface->CreateSession({{QStringLiteral("session_handle_token"), createHandleToken()},
            {QStringLiteral("handle_token"), createHandleToken()}});
        sessionReply.waitForFinished();
        if (sessionReply.isError()) {
            failHeadless(QStringLiteral("Could not initialize XDG RemoteDesktop session"), sessionReply.error());
            return;
        }

        qInfo() << "D-Bus RemoteDesktop session request created" << sessionReply.value().path();
        if (!connectPortalRequest(sessionReply.value(), this, SLOT(handleSessionCreated(uint, QVariantMap)), QStringLiteral("CreateSession"))) {
            QCoreApplication::exit(1);
        }
    }

public Q_SLOTS:
    void handleSessionCreated(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to create RemoteDesktop session" << code << results;
            QCoreApplication::exit(1);
            return;
        }

        m_sessionPath = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());
        if (m_sessionPath.path().isEmpty()) {
            qWarning() << "RemoteDesktop session response did not contain a session handle" << results;
            QCoreApplication::exit(1);
            return;
        }

        auto selectionOptions = buildHeadlessRemoteDesktopDeviceOptions();
        selectionOptions.insert(QStringLiteral("handle_token"), createHandleToken());
        auto selectorReply = m_remoteDesktopInterface->SelectDevices(m_sessionPath, selectionOptions);
        selectorReply.waitForFinished();
        if (selectorReply.isError()) {
            failHeadless(QStringLiteral("Could not select RemoteDesktop devices for session %1").arg(m_sessionPath.path()), selectorReply.error());
            return;
        }
        if (!connectPortalRequest(selectorReply.value(), this, SLOT(handleDevicesSelected(uint, QVariantMap)), QStringLiteral("SelectDevices"), m_sessionPath)) {
            QCoreApplication::exit(1);
        }
    }

    void handleDevicesSelected(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to select RemoteDesktop devices" << code << results << "session" << m_sessionPath.path();
            QCoreApplication::exit(1);
            return;
        }

        QString cursorError;
        const auto cursorMode = selectHeadlessPortalCursorMode(m_screenCastInterface->availableCursorModes(), s_requestedCursorMode, &cursorError);
        if (!cursorMode) {
            qWarning() << "Could not select RemoteDesktop ScreenCast cursor mode" << cursorError;
            QCoreApplication::exit(1);
            return;
        }

        auto selectionOptions = buildHeadlessScreenCastSourceOptions(m_screenCastInterface->availableSourceTypes(), *cursorMode);
        selectionOptions.insert(QStringLiteral("handle_token"), createHandleToken());
        auto selectorReply = m_screenCastInterface->SelectSources(m_sessionPath, selectionOptions);
        selectorReply.waitForFinished();
        if (selectorReply.isError()) {
            failHeadless(QStringLiteral("Could not select ScreenCast sources for RemoteDesktop session %1").arg(m_sessionPath.path()), selectorReply.error());
            return;
        }
        if (!connectPortalRequest(selectorReply.value(), this, SLOT(handleSourcesSelected(uint, QVariantMap)), QStringLiteral("SelectSources"), m_sessionPath)) {
            QCoreApplication::exit(1);
        }
    }

    void handleSourcesSelected(quint32 code, const QVariantMap& results)
    {
        if (code != 0) {
            qWarning() << "Failed to select RemoteDesktop ScreenCast sources" << code << results << "session" << m_sessionPath.path();
            QCoreApplication::exit(1);
            return;
        }

        auto startReply = m_remoteDesktopInterface->Start(m_sessionPath, QString(), {{QStringLiteral("handle_token"), createHandleToken()}});
        startReply.waitForFinished();
        if (startReply.isError()) {
            failHeadless(QStringLiteral("Could not start RemoteDesktop session %1").arg(m_sessionPath.path()), startReply.error());
            return;
        }
        if (!connectPortalRequest(startReply.value(), this, SLOT(handleRemoteDesktopStarted(uint, QVariantMap)), QStringLiteral("Start"), m_sessionPath)) {
            QCoreApplication::exit(1);
        }
    }

    void handleRemoteDesktopStarted(quint32 code, const QVariantMap &results)
    {
        if (code != 0) {
            qWarning() << "Failed to start RemoteDesktop session" << code << results << "session" << m_sessionPath.path();
            QCoreApplication::exit(1);
            return;
        }

        if (results.value(QStringLiteral("devices")).toUInt() == 0) {
            qWarning() << "No RemoteDesktop devices were granted" << results << "session" << m_sessionPath.path();
            QCoreApplication::exit(1);
            return;
        }

        const HeadlessPortalStreams streams = qdbus_cast<HeadlessPortalStreams>(results.value(QStringLiteral("streams")));
        auto streamReply = m_screenCastInterface->OpenPipeWireRemote(m_sessionPath, {{QStringLiteral("handle_token"), createHandleToken()}});
        streamReply.waitForFinished();
        if (streamReply.isError()) {
            failHeadless(QStringLiteral("Could not open PipeWire remote for RemoteDesktop session %1").arg(m_sessionPath.path()), streamReply.error());
            return;
        }
        if (!installPortalStreams(streams, streamReply.value())) {
            QCoreApplication::exit(1);
        }
    }

private:
    QScopedPointer<OrgFreedesktopPortalScreenCastInterface> m_screenCastInterface;
    QScopedPointer<OrgFreedesktopPortalRemoteDesktopInterface> m_remoteDesktopInterface;
    QDBusObjectPath m_sessionPath;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    qDBusRegisterMetaType<HeadlessPortalStream>();
    qDBusRegisterMetaType<HeadlessPortalStreams>();

    QCommandLineParser parser;
    QCommandLineOption useXdpRD(QStringLiteral("xdp-remotedesktop"), QStringLiteral("Uses the XDG Desktop Portal RemoteDesktop interface"));
    parser.addOption(useXdpRD);
    QCommandLineOption useXdpSC(QStringLiteral("xdp-screencast"), QStringLiteral("Uses the XDG Desktop Portal ScreenCast interface"));
    parser.addOption(useXdpSC);
    QCommandLineOption encodedStream(QStringLiteral("encoded"), QStringLiteral("Reports encoded streams with PipeWireEncodedStream"));
    parser.addOption(encodedStream);
    QCommandLineOption streamEncoder(QStringLiteral("encoder"),
        QStringLiteral("Which encoding to use with PipeWireEncodedStream"),
        u"encoding"_s,
        u"libvpx"_s);
    parser.addOption(streamEncoder);
    QCommandLineOption streamFramerate(QStringLiteral("framerate"),
        QStringLiteral("Makes sure a framerate is requested (format 30/1 would mean 30fps)"),
        QStringLiteral("num/denom"));
    parser.addOption(streamFramerate);
    QCommandLineOption cursorOption(QStringLiteral("cursor"),
        QStringLiteral("Portal cursor mode: hidden, embedded, metadata"),
        QStringLiteral("mode"));
    parser.addOption(cursorOption);
    QCommandLineOption durationOption(QStringLiteral("duration"), QStringLiteral("milliseconds length of the headless portal run"), QStringLiteral("milliseconds"));
    parser.addOption(durationOption);
    parser.addPositionalArgument(QStringLiteral("node"), QStringLiteral("Raw PipeWire node id to consume without portal acquisition"), QStringLiteral("[node]"));
    parser.addHelpOption();
    parser.process(app);

    KSignalHandler::self()->watchSignal(SIGTERM);
    KSignalHandler::self()->watchSignal(SIGINT);
    QObject::connect(KSignalHandler::self(), &KSignalHandler::signalReceived, qGuiApp, &beginOrderlyShutdown);

    if (parser.isSet(cursorOption)) {
        s_requestedCursorMode = headlessPortalCursorModeFromString(parser.value(cursorOption));
        if (!s_requestedCursorMode) {
            qWarning() << "Unsupported cursor mode" << parser.value(cursorOption);
            return 1;
        }
    }
    s_encodedStream = parser.isSet(encodedStream);
    if (parser.isSet(streamEncoder)) {
        s_encoder = parser.value(streamEncoder).toUtf8();
    }
    if (parser.isSet(streamFramerate)) {
        const auto framerateString = parser.value(streamFramerate).split(u'/');
        if (framerateString.count() != 2) {
            qWarning() << "wrong framerate" << framerateString;
            return 1;
        }
        s_framerate = {framerateString.constFirst().toUInt(), framerateString.constLast().toUInt()};
    }
    if (parser.isSet(durationOption)) {
        bool ok = false;
        const int duration = parser.value(durationOption).toInt(&ok);
        if (!ok || duration <= 0) {
            qWarning() << "invalid duration" << parser.value(durationOption);
            return 1;
        }
        s_duration = duration;
    }

    const auto positionalArguments = parser.positionalArguments();
    if (!positionalArguments.isEmpty()) {
        if (positionalArguments.size() != 1 || parser.isSet(useXdpRD) || parser.isSet(useXdpSC)) {
            qWarning() << "Raw node mode accepts exactly one node id and no portal mode option";
            return 1;
        }
        bool ok = false;
        const uint nodeId = positionalArguments.constFirst().toUInt(&ok);
        if (!ok || nodeId == 0) {
            qWarning() << "invalid raw PipeWire node id" << positionalArguments.constFirst();
            return 1;
        }
        if (!createPipeWireStream(nodeId)) {
            return 1;
        }
        startDurationTimerIfRequested();
    } else if (parser.isSet(useXdpRD)) {
        new XdpRemoteDesktop(&app);
    } else {
        new XdpScreenCast(&app);
    }

    return app.exec();
}

#include "HeadlessTest.moc"
