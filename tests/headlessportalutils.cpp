/*
    SPDX-FileCopyrightText: 2026 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "headlessportalutils.h"

#include <QStringList>

using namespace Qt::StringLiterals;

QString headlessPortalOptionKeys(const QVariantMap& options)
{
    QStringList keys;
    keys.reserve(options.size());
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        keys.push_back(it.key());
    }
    keys.sort();
    return keys.join(QLatin1Char(','));
}

std::optional<uint> headlessPortalCursorModeFromString(const QString& mode)
{
    const QString normalizedMode = mode.toLower();
    if (normalizedMode == "hidden"_L1) {
        return HeadlessPortalCursorHidden;
    }
    if (normalizedMode == "embedded"_L1) {
        return HeadlessPortalCursorEmbedded;
    }
    if (normalizedMode == "metadata"_L1) {
        return HeadlessPortalCursorMetadata;
    }
    return std::nullopt;
}

std::optional<uint> selectHeadlessPortalCursorMode(uint availableCursorModes, std::optional<uint> requestedCursorMode, QString* errorMessage)
{
    const auto setError = [errorMessage](const QString& message) {
        if (errorMessage) {
            *errorMessage = message;
        }
    };

    if (availableCursorModes == 0) {
        setError(QStringLiteral("portal advertises no cursor modes"));
        return std::nullopt;
    }

    if (requestedCursorMode) {
        if ((*requestedCursorMode & availableCursorModes) == 0) {
            setError(QStringLiteral("requested cursor mode %1 is not supported by advertised modes %2").arg(*requestedCursorMode).arg(availableCursorModes));
            return std::nullopt;
        }
        return requestedCursorMode;
    }

    if (availableCursorModes & HeadlessPortalCursorEmbedded) {
        return HeadlessPortalCursorEmbedded;
    }
    if (availableCursorModes & HeadlessPortalCursorHidden) {
        return HeadlessPortalCursorHidden;
    }

    setError(QStringLiteral("portal does not advertise an implicit headless cursor mode"));
    return std::nullopt;
}

bool validateHeadlessPortalStreams(const HeadlessPortalStreams& streams, QString* errorMessage)
{
    const auto setError = [errorMessage](const QString& message) {
        if (errorMessage) {
            *errorMessage = message;
        }
    };

    if (streams.isEmpty()) {
        setError(QStringLiteral("portal returned an empty stream list"));
        return false;
    }

    for (const auto& stream : streams) {
        if (stream.nodeId == 0) {
            setError(QStringLiteral("portal returned a zero PipeWire node with option keys %1").arg(headlessPortalOptionKeys(stream.map)));
            return false;
        }
    }

    return true;
}

QVariantMap buildHeadlessScreenCastSourceOptions(uint availableSourceTypes, uint cursorMode)
{
    return {{QStringLiteral("types"), availableSourceTypes},
        {QStringLiteral("multiple"), false},
        {QStringLiteral("cursor_mode"), cursorMode}};
}

QVariantMap buildHeadlessRemoteDesktopDeviceOptions()
{
    return {{QStringLiteral("types"), QVariant::fromValue<uint>(7)}};
}
