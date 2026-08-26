// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include <QtTest>

#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

#include "encoder_p.h"
#include "gifencoder_p.h"
#include "h264vaapiencoder_p.h"
#include "libopenh264encoder_p.h"
#include "libvpxencoder_p.h"
#include "libvpxvp9encoder_p.h"
#include "libwebpencoder_p.h"
#include "libx264encoder_p.h"
#include "pipewirebaseencodedstream.h"
#include "pipewireproduce_p.h"
#include "vaapiutils_p.h"

using namespace Qt::StringLiterals;

class TestProduce : public PipeWireProduce
{
public:
    TestProduce()
        : PipeWireProduce(PipeWireBaseEncodedStream::Encoder::NoEncoder, 0, 0, 0, Fraction{.numerator = 1, .denominator = 24})
    {
    }

    void processPacket(AVPacket* packet) override
    {
        ++receivedPackets;
        lastPts = packet->pts;
    }

    int receivedPackets = 0;
    int64_t lastPts = AV_NOPTS_VALUE;
};

class FrameTrackingEncoder : public Encoder {
public:
    using Encoder::Encoder;

    bool initialize(const QSize&) override
    {
        return true;
    }

    bool filterFrame(const PipeWireFrame& frame) override
    {
        if (!frame.dmabuf && !frame.dataFrame) {
            return false;
        }
        frames.append(frame);
        processingThread = QThread::currentThread();
        return true;
    }

    QList<PipeWireFrame> frames;
    QThread* processingThread = nullptr;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8>&) override
    {
        return 0;
    }
};

class FrameTrackingProduce : public TestProduce {
public:
    void processFrame(const PipeWireFrame& frame) override
    {
        PipeWireProduce::processFrame(frame);
        // Subclasses may still use the argument after the base clears its cache.
        lastObservedSequence = frame.sequential;
    }

    std::optional<quint64> lastObservedSequence;
};

static PipeWireFrame trackingFrame(quint64 sequence, int timestamp)
{
    PipeWireFrame frame{};
    frame.format = SPA_VIDEO_FORMAT_RGBA;
    frame.sequential = sequence;
    frame.presentationTimestamp = std::chrono::milliseconds(timestamp);
    // Only a payload marker for FrameTrackingEncoder, never imported as DMA-BUF.
    frame.dmabuf = DmaBufAttributes{.width = 64, .height = 64};
    return frame;
}

// Feed a synthetic image into the real filter graph without a live PipeWire
// stream, then exercise the normal encode/receive path without an EOF flush.
template <typename Base>
class TestSoftwareEncoder : public Base {
public:
    using Base::Base;

    bool filterFrame(const PipeWireFrame& frame) override
    {
        auto avFrame = av_frame_alloc();
        if (!avFrame) {
            return false;
        }
        avFrame->format = AV_PIX_FMT_RGBA;
        avFrame->width = this->m_avCodecContext->width;
        avFrame->height = this->m_avCodecContext->height;
        avFrame->pts = this->m_produce->framePts(frame.presentationTimestamp);
        if (av_frame_get_buffer(avFrame, 0) < 0) {
            av_frame_free(&avFrame);
            return false;
        }
        std::memset(avFrame->data[0], 0, avFrame->linesize[0] * avFrame->height);
        const auto result = av_buffersrc_add_frame(this->m_inputFilter, avFrame);
        av_frame_free(&avFrame);
        return result >= 0;
    }
};

class TestVAAPIEncoder : public H264VAAPIEncoder {
public:
    using H264VAAPIEncoder::buildEncodingOptions;
    using H264VAAPIEncoder::H264VAAPIEncoder;
};

class TestX264Encoder : public LibX264Encoder {
public:
    using LibX264Encoder::buildEncodingOptions;
    using LibX264Encoder::LibX264Encoder;
    using LibX264Encoder::percentageToAbsoluteQuality;
};

class TestVP9Encoder : public LibVpxVp9Encoder {
public:
    using LibVpxVp9Encoder::buildEncodingOptions;
    using LibVpxVp9Encoder::LibVpxVp9Encoder;
    using LibVpxVp9Encoder::percentageToAbsoluteQuality;

