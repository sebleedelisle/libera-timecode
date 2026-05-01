#include "Timecode.h"
#include "outputs/LtcProtocol.h"
#include "outputs/OutputTiming.h"
#include "outputs/TimecodeProtocol.h"

#include <ltc.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace libera_timecode;

int gFailures = 0;

std::string fieldsString(const TimecodeFields& tc) {
    return formatFields(tc, true);
}

void fail(const char* file, int line, const std::string& message) {
    ++gFailures;
    std::cerr << file << ":" << line << ": " << message << "\n";
}

#define EXPECT_TRUE(expr)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            fail(__FILE__, __LINE__, std::string("expected true: ") + #expr);   \
        }                                                                       \
    } while (false)

#define EXPECT_EQ(actual, expected)                                             \
    do {                                                                        \
        const auto actualValue = (actual);                                      \
        const auto expectedValue = (expected);                                  \
        if (!(actualValue == expectedValue)) {                                  \
            std::ostringstream os;                                              \
            os << "expected " << #actual << " == " << #expected                \
               << " (" << +actualValue << " vs " << +expectedValue << ")";    \
            fail(__FILE__, __LINE__, os.str());                                 \
        }                                                                       \
    } while (false)

void expectNear(double actual, double expected, double tolerance, const char* label) {
    if (std::fabs(actual - expected) > tolerance) {
        std::ostringstream os;
        os << label << " expected " << expected << " got " << actual;
        fail(__FILE__, __LINE__, os.str());
    }
}

template <typename T, std::size_t N>
void expectArrayEq(const std::array<T, N>& actual,
                   const std::array<T, N>& expected,
                   const char* label) {
    for (std::size_t i = 0; i < N; ++i) {
        if (actual[i] != expected[i]) {
            std::ostringstream os;
            os << label << "[" << i << "] expected " << +expected[i]
               << " got " << +actual[i];
            fail(__FILE__, __LINE__, os.str());
        }
    }
}

void expectFields(const TimecodeFields& actual,
                  int h,
                  int m,
                  int s,
                  int f,
                  bool dropFrame) {
    if (actual.hours != h || actual.minutes != m || actual.seconds != s
        || actual.frames != f || actual.dropFrame != dropFrame) {
        std::ostringstream os;
        os << "expected "
           << h << ":" << m << ":" << s << ":" << f
           << " dropFrame=" << dropFrame
           << " got " << fieldsString(actual)
           << " dropFrame=" << actual.dropFrame;
        fail(__FILE__, __LINE__, os.str());
    }
}

