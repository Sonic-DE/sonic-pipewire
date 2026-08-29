// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include <QtTest>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersrc.h>
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

class TestVP9Encoder : public LibVpxVp9Encoder {
public:
    using LibVpxVp9Encoder::buildEncodingOptions;
    using LibVpxVp9Encoder::LibVpxVp9Encoder;
    using LibVpxVp9Encoder::percentageToAbsoluteQuality;
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

    void testVP9QualityOptions_data()
    {
        QTest::addColumn<int>("quality");
        QTest::addColumn<int>("expectedCrf");
        QTest::addRow("default") << -1 << 31;
        QTest::addRow("lowest") << 0 << 37;
        QTest::addRow("quarter") << 25 << 31;
        QTest::addRow("middle") << 50 << 26;
        QTest::addRow("three-quarters") << 75 << 20;
        QTest::addRow("highest") << 100 << 15;
    }

    void testVP9QualityOptions()
    {
        QFETCH(int, quality);
        QFETCH(int, expectedCrf);
        const auto value = quality < 0 ? std::nullopt : std::optional<quint8>(quality);
        TestVP9Encoder encoder(m_produce.get());
        encoder.setEncodingPreference(PipeWireBaseEncodedStream::EncodingPreference::Speed);

        // Conversion must use the argument, not depend on a previously stored
        // quality or an initialized codec context.
        QCOMPARE(encoder.percentageToAbsoluteQuality(value), value ? expectedCrf : -1);
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