    bool setFrameHeight(int height)
    {
        // An unopened context is sufficient for testing option construction;
        // no installed libvpx encoder or live PipeWire stream is needed.
        m_avCodecContext = avcodec_alloc_context3(nullptr);
        if (!m_avCodecContext) {
            return false;
        }
        m_avCodecContext->height = height;
        return true;
    }
};

// This is a pretty simple smoke test that verifies all the encoders can
// initialize properly. This verifies that things like filter chains are correct.
class TestEncoder : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        m_produce = std::make_unique<TestProduce>();
    }

    void testEncoder_data()
    {
        QTest::addColumn<std::shared_ptr<Encoder>>("encoder");
        QTest::addColumn<QByteArray>("avcodecEncoder");

        QTest::addRow("h264_vaapi_main") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Main, m_produce.get())) << "h264_vaapi"_ba;
        QTest::addRow("h264_vaapi_baseline") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::Baseline, m_produce.get()))
                                             << "h264_vaapi"_ba;
        QTest::addRow("h264_vaapi_high") << std::shared_ptr<Encoder>(new H264VAAPIEncoder(Encoder::H264Profile::High, m_produce.get())) << "h264_vaapi"_ba;
        //
        QTest::addRow("x264_main") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Main, m_produce.get())) << "libx264"_ba;
        QTest::addRow("x264_baseline") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::Baseline, m_produce.get())) << "libx264"_ba;
        QTest::addRow("x264_high") << std::shared_ptr<Encoder>(new LibX264Encoder(Encoder::H264Profile::High, m_produce.get())) << "libx264"_ba;

        QTest::addRow("openh264_main") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Main, m_produce.get())) << "libopenh264"_ba;
        QTest::addRow("openh264_baseline") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::Baseline, m_produce.get()))
                                           << "libopenh264"_ba;
        QTest::addRow("openh264_high") << std::shared_ptr<Encoder>(new LibOpenH264Encoder(Encoder::H264Profile::High, m_produce.get())) << "libopenh264"_ba;

        QTest::addRow("vp8") << std::shared_ptr<Encoder>(new LibVpxEncoder(m_produce.get())) << "libvpx"_ba;
        QTest::addRow("vp9") << std::shared_ptr<Encoder>(new LibVpxVp9Encoder(m_produce.get())) << "libvpx-vp9"_ba;

        QTest::addRow("gif") << std::shared_ptr<Encoder>(new GifEncoder(m_produce.get())) << "gif"_ba;
        QTest::addRow("webp") << std::shared_ptr<Encoder>(new LibWebPEncoder(m_produce.get())) << "libwebp"_ba;
    }

    void testEncoder()
    {
        QFETCH(std::shared_ptr<Encoder>, encoder);
        QFETCH(QByteArray, avcodecEncoder);

        if (!avcodec_find_encoder_by_name(avcodecEncoder.data())) {
            QSKIP("Skipping because the encoder was not found");
        }

        if (avcodecEncoder.contains("vaapi") && VaapiUtils::instance()->devicePath().isEmpty()) {
            QSKIP("Skipping because hardware encoding is not supported on this device");
        }

        encoder->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
        QVERIFY(encoder->initialize(QSize(512, 512)));
    }

    void testSingleFrame_data()
    {
        QTest::addColumn<QByteArray>("avcodecEncoder");
        QTest::addColumn<int>("maximumFrames");
        for (const auto& codec : {"libx264"_ba, "libopenh264"_ba, "libvpx"_ba, "libvpx-vp9"_ba}) {
            for (int limit : {1, 2, 3}) {
                QTest::addRow("%s-limit-%d", codec.constData(), limit) << codec << limit;
            }
        }
    }

    void testSingleFrame()
    {
        QFETCH(QByteArray, avcodecEncoder);
        QFETCH(int, maximumFrames);
        if (!avcodec_find_encoder_by_name(avcodecEncoder.constData())) {
            QSKIP("Skipping because the encoder was not found");
        }

        TestProduce produce;
        if (avcodecEncoder == "libx264") {
            produce.m_encoder = std::make_unique<TestSoftwareEncoder<LibX264Encoder>>(Encoder::H264Profile::Main, &produce);
        } else if (avcodecEncoder == "libopenh264") {
            produce.m_encoder = std::make_unique<TestSoftwareEncoder<LibOpenH264Encoder>>(Encoder::H264Profile::Main, &produce);
        } else if (avcodecEncoder == "libvpx") {
            produce.m_encoder = std::make_unique<TestSoftwareEncoder<LibVpxEncoder>>(&produce);
        } else {
            produce.m_encoder = std::make_unique<TestSoftwareEncoder<LibVpxVp9Encoder>>(&produce);
        }
        produce.setMaxPendingFrames(maximumFrames);
        QCOMPARE(produce.maxPendingFrames(), maximumFrames);
        produce.m_encoder->setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
        QVERIFY(produce.m_encoder->initialize(QSize(64, 64)));

        PipeWireFrame frame;
        frame.presentationTimestamp = std::chrono::nanoseconds(0);
        produce.processFrame(frame);
        QCOMPARE(produce.m_pendingFilterFrames.load(), 1);

        // Duplicate and too-soon updates must still be throttled after the
        // first frame, even though that first frame is accepted at PTS zero.
        produce.processFrame(frame);
        frame.presentationTimestamp = std::chrono::milliseconds(1);
        produce.processFrame(frame);
        QCOMPARE(produce.m_pendingFilterFrames.load(), 1);

        const auto [filtered, queued] = produce.m_encoder->encodeFrame(maximumFrames);
        QCOMPARE(filtered, 1);
        QCOMPARE(queued, 1);
        QCOMPARE(produce.m_encoder->receivePacket(), 1);
        QCOMPARE(produce.receivedPackets, 1);
        QCOMPARE(produce.lastPts, 0);
        QCOMPARE(produce.m_encoder->receivePacket(), 0);
    }

    void testEncoderPauseDropsAndResumes()
    {
        FrameTrackingProduce produce;
        auto encoder = std::make_unique<FrameTrackingEncoder>(&produce);
        auto tracker = encoder.get();
        produce.m_encoder = std::move(encoder);
        QVERIFY(!produce.encoderPaused());

        std::thread pause([&produce] {
            produce.setEncoderPaused(true);
        });
        pause.join();
        QVERIFY(produce.encoderPaused());
        produce.processFrame(trackingFrame(1, 100));
        produce.processFrame(trackingFrame(2, 200));
        auto cursorFrame = trackingFrame(3, 300);
        cursorFrame.dmabuf.reset();
        cursorFrame.cursor = PipeWireCursor{.position = QPoint(10, 20)};
        produce.processFrame(cursorFrame);
        QCOMPARE(tracker->frames.size(), 0);
        QCOMPARE(produce.m_pendingFilterFrames.load(), 0);

        std::thread resume([&produce] {
            produce.setEncoderPaused(false);
        });
        resume.join();
        QVERIFY(!produce.encoderPaused());
        QCOMPARE(tracker->frames.size(), 0);
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        QCOMPARE(tracker->frames.size(), 1);
        QCOMPARE(tracker->frames.constLast().sequential.value(), quint64(2));
        QCOMPARE(tracker->frames.constLast().cursor->position, QPoint(10, 20));
        QCOMPARE(tracker->processingThread, produce.thread());
        QCOMPARE(produce.lastObservedSequence.value(), quint64(2));
        QVERIFY(!produce.m_lastDroppedFrame.dmabuf);

        produce.setEncoderPaused(false);
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        QCOMPARE(tracker->frames.size(), 1);
    }

    void testEncoderPausePendingResume()
    {
        TestProduce produce;
        auto encoder = std::make_unique<FrameTrackingEncoder>(&produce);
        auto tracker = encoder.get();
        produce.m_encoder = std::move(encoder);
        produce.setEncoderPaused(true);
        produce.processFrame(trackingFrame(1, 100));
        produce.setEncoderPaused(false);
        produce.setEncoderPaused(true);
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        QCOMPARE(tracker->frames.size(), 0);
        QVERIFY(produce.m_lastDroppedFrame.dmabuf);

        produce.setEncoderPaused(false);
        // A new frame arriving before the queued replay supersedes the cache.
        produce.processFrame(trackingFrame(2, 200));
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        QCOMPARE(tracker->frames.size(), 1);
        QCOMPARE(tracker->frames.constLast().sequential.value(), quint64(2));
    }

    void testEncoderPauseWaitsForQueue()
    {
        TestProduce produce;
        auto encoder = std::make_unique<FrameTrackingEncoder>(&produce);
        auto tracker = encoder.get();
        produce.m_encoder = std::move(encoder);
        produce.setMaxPendingFrames(1);
        produce.m_pendingFilterFrames = 1;
        produce.setEncoderPaused(true);
        produce.processFrame(trackingFrame(1, 100));
        produce.setEncoderPaused(false);
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        QCOMPARE(tracker->frames.size(), 0);
        QVERIFY(produce.m_lastDroppedFrame.dmabuf);

        produce.m_pendingFilterFrames = 0;
        produce.handleEncodedFramesChanged();
        QCOMPARE(tracker->frames.size(), 1);
        QVERIFY(!produce.m_lastDroppedFrame.dmabuf);
    }

    void testEncoderPauseStopsReplayOnShutdown()
    {
        TestProduce produce;
        auto encoder = std::make_unique<FrameTrackingEncoder>(&produce);
        auto tracker = encoder.get();
        produce.m_encoder = std::move(encoder);
        produce.setEncoderPaused(true);
        produce.processFrame(trackingFrame(1, 100));
        produce.setEncoderPaused(false);
        produce.deactivate();
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        produce.processFrame(trackingFrame(2, 200));
        QCOMPARE(tracker->frames.size(), 0);
        QVERIFY(!produce.m_lastDroppedFrame.dmabuf);
    }

    void testEncoderPauseWithoutEncoder()
    {
        TestProduce produce;
        produce.processFrame(trackingFrame(1, 100));
        produce.setEncoderPaused(true);
        produce.m_lastDroppedFrame = trackingFrame(2, 200);
        produce.setEncoderPaused(false);
        QCoreApplication::sendPostedEvents(&produce, QEvent::MetaCall);
        QCOMPARE(produce.m_pendingFilterFrames.load(), 0);
        produce.destroy();
        QVERIFY(!produce.m_lastDroppedFrame.dmabuf);
    }

    void testX264QualityOptions_data()
    {
        QTest::addColumn<int>("quality");
        QTest::addColumn<double>("expectedCrf");
        QTest::addRow("default") << -1 << 23.0;
        QTest::addRow("lowest") << 0 << 28.0;
        QTest::addRow("one-percent") << 1 << 27.89;
        QTest::addRow("quarter") << 25 << 25.25;
        QTest::addRow("middle") << 50 << 22.5;
        QTest::addRow("three-quarters") << 75 << 19.75;
        QTest::addRow("ninety-nine-percent") << 99 << 17.11;
        QTest::addRow("highest") << 100 << 17.0;
    }

    void testX264QualityOptions()
    {
        QFETCH(int, quality);
        QFETCH(double, expectedCrf);
        const auto value = quality < 0 ? std::nullopt : std::optional<quint8>(quality);
        TestX264Encoder encoder(Encoder::H264Profile::Main, m_produce.get());
        encoder.setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
        QCOMPARE(encoder.percentageToAbsoluteQuality(value), value ? int(expectedCrf) : -1);
        encoder.setQuality(value);
        auto options = encoder.buildEncodingOptions();
        const auto entry = av_dict_get(options, "crf", nullptr, 0);
        const QByteArray crf = entry ? QByteArray(entry->value) : QByteArray();
        av_dict_free(&options);

        bool ok = false;
        const auto actualCrf = crf.toDouble(&ok);
        QVERIFY(ok);
        QCOMPARE(actualCrf, expectedCrf);
    }

    void testX264QualityInitialization_data()
    {
        testX264QualityOptions_data();
    }

    void testX264QualityInitialization()
    {
        QFETCH(int, quality);
        QFETCH(double, expectedCrf);
        if (!avcodec_find_encoder_by_name("libx264")) {
            QSKIP("Skipping because the encoder was not found");
        }
        TestX264Encoder encoder(Encoder::H264Profile::Main, m_produce.get());
        encoder.setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);
        encoder.setQuality(quality < 0 ? std::nullopt : std::optional<quint8>(quality));
        QVERIFY(encoder.initialize(QSize(64, 64)));

        double crf = 0;
        QCOMPARE(av_opt_get_double(encoder.avCodecContext(), "crf", AV_OPT_SEARCH_CHILDREN, &crf), 0);
        QVERIFY(std::abs(crf - expectedCrf) < 0.00001);
    }

    void testVP9QualityOptions_data()
    {
        QTest::addColumn<int>("quality");
        QTest::addColumn<int>("expectedCrf");
        QTest::addColumn<int>("height");
        QTest::addRow("default") << -1 << 31 << 0;
        QTest::addRow("lowest") << 0 << 37 << 0;
        QTest::addRow("quarter") << 25 << 31 << 0;
        QTest::addRow("middle") << 50 << 26 << 0;
        QTest::addRow("three-quarters") << 75 << 20 << 0;
        QTest::addRow("highest") << 100 << 15 << 0;
        QTest::addRow("default-720p") << -1 << 31 << 720;
        QTest::addRow("default-1080p") << -1 << 31 << 1080;
        QTest::addRow("default-above-1080p") << -1 << 24 << 1081;
        QTest::addRow("default-1440p") << -1 << 24 << 1440;
        QTest::addRow("default-above-1440p") << -1 << 15 << 1441;
        QTest::addRow("default-2160p") << -1 << 15 << 2160;
        for (int height : {1080, 1440, 2160}) {
            QTest::addRow("lowest-%dp", height) << 0 << 37 << height;
            QTest::addRow("middle-%dp", height) << 50 << 26 << height;
            QTest::addRow("highest-%dp", height) << 100 << 15 << height;
        }
    }

    void testVP9QualityOptions()
    {
        QFETCH(int, quality);
        QFETCH(int, expectedCrf);
        QFETCH(int, height);
        const auto value = quality < 0 ? std::nullopt : std::optional<quint8>(quality);
        TestVP9Encoder encoder(m_produce.get());
        encoder.setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);

        // Conversion must use the argument, not depend on a previously stored
        // quality or an initialized codec context.
        QCOMPARE(encoder.percentageToAbsoluteQuality(value), value ? expectedCrf : -1);
        if (height > 0) {
            QVERIFY(encoder.setFrameHeight(height));
        }
        encoder.setQuality(value);
        auto options = encoder.buildEncodingOptions();
        const auto optionValue = [options](const char* key) {
            const auto entry = av_dict_get(options, key, nullptr, 0);
            return entry ? QByteArray(entry->value) : QByteArray();
        };
        const auto crf = optionValue("crf");
        const auto qmin = optionValue("qmin");
        const auto qmax = optionValue("qmax");
        const auto lag = optionValue("lag-in-frames");
        av_dict_free(&options);

        QCOMPARE(crf, QByteArray::number(expectedCrf));
        QCOMPARE(qmin, QByteArray::number(expectedCrf / 2));
        QCOMPARE(qmax, QByteArray::number(int(expectedCrf * 1.5)));
        QCOMPARE(lag, "0"_ba);
    }

    void testVAAPILowLatencyOptions()
    {
        using Preference = PipeWireBaseEncodedStream::EncodingPreference;
        TestVAAPIEncoder encoder(Encoder::H264Profile::Main, m_produce.get());
        for (auto preference : {Preference::NoPreference, Preference::Speed, Preference::Quality, Preference::Size}) {
            encoder.setEncodingPreference(preference);
            auto options = encoder.buildEncodingOptions();
            const auto depth = av_dict_get(options, "async_depth", nullptr, 0);
            const QByteArray value = depth ? QByteArray(depth->value) : QByteArray();
            av_dict_free(&options);
            QCOMPARE(value, preference == Preference::Speed ? "1"_ba : QByteArray());
        }
    }

private:
    std::unique_ptr<TestProduce> m_produce;
};

QTEST_MAIN(TestEncoder)

#include "TestEncoder.moc"
