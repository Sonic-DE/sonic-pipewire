/*
    SPDX-FileCopyrightText: 2026 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include <QtGlobal>

#include <functional>
#include <optional>
#include <vector>

class PortalUniqueFd {
public:
    PortalUniqueFd() = default;
    explicit PortalUniqueFd(int fd);
    PortalUniqueFd(const PortalUniqueFd&) = delete;
    PortalUniqueFd& operator=(const PortalUniqueFd&) = delete;
    PortalUniqueFd(PortalUniqueFd&& other) noexcept;
    PortalUniqueFd& operator=(PortalUniqueFd&& other) noexcept;
    ~PortalUniqueFd();

    int get() const;
    bool isValid() const;
    int release();
    void reset(int fd = -1);

private:
    int m_fd = -1;
};

using PortalDuplicateFdFunction = std::function<std::optional<PortalUniqueFd>(int)>;

std::optional<PortalUniqueFd> portalDuplicateFd(int fd);
std::optional<std::vector<PortalUniqueFd>> portalDuplicateFdForStreams(PortalUniqueFd originalFd, qsizetype streamCount, const PortalDuplicateFdFunction& duplicateFd);
