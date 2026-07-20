/*
    SPDX-FileCopyrightText: 2026 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

#include <optional>

enum HeadlessPortalCursorMode : uint {
    HeadlessPortalCursorHidden = 1,
    HeadlessPortalCursorEmbedded = 2,
    HeadlessPortalCursorMetadata = 4,
};

struct HeadlessPortalStream {
    uint nodeId = 0;
    QVariantMap map;
};

using HeadlessPortalStreams = QList<HeadlessPortalStream>;

QString headlessPortalOptionKeys(const QVariantMap& options);
std::optional<uint> headlessPortalCursorModeFromString(const QString& mode);
std::optional<uint> selectHeadlessPortalCursorMode(uint availableCursorModes, std::optional<uint> requestedCursorMode, QString* errorMessage = nullptr);
bool validateHeadlessPortalStreams(const HeadlessPortalStreams& streams, QString* errorMessage = nullptr);
QVariantMap buildHeadlessScreenCastSourceOptions(uint availableSourceTypes, uint cursorMode);
QVariantMap buildHeadlessRemoteDesktopDeviceOptions();
