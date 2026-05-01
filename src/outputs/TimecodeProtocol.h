#pragma once

#include "../Timecode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace libera_timecode {
namespace timecode_protocol {

inline uint8_t mtcRateCode(FrameRate r) {
    switch (r) {
    case FrameRate::fps_23_976:
    case FrameRate::fps_24:        return 0;
    case FrameRate::fps_25:        return 1;
    case FrameRate::fps_29_97_DF:
    case FrameRate::fps_29_97_NDF: return 2;
    case FrameRate::fps_30_DF:
    case FrameRate::fps_30_NDF:    return 3;
    }
    return 3;
}

inline std::array<unsigned char, 10> buildMidiFullFrame(
    const TimecodeFields& tc,
    FrameRate fps) {
    const uint8_t rateCode = mtcRateCode(fps);
    return {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        static_cast<unsigned char>(((rateCode & 0x03) << 5) | (tc.hours & 0x1F)),
        static_cast<unsigned char>(tc.minutes & 0x3F),
        static_cast<unsigned char>(tc.seconds & 0x3F),
        static_cast<unsigned char>(tc.frames & 0x1F),
        0xF7,
    };
}

inline std::array<unsigned char, 2> buildMidiQuarterFrame(
    int piece,
    const TimecodeFields& tc,
    FrameRate fps) {
    const uint8_t rateCode = mtcRateCode(fps);
    uint8_t value = 0;
    switch (piece & 0x07) {
    case 0: value = tc.frames  & 0x0F; break;
    case 1: value = (tc.frames >> 4) & 0x01; break;
    case 2: value = tc.seconds & 0x0F; break;
    case 3: value = (tc.seconds >> 4) & 0x03; break;
    case 4: value = tc.minutes & 0x0F; break;
    case 5: value = (tc.minutes >> 4) & 0x03; break;
    case 6: value = tc.hours   & 0x0F; break;
    case 7: value = ((tc.hours >> 4) & 0x01) | ((rateCode & 0x03) << 1); break;
    }

    return {
        0xF1,
        static_cast<unsigned char>(((piece & 0x07) << 4) | (value & 0x0F)),
    };
}

inline uint8_t artnetTypeForFps(FrameRate r) {
    // Art-Net defines four timecode type codes. Non-exact frame rates map to
    // the closest available code.
    switch (r) {
    case FrameRate::fps_23_976:
    case FrameRate::fps_24:        return 0;
    case FrameRate::fps_25:        return 1;
    case FrameRate::fps_29_97_DF:
    case FrameRate::fps_30_DF:     return 2;
    case FrameRate::fps_29_97_NDF:
    case FrameRate::fps_30_NDF:    return 3;
    }
    return 3;
}

inline std::array<uint8_t, 19> buildArtnetTimecodePacket(
    const TimecodeFields& tc,
    FrameRate fps) {
    return {
        'A', 'r', 't', '-', 'N', 'e', 't', '\0',
        0x00, 0x97,
        0x00, 0x0E,
        0x00, 0x00,
        static_cast<uint8_t>(tc.frames),
        static_cast<uint8_t>(tc.seconds),
        static_cast<uint8_t>(tc.minutes),
        static_cast<uint8_t>(tc.hours),
        artnetTypeForFps(fps),
    };
}

inline uint8_t sntcChecksum(const std::array<uint8_t, 16>& packet) {
    uint8_t cs = 0;
    for (int i = 0; i < 15; ++i) cs ^= packet[static_cast<std::size_t>(i)];
    return cs;
}

inline std::array<uint8_t, 16> buildSntcPacket(
    const TimecodeFields& tc,
    FrameRate fps,
    const std::string& senderId) {
    std::array<uint8_t, 16> out{
        'S', 'N', 'T', 'C',
        static_cast<uint8_t>(std::clamp(frameRateInfo(fps).integerFps, 1, 240)),
        static_cast<uint8_t>(tc.hours),
        static_cast<uint8_t>(tc.minutes),
        static_cast<uint8_t>(tc.seconds),
        static_cast<uint8_t>(tc.frames),
        0,
        ' ', ' ', ' ', ' ',
        0x02,
        0,
    };

    for (std::size_t i = 0; i < 4 && i < senderId.size(); ++i) {
        const char c = senderId[i];
        out[10 + i] = static_cast<uint8_t>((c >= 32 && c < 127) ? c : ' ');
    }
    out[15] = sntcChecksum(out);
    return out;
}

} // namespace timecode_protocol
} // namespace libera_timecode
