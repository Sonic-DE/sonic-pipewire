/*
    SPDX-FileCopyrightText: 2026 SonicDE Project

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "pipewireframeutils_p.h"
#include "pwhelpers.h"

#include <QByteArray>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTest>

#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

using namespace PipeWireFrameUtils;

namespace {
struct SpaBufferFixture {
    spa_chunk chunk = {};
    spa_data data = {};
    spa_buffer spaBuffer = {};
    pw_buffer pwBuffer = {};

    SpaBufferFixture()
    {
        data.chunk = &chunk;
        spaBuffer.n_datas = 1;
        spaBuffer.datas = &data;
        pwBuffer.buffer = &spaBuffer;
    }
};

spa_video_info_raw videoFormat(const QSize& size, spa_video_format format = SPA_VIDEO_FORMAT_RGBx)
{
    spa_video_info_raw raw = {};
    raw.format = format;
    raw.size.width = size.width();
    raw.size.height = size.height();
    return raw;
}

std::shared_ptr<PipeWireFrameData> makeFrame(const OwnedFrame& owned)
{
    return std::make_shared<PipeWireFrameData>(owned.format, owned.data, owned.size, owned.stride, owned.cleanup);
}

QByteArray bytes(std::initializer_list<char> values)
{
    return QByteArray(values.begin(), qsizetype(values.size()));
}
}

class TestPipeWireFrameData : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void memPtrCopyOutlivesProducerAndUsesChunkOffset();
    void paddedStrideIsPreserved();
    void memFdMapAndChunkOffsets();
    void exactCopySizeDoesNotReadPastImage();
    void cleanupRunsOnceAfterAllQImageCopies();
    void nullCleanupInstallsNoopCleanup();
    void malformedAndOverflowingBuffersAreRejected();
    void cursorMetadataValidAndInvalid();
};

void TestPipeWireFrameData::memPtrCopyOutlivesProducerAndUsesChunkOffset()
{
    QByteArray producer(64, '\0');
    const qsizetype offset = 5;
    const QByteArray payload = bytes({1, 2, 3, 4, 5, 6, 7, 8});
    memcpy(producer.data() + offset, payload.constData(), size_t(payload.size()));

    SpaBufferFixture fixture;
    fixture.data.type = SPA_DATA_MemPtr;
    fixture.data.data = producer.data();
    fixture.data.maxsize = producer.size();
    fixture.chunk.offset = offset;
    fixture.chunk.size = payload.size();
    fixture.chunk.stride = 4;

    const auto view = validateVideoBuffer(&fixture.pwBuffer, videoFormat({1, 2}));
    QVERIFY(view);
    const auto owned = copyMemPtrFrame(SPA_VIDEO_FORMAT_RGBx, *view);
    QVERIFY(owned);
    auto frame = makeFrame(*owned);

    producer.fill(char(0x7f));
    QCOMPARE(QByteArray(static_cast<const char*>(frame->data), payload.size()), payload);
}

void TestPipeWireFrameData::paddedStrideIsPreserved()
{
    QByteArray producer(48, '\0');
    for (int i = 0; i < producer.size(); ++i) {
        producer[i] = char(i + 1);
    }

    SpaBufferFixture fixture;
    fixture.data.type = SPA_DATA_MemPtr;
    fixture.data.data = producer.data();
    fixture.data.maxsize = producer.size();
    fixture.chunk.offset = 3;
    fixture.chunk.size = 24;
    fixture.chunk.stride = 12;

    const auto view = validateVideoBuffer(&fixture.pwBuffer, videoFormat({2, 2}));
    QVERIFY(view);
    QCOMPARE(view->stride, 12);
    QCOMPARE(view->imageBytes, size_t(24));

    const auto owned = copyMemPtrFrame(SPA_VIDEO_FORMAT_RGBx, *view);
    QVERIFY(owned);
    auto frame = makeFrame(*owned);
    QCOMPARE(frame->stride, 12);
    QCOMPARE(QByteArray(static_cast<const char*>(frame->data), 24), producer.mid(3, 24));
}

void TestPipeWireFrameData::memFdMapAndChunkOffsets()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QVERIFY(file.resize(64));

    QByteArray fileData(64, '\0');
    const QByteArray payload = bytes({9, 8, 7, 6, 5, 4, 3, 2});
    memcpy(fileData.data() + 24, payload.constData(), size_t(payload.size()));
    QCOMPARE(file.write(fileData), qint64(fileData.size()));
    QVERIFY(file.flush());

    SpaBufferFixture fixture;
    fixture.data.type = SPA_DATA_MemFd;
    fixture.data.fd = file.handle();
    fixture.data.mapoffset = 16;
    fixture.data.maxsize = 32;
    fixture.chunk.offset = 8;
    fixture.chunk.size = payload.size();
    fixture.chunk.stride = 4;

    const auto view = validateVideoBuffer(&fixture.pwBuffer, videoFormat({1, 2}));
    QVERIFY(view);
    const auto owned = mapMemFdFrame(SPA_VIDEO_FORMAT_RGBx, *view);
    QVERIFY(owned);
    auto frame = makeFrame(*owned);
    QCOMPARE(QByteArray(static_cast<const char*>(frame->data), payload.size()), payload);
}

void TestPipeWireFrameData::exactCopySizeDoesNotReadPastImage()
{
    const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
    void* mapping = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    QVERIFY(mapping != MAP_FAILED);
    QVERIFY(mprotect(static_cast<char*>(mapping) + pageSize, pageSize, PROT_NONE) == 0);

    const qsizetype imageBytes = 16;
    auto imageStart = static_cast<char*>(mapping) + pageSize - imageBytes;
    for (qsizetype i = 0; i < imageBytes; ++i) {
        imageStart[i] = char(i + 11);
    }
    const QByteArray expected(imageStart, imageBytes);

    std::shared_ptr<PipeWireFrameData> copy;
    {
        PipeWireFrameData frame(SPA_VIDEO_FORMAT_RGBx, imageStart, QSize(1, 4), 4, new PipeWireFrameCleanupFunction([mapping, pageSize] {
            munmap(mapping, pageSize * 2);
        }));
        copy = frame.copy();
    }

    QVERIFY(copy);
    QCOMPARE(QByteArray(static_cast<const char*>(copy->data), imageBytes), expected);
}

void TestPipeWireFrameData::cleanupRunsOnceAfterAllQImageCopies()
{
    int cleanupCount = 0;
    QByteArray storage(16, '\0');
    auto frame = std::make_unique<PipeWireFrameData>(SPA_VIDEO_FORMAT_RGBx, storage.data(), QSize(1, 1), 4, new PipeWireFrameCleanupFunction([&cleanupCount] {
        ++cleanupCount;
    }));

    QImage image = frame->toImage();
    QImage imageCopy = image;
    frame.reset();
    QCOMPARE(cleanupCount, 0);
    image = {};
    QCOMPARE(cleanupCount, 0);
    imageCopy = {};
    QCOMPARE(cleanupCount, 1);
}

void TestPipeWireFrameData::nullCleanupInstallsNoopCleanup()
{
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(".*PipeWireFrameData constructed without cleanup.*")));
    QByteArray storage(4, '\0');
    PipeWireFrameData frame(SPA_VIDEO_FORMAT_RGBx, storage.data(), QSize(1, 1), 4, nullptr);
    QVERIFY(frame.cleanup);
}

void TestPipeWireFrameData::malformedAndOverflowingBuffersAreRejected()
{
    QVERIFY(!validateVideoBuffer(nullptr, videoFormat({1, 1})));

    pw_buffer missingSpa = {};
    QVERIFY(!validateVideoBuffer(&missingSpa, videoFormat({1, 1})));

    spa_buffer noData = {};
    pw_buffer noDataBuffer = {};
    noDataBuffer.buffer = &noData;
    QVERIFY(!validateVideoBuffer(&noDataBuffer, videoFormat({1, 1})));

    SpaBufferFixture noChunk;
    noChunk.data.chunk = nullptr;
    QVERIFY(!validateVideoBuffer(&noChunk.pwBuffer, videoFormat({1, 1})));

    SpaBufferFixture negativeStride;
    negativeStride.chunk.stride = -1;
    QVERIFY(!validateVideoBuffer(&negativeStride.pwBuffer, videoFormat({1, 1})));

    QVERIFY(!checkedImageBytes(QSize(1, 1), 0));
    QVERIFY(!checkedImageBytes(QSize(), 4));

    QByteArray producer(16, '\0');
    SpaBufferFixture memPtrOverflow;
    memPtrOverflow.data.type = SPA_DATA_MemPtr;
    memPtrOverflow.data.data = producer.data();
    memPtrOverflow.data.maxsize = producer.size();
    memPtrOverflow.chunk.offset = 12;
    memPtrOverflow.chunk.size = 8;
    memPtrOverflow.chunk.stride = 4;
    const auto overflowingView = validateVideoBuffer(&memPtrOverflow.pwBuffer, videoFormat({1, 2}));
    QVERIFY(overflowingView);
    QVERIFY(!copyMemPtrFrame(SPA_VIDEO_FORMAT_RGBx, *overflowingView));

    SpaBufferFixture shortChunk;
    shortChunk.data.type = SPA_DATA_MemPtr;
    shortChunk.data.data = producer.data();
    shortChunk.data.maxsize = producer.size();
    shortChunk.chunk.offset = 0;
    shortChunk.chunk.size = 4;
    shortChunk.chunk.stride = 4;
    const auto shortChunkView = validateVideoBuffer(&shortChunk.pwBuffer, videoFormat({1, 2}));
    QVERIFY(shortChunkView);
    QVERIFY(!copyMemPtrFrame(SPA_VIDEO_FORMAT_RGBx, *shortChunkView));

    SpaBufferFixture memFdOverflow;
    memFdOverflow.data.type = SPA_DATA_MemFd;
    memFdOverflow.data.fd = -1;
    memFdOverflow.data.maxsize = 16;
    memFdOverflow.chunk.offset = 12;
    memFdOverflow.chunk.size = 8;
    memFdOverflow.chunk.stride = 4;
    const auto memFdView = validateVideoBuffer(&memFdOverflow.pwBuffer, videoFormat({1, 2}));
    QVERIFY(memFdView);
    QVERIFY(!mapMemFdFrame(SPA_VIDEO_FORMAT_RGBx, *memFdView));
}

void TestPipeWireFrameData::cursorMetadataValidAndInvalid()
{
    const uint32_t bitmapOffset = sizeof(spa_meta_cursor);
    const uint32_t bitmapDataOffset = sizeof(spa_meta_bitmap);
    const uint32_t bitmapBytes = 8;
    QByteArray metadata(bitmapOffset + bitmapDataOffset + bitmapBytes, '\0');

    auto cursor = reinterpret_cast<spa_meta_cursor*>(metadata.data());
    cursor->id = 1;
    cursor->position = SPA_POINT(10, 20);
    cursor->hotspot = SPA_POINT(1, 2);
    cursor->bitmap_offset = bitmapOffset;

    auto bitmap = reinterpret_cast<spa_meta_bitmap*>(metadata.data() + bitmapOffset);
    bitmap->format = SPA_VIDEO_FORMAT_RGBx;
    bitmap->size = SPA_RECTANGLE(2, 1);
    bitmap->stride = 8;
    bitmap->offset = bitmapDataOffset;

    const QByteArray pixels = bytes({1, 2, 3, 4, 5, 6, 7, 8});
    memcpy(metadata.data() + bitmapOffset + bitmapDataOffset, pixels.constData(), pixels.size());

    spa_meta meta = {};
    meta.type = SPA_META_Cursor;
    meta.data = metadata.data();
    meta.size = metadata.size();
    spa_buffer buffer = {};
    buffer.n_metas = 1;
    buffer.metas = &meta;

    const auto validCursor = copyCursor(&buffer);
    QVERIFY(validCursor);
    QCOMPARE(validCursor->position, QPoint(10, 20));
    QCOMPARE(validCursor->hotspot, QPoint(1, 2));
    QCOMPARE(validCursor->texture.size(), QSize(2, 1));
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(validCursor->texture.constBits()), bitmapBytes), pixels);

    cursor->bitmap_offset = sizeof(spa_meta_cursor) - 1;
    QVERIFY(!copyCursor(&buffer));
    cursor->bitmap_offset = bitmapOffset;

    bitmap->offset = metadata.size();
    QVERIFY(!copyCursor(&buffer));
    bitmap->offset = bitmapDataOffset;

    meta.size = sizeof(spa_meta_cursor);
    QVERIFY(!copyCursor(&buffer));

    cursor->bitmap_offset = 0;
    meta.size = sizeof(spa_meta_cursor);
    const auto positionOnlyCursor = copyCursor(&buffer);
    QVERIFY(positionOnlyCursor);
    QVERIFY(positionOnlyCursor->texture.isNull());
}

QTEST_GUILESS_MAIN(TestPipeWireFrameData)

#include "TestPipeWireFrameData.moc"