double seconds(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

void testTimecodeBoundaries() {
    expectFields(frameIndexToFields(0, FrameRate::fps_30_NDF), 0, 0, 0, 0, false);
    expectFields(frameIndexToFields(29, FrameRate::fps_30_NDF), 0, 0, 0, 29, false);
    expectFields(frameIndexToFields(30, FrameRate::fps_30_NDF), 0, 0, 1, 0, false);
    expectFields(frameIndexToFields(24 * 60 * 60 * 30LL, FrameRate::fps_30_NDF),
                 0, 0, 0, 0, false);

    expectFields(frameIndexToFields(1799, FrameRate::fps_29_97_DF), 0, 0, 59, 29, true);
    expectFields(frameIndexToFields(1800, FrameRate::fps_29_97_DF), 0, 1, 0, 2, true);
    expectFields(frameIndexToFields(17981, FrameRate::fps_29_97_DF), 0, 9, 59, 29, true);
    expectFields(frameIndexToFields(17982, FrameRate::fps_29_97_DF), 0, 10, 0, 0, true);

    expectFields(secondsToFields(1.0, FrameRate::fps_25), 0, 0, 1, 0, false);
    expectFields(secondsToFields(-1.0, FrameRate::fps_25), 0, 0, 0, 0, false);
}

void testProtocolPackets() {
    TimecodeFields tc;
    tc.hours = 1;
    tc.minutes = 2;
    tc.seconds = 3;
    tc.frames = 4;
    tc.integerFps = 30;

    expectArrayEq(
        timecode_protocol::buildMidiFullFrame(tc, FrameRate::fps_30_NDF),
        std::array<unsigned char, 10>{0xF0, 0x7F, 0x7F, 0x01, 0x01,
                                      0x61, 0x02, 0x03, 0x04, 0xF7},
        "mtc-full");
    expectArrayEq(
        timecode_protocol::buildMidiQuarterFrame(0, tc, FrameRate::fps_30_NDF),
        std::array<unsigned char, 2>{0xF1, 0x04},
        "mtc-qf0");
    expectArrayEq(
        timecode_protocol::buildMidiQuarterFrame(7, tc, FrameRate::fps_30_NDF),
        std::array<unsigned char, 2>{0xF1, 0x76},
        "mtc-qf7");
    EXPECT_EQ(timecode_protocol::mtcRateCode(FrameRate::fps_25), 1);
    EXPECT_EQ(timecode_protocol::mtcRateCode(FrameRate::fps_29_97_DF), 2);

    expectArrayEq(
        timecode_protocol::buildArtnetTimecodePacket(tc, FrameRate::fps_25),
        std::array<uint8_t, 19>{'A', 'r', 't', '-', 'N', 'e', 't', '\0',
                                0x00, 0x97, 0x00, 0x0E, 0x00, 0x00,
                                4, 3, 2, 1, 1},
        "artnet");
    EXPECT_EQ(timecode_protocol::artnetTypeForFps(FrameRate::fps_24), 0);
    EXPECT_EQ(timecode_protocol::artnetTypeForFps(FrameRate::fps_30_DF), 2);
    EXPECT_EQ(timecode_protocol::artnetTypeForFps(FrameRate::fps_30_NDF), 3);

    const auto sntc = timecode_protocol::buildSntcPacket(tc, FrameRate::fps_30_NDF, "AB\001D");
    EXPECT_EQ(sntc[0], static_cast<uint8_t>('S'));
    EXPECT_EQ(sntc[4], static_cast<uint8_t>(30));
    EXPECT_EQ(sntc[5], static_cast<uint8_t>(1));
    EXPECT_EQ(sntc[8], static_cast<uint8_t>(4));
    EXPECT_EQ(sntc[10], static_cast<uint8_t>('A'));
    EXPECT_EQ(sntc[11], static_cast<uint8_t>('B'));
    EXPECT_EQ(sntc[12], static_cast<uint8_t>(' '));
    EXPECT_EQ(sntc[13], static_cast<uint8_t>('D'));
    EXPECT_EQ(sntc[14], static_cast<uint8_t>(0x02));
    EXPECT_EQ(sntc[15], timecode_protocol::sntcChecksum(sntc));
}

void testTimingHelpers() {
    TransportSnapshot snap;
    snap.playheadSeconds = 1.25;
    snap.rate = 1.05;
    snap.playing = true;

    expectNear(seconds(output_timing::quarterFrameInterval(snap, FrameRate::fps_30_NDF)),
               1.0 / (30.0 * 1.05 * 4.0), 1.0e-9, "quarter frame interval +5%");
    expectNear(seconds(output_timing::frameKeepaliveInterval(snap, FrameRate::fps_30_NDF)),
               1.5 / (30.0 * 1.05), 1.0e-9, "keepalive interval +5%");
    expectNear(seconds(output_timing::nextFrameBoundaryDelay(snap, FrameRate::fps_30_NDF)),
               0.5 / (30.0 * 1.05), 1.0e-9, "next frame delay +5%");

    snap.rate = 0.95;
    expectNear(seconds(output_timing::quarterFrameInterval(snap, FrameRate::fps_30_NDF)),
               1.0 / (30.0 * 0.95 * 4.0), 1.0e-9, "quarter frame interval -5%");

    snap.rate = 0.0;
    snap.playing = false;
    expectNear(seconds(output_timing::nextFrameBoundaryDelay(snap, FrameRate::fps_30_NDF)),
               0.020, 1.0e-9, "paused poll interval");

    snap.clockMode = true;
    EXPECT_EQ(output_timing::activeRate(snap), 1.0);
}

void testVarispeedContinuity() {
    const std::array<double, 3> rates{0.95, 1.0, 1.05};
    constexpr double pollSeconds = 0.002;
    constexpr double durationSeconds = 30.0;

    for (int i = 0; i < numFrameRates(); ++i) {
        const FrameRate fps = frameRateAt(i);
        const double nominal = frameRateInfo(fps).nominalFps;
        for (double rate : rates) {
            long long lastFrame = -1;
            int emitted = 0;
            for (double t = 0.0; t < durationSeconds; t += pollSeconds) {
                const long long frame = static_cast<long long>(
                    std::floor(t * nominal * rate + 1.0e-9));
                if (frame == lastFrame) continue;
                if (lastFrame >= 0) {
                    EXPECT_EQ(frame - lastFrame, 1);
                }
                lastFrame = frame;
                ++emitted;
            }

            const int expectedApprox = static_cast<int>(durationSeconds * nominal * rate);
            EXPECT_TRUE(emitted >= expectedApprox - 1);
            EXPECT_TRUE(emitted <= expectedApprox + 2);
        }
    }
}

void testLtcProtocol() {
    constexpr double sampleRate = 48000.0;
    const TimecodeFields tc = frameIndexToFields(1800, FrameRate::fps_29_97_DF);

    LTCEncoder* enc = ltc_encoder_create(
        sampleRate,
        ltc_protocol::effectiveFps(FrameRate::fps_29_97_DF),
        ltc_protocol::standardForFps(FrameRate::fps_29_97_DF),
        0);
    EXPECT_TRUE(enc != nullptr);
    if (!enc) return;

    ltc_protocol::setEncoderTimecode(enc, tc, FrameRate::fps_29_97_DF);
    LTCFrame frame;
    ltc_encoder_get_frame(enc, &frame);
    EXPECT_EQ(frame.dfbit, 1);

    SMPTETimecode decoded;
    ltc_frame_to_time(&decoded, &frame, 0);
    EXPECT_EQ(decoded.hours, 0);
    EXPECT_EQ(decoded.mins, 1);
    EXPECT_EQ(decoded.secs, 0);
    EXPECT_EQ(decoded.frame, 2);

    const size_t bufSize = ltc_encoder_get_buffersize(enc);
    std::vector<ltcsnd_sample_t> buf(bufSize);

    EXPECT_TRUE(ltc_protocol::encodeFrame(enc, tc, FrameRate::fps_29_97_DF, 1.0));
    const int nominalSamples = ltc_encoder_copy_buffer(enc, buf.data());
    EXPECT_TRUE(nominalSamples > 0);

    EXPECT_TRUE(ltc_protocol::encodeFrame(enc, tc, FrameRate::fps_29_97_DF, 1.0 / 1.05));
    const int fastSamples = ltc_encoder_copy_buffer(enc, buf.data());
    EXPECT_TRUE(fastSamples > 0);
    EXPECT_TRUE(fastSamples < nominalSamples);

    EXPECT_TRUE(ltc_encoder_set_buffersize(
        enc,
        sampleRate,
        ltc_protocol::effectiveFps(FrameRate::fps_29_97_DF) * 0.5) == 0);
    buf.assign(ltc_encoder_get_buffersize(enc), 0);
    EXPECT_TRUE(ltc_protocol::encodeFrame(enc, tc, FrameRate::fps_29_97_DF, 1.0 / 0.95));
    const int slowSamples = ltc_encoder_copy_buffer(enc, buf.data());
    EXPECT_TRUE(slowSamples > nominalSamples);

    ltc_encoder_free(enc);
}

void testLtcDecodeRoundTrip() {
    constexpr double sampleRate = 48000.0;
    const FrameRate fps = FrameRate::fps_30_NDF;
    const double nominalFps = ltc_protocol::effectiveFps(fps);

    LTCEncoder* enc = ltc_encoder_create(
        sampleRate,
        nominalFps,
        ltc_protocol::standardForFps(fps),
        0);
    EXPECT_TRUE(enc != nullptr);
    if (!enc) return;

    std::vector<ltcsnd_sample_t> audio;
    std::vector<TimecodeFields> expected;
    std::vector<ltcsnd_sample_t> frameBuf(ltc_encoder_get_buffersize(enc));

    for (long long i = 0; i < 8; ++i) {
        const TimecodeFields tc = frameIndexToFields(30 + i, fps);
        expected.push_back(tc);
        EXPECT_TRUE(ltc_protocol::encodeFrame(enc, tc, fps, 1.0));
        const int n = ltc_encoder_copy_buffer(enc, frameBuf.data());
        audio.insert(audio.end(), frameBuf.begin(), frameBuf.begin() + n);
    }

    LTCDecoder* dec = ltc_decoder_create(
        static_cast<int>(std::lround(sampleRate / nominalFps)),
        32);
    EXPECT_TRUE(dec != nullptr);
    if (!dec) {
        ltc_encoder_free(enc);
        return;
    }

    ltc_decoder_write(dec, audio.data(), audio.size(), 0);

    std::vector<TimecodeFields> decodedFields;
    LTCFrameExt ext;
    while (ltc_decoder_read(dec, &ext)) {
        SMPTETimecode decoded;
        ltc_frame_to_time(&decoded, &ext.ltc, 0);
        TimecodeFields tc;
        tc.hours = decoded.hours;
        tc.minutes = decoded.mins;
        tc.seconds = decoded.secs;
        tc.frames = decoded.frame;
        tc.dropFrame = false;
        tc.integerFps = 30;
        decodedFields.push_back(tc);
    }

    EXPECT_TRUE(!decodedFields.empty());
    bool matchedAny = false;
    for (const auto& got : decodedFields) {
        for (const auto& want : expected) {
            if (got.hours == want.hours && got.minutes == want.minutes
                && got.seconds == want.seconds && got.frames == want.frames) {
                matchedAny = true;
            }
        }
    }
    EXPECT_TRUE(matchedAny);

    ltc_decoder_free(dec);
    ltc_encoder_free(enc);
}

} // namespace

int main() {
    testTimecodeBoundaries();
    testProtocolPackets();
    testTimingHelpers();
    testVarispeedContinuity();
    testLtcProtocol();
    testLtcDecodeRoundTrip();

    if (gFailures != 0) {
        std::cerr << gFailures << " test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "All timecode tests passed\n";
    return EXIT_SUCCESS;
}
