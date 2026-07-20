/*
    SPDX-FileCopyrightText: 2026 SonicDE Project

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "dmabufeglsetup_p.h"

#include <QTest>

namespace {
const auto borrowedDisplay = reinterpret_cast<EGLDisplay>(0x10);
const auto ownedDisplay = reinterpret_cast<EGLDisplay>(0x20);
const auto context = reinterpret_cast<EGLContext>(0x30);
const auto config = reinterpret_cast<EGLConfig>(0x40);
const auto gbmDevice = reinterpret_cast<gbm_device*>(0x50);

struct FakeEgl {
    enum class Failure {
        None,
        MissingPlatformBase,
        MissingPlatformGbm,
        MissingDmaBufImport,
        EmptyRenderNode,
        OpenRenderNode,
        CreateGbm,
        GetPlatformDisplay,
        Initialize,
        BindApi,
        ChooseConfigCall,
        ChooseConfigEmpty,
        CreateContext,
        MakeCurrent,
    };

    Failure failure = Failure::None;
    bool useBorrowedDisplay = false;
    QStringList calls;

    DmaBufEglSetup::Ops ops()
    {
        return {
            .resolveRenderNode = [this] {
                calls << QStringLiteral("resolve");
                RenderNodeContext context;
                if (useBorrowedDisplay) {
                    context.eglDisplay = borrowedDisplay;
                }
                if (failure != Failure::EmptyRenderNode) {
                    context.renderNode = QByteArrayLiteral("/dev/dri/renderD128");
                }
                return context; },
            .hasExtension = [this](EGLDisplay display, const char* extension) {
                calls << QStringLiteral("hasExtension:%1:%2").arg(display == EGL_NO_DISPLAY ? QStringLiteral("client") : QStringLiteral("display"), QString::fromLatin1(extension));
                if (failure == Failure::MissingPlatformBase && qstrcmp(extension, "EGL_EXT_platform_base") == 0) {
                    return false;
                }
                if (failure == Failure::MissingPlatformGbm && qstrcmp(extension, "EGL_MESA_platform_gbm") == 0) {
                    return false;
                }
                if (failure == Failure::MissingDmaBufImport && qstrcmp(extension, "EGL_EXT_image_dma_buf_import") == 0) {
                    return false;
                }
                return true; },
            .openRenderNode = [this](const char*, int) {
                calls << QStringLiteral("open");
                return failure == Failure::OpenRenderNode ? -1 : 7; },
            .closeFd = [this](int fd) { calls << QStringLiteral("close:%1").arg(fd); },
            .createGbmDevice = [this](int) {
                calls << QStringLiteral("createGbm");
                return failure == Failure::CreateGbm ? nullptr : gbmDevice; },
            .destroyGbmDevice = [this](gbm_device*) { calls << QStringLiteral("destroyGbm"); },
            .getPlatformDisplay = [this](EGLenum platform, void* nativeDisplay, const EGLint*) {
                calls << QStringLiteral("getPlatformDisplay:%1").arg(platform == EGL_PLATFORM_GBM_MESA && nativeDisplay == gbmDevice ? QStringLiteral("gbm") : QStringLiteral("unexpected"));
                return failure == Failure::GetPlatformDisplay ? EGL_NO_DISPLAY : ownedDisplay; },
            .initialize = [this](EGLDisplay display, EGLint* major, EGLint* minor) {
                calls << QStringLiteral("initialize:%1").arg(display == borrowedDisplay ? QStringLiteral("borrowed") : QStringLiteral("owned"));
                *major = 1;
                *minor = 5;
                return failure == Failure::Initialize ? EGL_FALSE : EGL_TRUE; },
            .terminate = [this](EGLDisplay display) {
                calls << QStringLiteral("terminate:%1").arg(display == ownedDisplay ? QStringLiteral("owned") : QStringLiteral("borrowed"));
                return EGL_TRUE; },
            .bindApi = [this](EGLenum api) {
                calls << QStringLiteral("bindApi:%1").arg(api == EGL_OPENGL_API ? QStringLiteral("opengl") : QStringLiteral("unexpected"));
                return failure == Failure::BindApi ? EGL_FALSE : EGL_TRUE; },
            .chooseConfig = [this](EGLDisplay, const EGLint*, EGLConfig* configs, EGLint, EGLint* count) {
                calls << QStringLiteral("chooseConfig");
                if (failure == Failure::ChooseConfigCall) {
                    return EGL_FALSE;
                }
                *configs = failure == Failure::ChooseConfigEmpty ? EGL_NO_CONFIG_KHR : config;
                *count = failure == Failure::ChooseConfigEmpty ? 0 : 1;
                return EGL_TRUE; },
            .createContext = [this](EGLDisplay, EGLConfig, EGLContext, const EGLint*) {
                calls << QStringLiteral("createContext");
                return failure == Failure::CreateContext ? EGL_NO_CONTEXT : context; },
            .destroyContext = [this](EGLDisplay, EGLContext) {
                calls << QStringLiteral("destroyContext");
                return EGL_TRUE; },
            .makeCurrent = [this](EGLDisplay, EGLSurface, EGLSurface, EGLContext) {
                calls << QStringLiteral("makeCurrent");
                return failure == Failure::MakeCurrent ? EGL_FALSE : EGL_TRUE; },
        };
    }
};
}

class TestDmaBufEglSetup : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void borrowedDisplayDoesNotRequireGbmExtensionsOrTerminate();
    void ownedDisplayCleansUpInReverseConstructionOrder();
    void ownedDisplayUsesClientExtensionsBeforePlatformDisplay();
    void failureBranchesCleanOwnedResources_data();
    void failureBranchesCleanOwnedResources();
};

void TestDmaBufEglSetup::borrowedDisplayDoesNotRequireGbmExtensionsOrTerminate()
{
    FakeEgl fake;
    fake.useBorrowedDisplay = true;
    fake.failure = FakeEgl::Failure::MissingPlatformBase;
    DmaBufEglSetup::State state;

    QVERIFY(DmaBufEglSetup::setup(state, fake.ops()));
    QVERIFY(state.initialized);
    QCOMPARE(state.display, borrowedDisplay);
    QVERIFY(!state.ownsDisplay);
    QVERIFY(!fake.calls.contains(QStringLiteral("open")));
    QVERIFY(!fake.calls.contains(QStringLiteral("hasExtension:client:EGL_EXT_platform_base")));

    DmaBufEglSetup::cleanup(state, fake.ops());
    QVERIFY(fake.calls.contains(QStringLiteral("destroyContext")));
    QVERIFY(!fake.calls.contains(QStringLiteral("terminate:borrowed")));
    QCOMPARE(state.display, EGL_NO_DISPLAY);
}

void TestDmaBufEglSetup::ownedDisplayCleansUpInReverseConstructionOrder()
{
    FakeEgl fake;
    DmaBufEglSetup::State state;

    QVERIFY(DmaBufEglSetup::setup(state, fake.ops()));
    QVERIFY(state.initialized);
    QVERIFY(state.ownsDisplay);
    QCOMPARE(state.display, ownedDisplay);
    QCOMPARE(state.drmFd, 7);
    QCOMPARE(state.gbmDevice, gbmDevice);

    fake.calls.clear();
    DmaBufEglSetup::cleanup(state, fake.ops());
    QCOMPARE(fake.calls, QStringList({QStringLiteral("destroyContext"), QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")}));
    QVERIFY(!state.initialized);
    QCOMPARE(state.display, EGL_NO_DISPLAY);
    QCOMPARE(state.context, EGL_NO_CONTEXT);
    QCOMPARE(state.gbmDevice, nullptr);
    QCOMPARE(state.drmFd, -1);
}

void TestDmaBufEglSetup::ownedDisplayUsesClientExtensionsBeforePlatformDisplay()
{
    FakeEgl fake;
    DmaBufEglSetup::State state;

    QVERIFY(DmaBufEglSetup::setup(state, fake.ops()));
    const int platformBase = fake.calls.indexOf(QStringLiteral("hasExtension:client:EGL_EXT_platform_base"));
    const int platformGbm = fake.calls.indexOf(QStringLiteral("hasExtension:client:EGL_MESA_platform_gbm"));
    const int getDisplay = fake.calls.indexOf(QStringLiteral("getPlatformDisplay:gbm"));
    QVERIFY(platformBase >= 0);
    QVERIFY(platformGbm >= 0);
    QVERIFY(getDisplay >= 0);
    QVERIFY(platformBase < getDisplay);
    QVERIFY(platformGbm < getDisplay);
    const int importCheck = fake.calls.indexOf(QStringLiteral("hasExtension:display:EGL_EXT_image_dma_buf_import"));
    QVERIFY(importCheck > getDisplay);
}

void TestDmaBufEglSetup::failureBranchesCleanOwnedResources_data()
{
    QTest::addColumn<FakeEgl::Failure>("failure");
    QTest::addColumn<QStringList>("cleanupCalls");

    QTest::newRow("missing-platform-base") << FakeEgl::Failure::MissingPlatformBase << QStringList{};
    QTest::newRow("missing-platform-gbm") << FakeEgl::Failure::MissingPlatformGbm << QStringList{};
    QTest::newRow("empty-render-node") << FakeEgl::Failure::EmptyRenderNode << QStringList{};
    QTest::newRow("open-render-node") << FakeEgl::Failure::OpenRenderNode << QStringList{};
    QTest::newRow("create-gbm") << FakeEgl::Failure::CreateGbm << QStringList{QStringLiteral("close:7")};
    QTest::newRow("get-platform-display") << FakeEgl::Failure::GetPlatformDisplay << QStringList{QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("initialize") << FakeEgl::Failure::Initialize << QStringList{QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("missing-dmabuf-import") << FakeEgl::Failure::MissingDmaBufImport
                                           << QStringList{QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("bind-api") << FakeEgl::Failure::BindApi << QStringList{QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("choose-config-call") << FakeEgl::Failure::ChooseConfigCall << QStringList{QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("choose-config-empty") << FakeEgl::Failure::ChooseConfigEmpty << QStringList{QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("create-context") << FakeEgl::Failure::CreateContext << QStringList{QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
    QTest::newRow("make-current") << FakeEgl::Failure::MakeCurrent << QStringList{QStringLiteral("destroyContext"), QStringLiteral("terminate:owned"), QStringLiteral("destroyGbm"), QStringLiteral("close:7")};
}

void TestDmaBufEglSetup::failureBranchesCleanOwnedResources()
{
    QFETCH(FakeEgl::Failure, failure);
    QFETCH(QStringList, cleanupCalls);

    FakeEgl fake;
    fake.failure = failure;
    DmaBufEglSetup::State state;

    QVERIFY(!DmaBufEglSetup::setup(state, fake.ops()));
    QVERIFY(!state.initialized);
    QCOMPARE(state.display, EGL_NO_DISPLAY);
    QCOMPARE(state.context, EGL_NO_CONTEXT);
    QCOMPARE(state.gbmDevice, nullptr);
    QCOMPARE(state.drmFd, -1);

    QStringList actualCleanupCalls;
    for (const QString& call : std::as_const(fake.calls)) {
        if (call == QLatin1String("destroyContext") || call.startsWith(QLatin1String("terminate")) || call == QLatin1String("destroyGbm")
            || call.startsWith(QLatin1String("close"))) {
            actualCleanupCalls << call;
        }
    }
    QCOMPARE(actualCleanupCalls, cleanupCalls);
}

QTEST_APPLESS_MAIN(TestDmaBufEglSetup)

#include "TestDmaBufEglSetup.moc"
