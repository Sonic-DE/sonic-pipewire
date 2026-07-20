/*
    SPDX-FileCopyrightText: 2026 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "portalfdutils.h"

#include <unistd.h>

#include <cerrno>
#include <fcntl.h>

PortalUniqueFd::PortalUniqueFd(int fd)
    : m_fd(fd)
{
}

PortalUniqueFd::PortalUniqueFd(PortalUniqueFd&& other) noexcept
    : m_fd(other.release())
{
}

PortalUniqueFd& PortalUniqueFd::operator=(PortalUniqueFd&& other) noexcept
{
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

PortalUniqueFd::~PortalUniqueFd()
{
    reset();
}

int PortalUniqueFd::get() const
{
    return m_fd;
}

bool PortalUniqueFd::isValid() const
{
    return m_fd >= 0;
}

int PortalUniqueFd::release()
{
    const int fd = m_fd;
    m_fd = -1;
    return fd;
}

void PortalUniqueFd::reset(int fd)
{
    if (m_fd >= 0) {
        close(m_fd);
    }
    m_fd = fd;
}

std::optional<PortalUniqueFd> portalDuplicateFd(int fd)
{
    if (fd < 0) {
        return std::nullopt;
    }

    int duplicatedFd = -1;
    do {
        duplicatedFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    } while (duplicatedFd == -1 && errno == EINTR);

    if (duplicatedFd < 0) {
        return std::nullopt;
    }

    return PortalUniqueFd(duplicatedFd);
}

std::optional<std::vector<PortalUniqueFd>> portalDuplicateFdForStreams(PortalUniqueFd originalFd, qsizetype streamCount, const PortalDuplicateFdFunction& duplicateFd)
{
    if (!originalFd.isValid() || streamCount <= 0 || !duplicateFd) {
        return std::nullopt;
    }

    std::vector<PortalUniqueFd> duplicatedFds;
    duplicatedFds.reserve(static_cast<size_t>(streamCount));
    for (qsizetype i = 0; i < streamCount; ++i) {
        auto duplicatedFd = duplicateFd(originalFd.get());
        if (!duplicatedFd || !duplicatedFd->isValid()) {
            return std::nullopt;
        }
        duplicatedFds.push_back(std::move(*duplicatedFd));
    }

    originalFd.reset();
    return duplicatedFds;
}
