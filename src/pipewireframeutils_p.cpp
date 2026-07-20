/*
    SPDX-FileCopyrightText: 2026 SonicDE Project

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireframeutils_p.h"

#include "logging.h"
#include "pwhelpers.h"

#include <cerrno>
#include <cstring>

#include <limits>

#include <sys/mman.h>
#include <unistd.h>

using namespace PipeWireFrameUtils;

namespace {
std::optional<size_t> checkedAdd(size_t a, size_t b)
{
    if (a > std::numeric_limits<size_t>::max() - b) {
        return std::nullopt;
    }
    return a + b;
}

bool rangeInBlock(size_t offset, size_t size, size_t blockSize)
{
    const auto end = checkedAdd(offset, size);
    return end && *end <= blockSize;
}
}

std::optional<size_t> PipeWireFrameUtils::checkedImageBytes(QSize size, qint32 stride)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0 || stride <= 0) {
        return std::nullopt;
    }

    const auto height = static_cast<size_t>(size.height());
    const auto strideBytes = static_cast<size_t>(stride);
    if (height > std::numeric_limits<size_t>::max() / strideBytes) {
        return std::nullopt;
    }
    return height * strideBytes;
}

std::optional<VideoBufferView> PipeWireFrameUtils::validateVideoBuffer(pw_buffer* buffer, const spa_video_info_raw& videoFormat)
{
    if (!buffer || !buffer->buffer) {
        qCWarning(PIPEWIRE_LOGGING) << "received null PipeWire buffer";
        return std::nullopt;
    }

    auto spaBuffer = buffer->buffer;
    if (spaBuffer->n_datas == 0 || !spaBuffer->datas) {
        qCWarning(PIPEWIRE_LOGGING) << "received PipeWire buffer without data planes";
        return std::nullopt;
    }

    spa_data* data = &spaBuffer->datas[0];
    if (!data || !data->chunk) {
        qCWarning(PIPEWIRE_LOGGING) << "received PipeWire buffer without first data chunk";
        return std::nullopt;
    }

    const QSize size(videoFormat.size.width, videoFormat.size.height);
    const auto stride = data->chunk->stride;
    const auto imageBytes = checkedImageBytes(size, stride);
    if (!imageBytes) {
        qCWarning(PIPEWIRE_LOGGING) << "invalid PipeWire frame dimensions or stride" << size << stride;
        return std::nullopt;
    }

    VideoBufferView view;
    view.data = data;
    view.chunk = data->chunk;
    view.size = size;
    view.stride = stride;
    view.imageBytes = *imageBytes;
    return view;
}

bool PipeWireFrameUtils::imageFitsChunk(const VideoBufferView& view)
{
    return view.chunk->size >= view.imageBytes;
}

std::optional<OwnedFrame> PipeWireFrameUtils::copyMemPtrFrame(spa_video_format format, const VideoBufferView& view)
{
    if (!view.data->data) {
        qCWarning(PIPEWIRE_LOGGING) << "received null MemPtr data";
        return std::nullopt;
    }
    if (!rangeInBlock(view.chunk->offset, view.imageBytes, view.data->maxsize)) {
        qCWarning(PIPEWIRE_LOGGING) << "MemPtr frame exceeds buffer bounds" << view.chunk->offset << view.imageBytes << view.data->maxsize;
        return std::nullopt;
    }
    if (!imageFitsChunk(view)) {
        qCWarning(PIPEWIRE_LOGGING) << "MemPtr frame exceeds chunk size" << view.imageBytes << view.chunk->size;
        return std::nullopt;
    }

    auto bytes = static_cast<const char*>(view.data->data) + view.chunk->offset;
    auto storage = new QByteArray(bytes, qsizetype(view.imageBytes));
    return OwnedFrame{format, storage->data(), view.size, view.stride, new PipeWireFrameCleanupFunction([storage] {
                          delete storage;
                      })};
}

std::optional<OwnedFrame> PipeWireFrameUtils::mapMemFdFrame(spa_video_format format, const VideoBufferView& view)
{
    if (view.data->fd < 0) {
        qCWarning(PIPEWIRE_LOGGING) << "received MemFd frame without file descriptor";
        return std::nullopt;
    }

    const auto chunkEnd = checkedAdd(view.chunk->offset, view.imageBytes);
    if (!chunkEnd || *chunkEnd > view.data->maxsize) {
        qCWarning(PIPEWIRE_LOGGING) << "MemFd frame exceeds advertised data bounds" << view.chunk->offset << view.imageBytes << view.data->maxsize;
        return std::nullopt;
    }
    if (!imageFitsChunk(view)) {
        qCWarning(PIPEWIRE_LOGGING) << "MemFd frame exceeds chunk size" << view.imageBytes << view.chunk->size;
        return std::nullopt;
    }

    const auto mapLength = checkedAdd(view.data->mapoffset, view.data->maxsize);
    if (!mapLength || *mapLength == 0) {
        qCWarning(PIPEWIRE_LOGGING) << "invalid MemFd mapping length" << view.data->mapoffset << view.data->maxsize;
        return std::nullopt;
    }

    auto map = static_cast<uint8_t*>(mmap(nullptr, *mapLength, PROT_READ, MAP_PRIVATE, view.data->fd, 0));
    if (map == MAP_FAILED) {
        qCWarning(PIPEWIRE_LOGGING) << "Failed to mmap the memory:" << strerror(errno);
        return std::nullopt;
    }

    const auto exposedOffset = checkedAdd(view.data->mapoffset, view.chunk->offset);
    if (!exposedOffset || !rangeInBlock(*exposedOffset, view.imageBytes, *mapLength)) {
        qCWarning(PIPEWIRE_LOGGING) << "MemFd exposed image exceeds mapping" << view.data->mapoffset << view.chunk->offset << view.imageBytes << *mapLength;
        munmap(map, *mapLength);
        return std::nullopt;
    }

    return OwnedFrame{format, map + *exposedOffset, view.size, view.stride, new PipeWireFrameCleanupFunction([map, length = *mapLength] {
                          munmap(map, length);
                      })};
}

std::optional<PipeWireCursor> PipeWireFrameUtils::copyCursor(const spa_buffer* buffer)
{
    auto meta = spa_buffer_find_meta(buffer, SPA_META_Cursor);
    if (!meta || !meta->data || meta->size < sizeof(spa_meta_cursor)) {
        return std::nullopt;
    }

    auto cursor = static_cast<const spa_meta_cursor*>(meta->data);
    if (!spa_meta_cursor_is_valid(cursor)) {
        return std::nullopt;
    }

    QImage cursorTexture;
    if (cursor->bitmap_offset) {
        if (!rangeInBlock(cursor->bitmap_offset, sizeof(spa_meta_bitmap), meta->size)) {
            qCWarning(PIPEWIRE_LOGGING) << "dropping cursor with invalid bitmap metadata offset" << cursor->bitmap_offset << meta->size;
            return std::nullopt;
        }

        auto bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, const spa_meta_bitmap);
        const QSize bitmapSize(bitmap->size.width, bitmap->size.height);
        const auto bitmapBytes = checkedImageBytes(bitmapSize, qint32(bitmap->stride));
        const auto bitmapDataOffset = checkedAdd(cursor->bitmap_offset, bitmap->offset);
        if (!bitmapBytes || !bitmapDataOffset || !rangeInBlock(*bitmapDataOffset, *bitmapBytes, meta->size)) {
            qCWarning(PIPEWIRE_LOGGING) << "dropping cursor with invalid bitmap bounds" << bitmapSize << bitmap->stride << bitmap->offset << meta->size;
            return std::nullopt;
        }

        auto bytes = SPA_MEMBER(bitmap, bitmap->offset, const char);
        auto storage = new QByteArray(bytes, qsizetype(*bitmapBytes));
        cursorTexture = PWHelpers::SpaBufferToQImage(reinterpret_cast<const uchar*>(storage->data()),
            bitmapSize.width(),
            bitmapSize.height(),
            bitmap->stride,
            spa_video_format(bitmap->format),
            new PipeWireFrameCleanupFunction([storage] {
                delete storage;
            }));
    }

    return PipeWireCursor{{cursor->position.x, cursor->position.y}, {cursor->hotspot.x, cursor->hotspot.y}, cursorTexture};
}
