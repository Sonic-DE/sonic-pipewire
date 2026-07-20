/*
    SPDX-FileCopyrightText: 2026 SonicDE Project

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "pipewiresourcestream.h"

#include <QByteArray>
#include <QImage>
#include <QSize>

#include <spa/buffer/buffer.h>
#include <spa/param/video/raw.h>

#include <optional>

namespace PipeWireFrameUtils {

struct VideoBufferView {
    spa_data* data = nullptr;
    spa_chunk* chunk = nullptr;
    QSize size;
    qint32 stride = 0;
    size_t imageBytes = 0;
};

struct OwnedFrame {
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    void* data = nullptr;
    QSize size;
    qint32 stride = 0;
    PipeWireFrameCleanupFunction* cleanup = nullptr;
};

std::optional<size_t> checkedImageBytes(QSize size, qint32 stride);
std::optional<VideoBufferView> validateVideoBuffer(pw_buffer* buffer, const spa_video_info_raw& videoFormat);
std::optional<OwnedFrame> copyMemPtrFrame(spa_video_format format, const VideoBufferView& view);
std::optional<OwnedFrame> mapMemFdFrame(spa_video_format format, const VideoBufferView& view);
std::optional<PipeWireCursor> copyCursor(const spa_buffer* buffer);
bool imageFitsChunk(const VideoBufferView& view);

}
