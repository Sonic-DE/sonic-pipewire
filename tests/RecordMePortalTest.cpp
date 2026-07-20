/*
    SPDX-FileCopyrightText: 2026 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "RecordMe.h"

#include <QtTest>

#include <unistd.h>

#include <fcntl.h>

class RecordMePortalTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cursorSelection_data()
    {
        QTest::addColumn<uint>("available");
        QTest::addColumn<uint>("expected");

        QTest::addRow("metadata") << uint(1 | 2 | 4) << uint(4);
        QTest::addRow("embedded") << uint(1 | 2) << uint(2);
        QTest::addRow("hidden") << uint(1) << uint(1);
    }

    void cursorSelection()
    {
        QFETCH(uint, available);
        QFETCH(uint, expected);

        QCOMPARE(RecordMe::selectCursorMode(available), expected);
    }

    void capabilitiesRejectZeroSourceTypes()
    {
        QVERIFY(!RecordMe::validateCapabilities(0, 1, 4, QStringLiteral("service"), QStringLiteral("xcb"), QStringLiteral("SONICDE")));
        QVERIFY(RecordMe::validateCapabilities(1, 1, 4, QStringLiteral("service"), QStringLiteral("xcb"), QStringLiteral("SONICDE")));
    }

    void streamValidation()
    {
        QVERIFY(!RecordMe::validateStreams({}));
        QVERIFY(!RecordMe::validateStreams({{0, {}}}));
        QVERIFY(RecordMe::validateStreams({{12, {{QStringLiteral("id"), QStringLiteral("monitor")}}}}));
    }

    void fdDuplicationClosesOriginalAfterSuccess()
    {
        int pipeFds[2] = {-1, -1};
        QVERIFY(::pipe(pipeFds) == 0);
        PortalUniqueFd readFd(pipeFds[0]);
        PortalUniqueFd writeFd(pipeFds[1]);

        auto duplicated = portalDuplicateFdForStreams(std::move(readFd), 2, portalDuplicateFd);
        QVERIFY(duplicated);
        QCOMPARE(duplicated->size(), size_t(2));
        QVERIFY(!readFd.isValid());
        QVERIFY(fcntl(pipeFds[0], F_GETFD) == -1);
        QVERIFY(duplicated->at(0).isValid());
        QVERIFY(duplicated->at(1).isValid());
        QVERIFY(duplicated->at(0).get() != duplicated->at(1).get());
    }

    void fdDuplicationRollsBackOnFailure()
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
        QVERIFY(!readFd.isValid());
        QVERIFY(fcntl(pipeFds[0], F_GETFD) == -1);
        QVERIFY(fcntl(duplicateFd, F_GETFD) == -1);
    }
};

QTEST_GUILESS_MAIN(RecordMePortalTest)

#include "RecordMePortalTest.moc"
