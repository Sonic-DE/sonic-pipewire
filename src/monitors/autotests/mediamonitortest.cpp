/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#define QT_FORCE_ASSERTS 1

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {
template <typename T>
[[nodiscard]] T evaluate(QObject* scope, const QString& expression)
{
    QQmlExpression expr(qmlContext(scope), scope, expression);
    QVariant result = expr.evaluate();
    Q_ASSERT_X(!expr.hasError(), "MediaMonitorTest", qUtf8Printable(expr.error().toString()));
    return result.value<T>();
}

template <>
void evaluate<void>(QObject* scope, const QString& expression)
{
    QQmlExpression expr(qmlContext(scope), scope, expression);
    expr.evaluate();
    Q_ASSERT_X(!expr.hasError(), "MediaMonitorTest", qUtf8Printable(expr.error().toString()));
}

bool initView(QQuickView* view, const QString& urlStr)
{
    view->setSource(QUrl(urlStr));

    QSignalSpy statusSpy(view, &QQuickView::statusChanged);
    if (view->status() == QQuickView::Loading) {
        statusSpy.wait();
    } else if (view->status() != QQuickView::Ready) {
        qCritical() << "Not loading" << view->errors();
        return false;
    }

    if (view->status() != QQuickView::Ready) {
        qCritical() << view->errors();
        return false;
    }

    return true;
}

void stopProcess(QProcess& process)
{
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    process.terminate();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(5000);
    }
}
}

class MediaMonitorTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    /**
     * Loads MediaMonitor in QML, and tests it can monitor music streams
     */
    void test_MediaMonitor();
};

void MediaMonitorTest::test_MediaMonitor()
{
    QTemporaryDir runtime;
    QVERIFY(runtime.isValid());
    const auto oldRuntime = qgetenv("PIPEWIRE_RUNTIME_DIR");
    const auto oldRemote = qgetenv("PIPEWIRE_REMOTE");
    QScopeGuard environmentGuard([oldRuntime, oldRemote] {
        if (oldRuntime.isNull()) {
            qunsetenv("PIPEWIRE_RUNTIME_DIR");
        } else {
            qputenv("PIPEWIRE_RUNTIME_DIR", oldRuntime);
        }
        if (oldRemote.isNull()) {
            qunsetenv("PIPEWIRE_REMOTE");
        } else {
            qputenv("PIPEWIRE_REMOTE", oldRemote);
        }
    });
    QVERIFY(qputenv("PIPEWIRE_RUNTIME_DIR", runtime.path().toUtf8()));
    QVERIFY(qputenv("PIPEWIRE_REMOTE", "pipewire-0"));

    // Own the server under test, even when the desktop already has one or more
    // PipeWire processes. No session manager or physical audio devices are needed
    // to observe a suspended playback node.
    QFile config(runtime.filePath(QStringLiteral("pipewire.conf")));
    QVERIFY(config.open(QIODevice::WriteOnly));
    const QByteArray configuration = R"(
context.properties = { core.daemon = true core.name = pipewire-0 }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-access }
]
)";
    QCOMPARE(config.write(configuration), qint64(configuration.size()));
    config.close();

    QProcess pipewire;
    QProcess player;
    QScopeGuard processGuard([&pipewire, &player] {
        stopProcess(player);
        stopProcess(pipewire);
    });
    pipewire.start(QStringLiteral("pipewire"), {QStringLiteral("-c"), config.fileName()});
    QVERIFY2(pipewire.waitForStarted(), pipewire.readAllStandardError().constData());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(runtime.filePath(QStringLiteral("pipewire-0"))), 5000);
    QCOMPARE(pipewire.state(), QProcess::Running);

    QQuickView view;
    QByteArray errorMessage;
    QVERIFY(initView(&view, QFINDTESTDATA(QStringLiteral("mediamonitortest.qml"))));

    QQuickItem* rootObject = view.rootObject();
    QSignalSpy countSpy(rootObject, SIGNAL(countChanged()));

    player.setProgram(QStringLiteral("pw-play"));
    player.setArguments({QStringLiteral("-v"), QFINDTESTDATA(QStringLiteral("alarm-clock-elapsed.oga"))});
    player.start();
    QVERIFY2(player.waitForStarted(), player.readAllStandardError().constData());

    if (evaluate<int>(rootObject, QStringLiteral("root.count")) == 0) {
        countSpy.wait();
    }

    QVERIFY(evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 1);
    const int suspendedInt = evaluate<int>(rootObject, QStringLiteral("Monitor.NodeState.Suspended"));
    QTRY_COMPARE(evaluate<int>(rootObject, QStringLiteral("root.state")), suspendedInt);

    // Changing role will trigger reconnecting
    QObject* monitor = evaluate<QObject*>(rootObject, QStringLiteral("monitor"));
    QVERIFY(monitor);
    QSignalSpy detectionAvailableChangedSpy(monitor, SIGNAL(detectionAvailableChanged()));
    QSignalSpy roleChangedSpy(monitor, SIGNAL(roleChanged()));
    evaluate<void>(rootObject, QStringLiteral("monitor.role = Monitor.MediaMonitor.Camera"));
    if (roleChangedSpy.empty()) {
        QVERIFY(roleChangedSpy.wait());
    }
    QVERIFY(evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    QCOMPARE(detectionAvailableChangedSpy.size(), 2); // True -> False -> True
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 0);

    // Change back to Music role
    roleChangedSpy.clear();
    detectionAvailableChangedSpy.clear();
    evaluate<void>(rootObject, QStringLiteral("monitor.role = Monitor.MediaMonitor.Music"));
    if (roleChangedSpy.empty()) {
        QVERIFY(roleChangedSpy.wait());
    }
    QVERIFY(evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
    QCOMPARE(detectionAvailableChangedSpy.size(), 2); // True -> False -> True
    if (evaluate<int>(rootObject, QStringLiteral("root.count")) == 0) {
        QVERIFY(countSpy.wait());
    }
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 1);

    // Now close the player, and check if the count changes
    player.terminate();
    QVERIFY2(player.waitForFinished(5000), player.readAllStandardError().constData());
    if (evaluate<int>(rootObject, QStringLiteral("root.count")) > 0) {
        QVERIFY(countSpy.wait());
    }
    QCOMPARE(evaluate<int>(rootObject, QStringLiteral("root.count")), 0);

    pipewire.terminate();
    QVERIFY2(pipewire.waitForFinished(5000), pipewire.readAllStandardError().constData());
    QTRY_VERIFY(!evaluate<bool>(rootObject, QStringLiteral("monitor.detectionAvailable")));
}

QTEST_MAIN(MediaMonitorTest)

#include "mediamonitortest.moc"
