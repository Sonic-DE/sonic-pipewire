/*
    SPDX-FileCopyrightText: 2026 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "headlessportalutils.h"
#include "portalfdutils.h"

#include <QtTest>

#include <fcntl.h>
#include <unistd.h>

class HeadlessPortalTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void defaultCursorSelection_data()
    {
        QTest::addColumn<uint>("available");
        QTest::addColumn<uint>("expected");

        QTest::addRow("prefers-embedded") << uint(HeadlessPortalCursorHidden | HeadlessPortalCursorEmbedded | HeadlessPortalCursorMetadata)
                                          << uint(HeadlessPortalCursorEmbedded);
        QTest::addRow("hidden-fallback") << uint(HeadlessPortalCursorHidden | HeadlessPortalCursorMetadata) << uint(HeadlessPortalCursorHidden);
    }

    void defaultCursorSelection()
    {
        QFETCH(uint, available);
        QFETCH(uint, expected);

        QString error;
        const auto selected = selectHeadlessPortalCursorMode(available, std::nullopt, &error);
        QVERIFY2(selected, qPrintable(error));
        QCOMPARE(*selected, expected);
    }

    void explicitCursorSelectionRejectsUnsupported()
    {
        QString error;
        QVERIFY(selectHeadlessPortalCursorMode(HeadlessPortalCursorHidden, HeadlessPortalCursorHidden, &error));
        QVERIFY(!selectHeadlessPortalCursorMode(HeadlessPortalCursorHidden, HeadlessPortalCursorEmbedded, &error));
        QVERIFY(error.contains(QStringLiteral("not supported")));
    }

    void cursorModeParsing()
    {
        QCOMPARE(*headlessPortalCursorModeFromString(QStringLiteral("hidden")), uint(HeadlessPortalCursorHidden));
        QCOMPARE(*headlessPortalCursorModeFromString(QStringLiteral("Embedded")), uint(HeadlessPortalCursorEmbedded));
        QCOMPARE(*headlessPortalCursorModeFromString(QStringLiteral("metadata")), uint(HeadlessPortalCursorMetadata));
        QVERIFY(!headlessPortalCursorModeFromString(QStringLiteral("workspace")));
    }

    void sourceAndDeviceOptions()
    {
        const QVariantMap sourceOptions = buildHeadlessScreenCastSourceOptions(3, HeadlessPortalCursorEmbedded);
        QCOMPARE(sourceOptions.value(QStringLiteral("types")).toUInt(), uint(3));
        QCOMPARE(sourceOptions.value(QStringLiteral("multiple")).toBool(), false);
        QCOMPARE(sourceOptions.value(QStringLiteral("cursor_mode")).toUInt(), uint(HeadlessPortalCursorEmbedded));

        const QVariantMap deviceOptions = buildHeadlessRemoteDesktopDeviceOptions();
        QCOMPARE(deviceOptions.value(QStringLiteral("types")).toUInt(), uint(7));
    }

    void streamValidationRejectsInvalidResponses()
    {
        QString error;
        QVERIFY(!validateHeadlessPortalStreams({}, &error));
        QVERIFY(error.contains(QStringLiteral("empty")));

        QVERIFY(!validateHeadlessPortalStreams({{0, {{QStringLiteral("source_type"), QStringLiteral("monitor")}}}}, &error));
        QVERIFY(error.contains(QStringLiteral("zero")));

        QVERIFY(validateHeadlessPortalStreams({{42, {{QStringLiteral("source_type"), QStringLiteral("monitor")}}}}, &error));
    }

    void perStreamFdTransfer()
    {
        int pipeFds[2] = {-1, -1};
        QVERIFY(::pipe(pipeFds) == 0);
        PortalUniqueFd readFd(pipeFds[0]);
        PortalUniqueFd writeFd(pipeFds[1]);

        auto duplicated = portalDuplicateFdForStreams(std::move(readFd), 2, portalDuplicateFd);
        QVERIFY(duplicated);
        QCOMPARE(duplicated->size(), size_t(2));
        QVERIFY(fcntl(pipeFds[0], F_GETFD) == -1);
        QVERIFY(duplicated->at(0).isValid());
        QVERIFY(duplicated->at(1).isValid());
        QVERIFY(duplicated->at(0).get() != duplicated->at(1).get());
    }

    void perStreamFdRollback()
    {
        int pipeFds[2] = {-1, -1};
        QVERIFY(::pipe(pipeFds) == 0);
        PortalUniqueFd readFd(pipeFds[0]);
        PortalUniqueFd writeFd(pipeFds[1]);
        int duplicateFd = -1;
        int calls = 0;

        auto duplicated = portalDuplicateFdForStreams(std::move(readFd), 2, [&duplicateFd, &calls](int fd) -> std::optional<PortalUniqueFd> {
            ++calls;
            if (calls == 2) {
                return std::nullopt;
            }
            duplicateFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
            return PortalUniqueFd(duplicateFd);
        });

        QVERIFY(!duplicated);
        QCOMPARE(calls, 2);
        QVERIFY(fcntl(pipeFds[0], F_GETFD) == -1);
        QVERIFY(fcntl(duplicateFd, F_GETFD) == -1);
    }
};

QTEST_GUILESS_MAIN(HeadlessPortalTest)

#include "HeadlessPortalTest.moc"
