#pragma once

#include "../Timecode.h"

#include <ltc.h>

#include <cmath>
#include <cstring>

namespace libera_timecode {
namespace ltc_protocol {

inline LTC_TV_STANDARD standardForFps(FrameRate r) {
    switch (r) {
    case FrameRate::fps_23_976:
    case FrameRate::fps_24:        return LTC_TV_FILM_24;
    case FrameRate::fps_25:        return LTC_TV_625_50;
    case FrameRate::fps_29_97_DF:
    case FrameRate::fps_29_97_NDF: return LTC_TV_525_60;
    case FrameRate::fps_30_DF:
    case FrameRate::fps_30_NDF:    return LTC_TV_1125_60;
    }
    return LTC_TV_525_60;
}

inline double effectiveFps(FrameRate r) {
    return frameRateInfo(r).nominalFps;
}

inline long long frameIndexForSeconds(double seconds, FrameRate fps) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    return static_cast<long long>(
        std::floor(seconds * frameRateInfo(fps).nominalFps + 1.0e-9));
}

inline void setEncoderTimecode(LTCEncoder* enc,
                               const TimecodeFields& tc,
                               FrameRate fps) {
    SMPTETimecode smpte;
    std::memset(&smpte, 0, sizeof(smpte));
    std::strncpy(smpte.timezone, "+0000", sizeof(smpte.timezone) - 1);
    smpte.years = 0;
    smpte.months = 1;
    smpte.days = 1;
    smpte.hours = static_cast<unsigned char>(tc.hours);
    smpte.mins = static_cast<unsigned char>(tc.minutes);
    smpte.secs = static_cast<unsigned char>(tc.seconds);
    smpte.frame = static_cast<unsigned char>(tc.frames);
    ltc_encoder_set_timecode(enc, &smpte);

    LTCFrame frame;
    ltc_encoder_get_frame(enc, &frame);
    frame.dfbit = frameRateInfo(fps).dropFrame ? 1 : 0;
    ltc_frame_set_parity(&frame, standardForFps(fps));
    ltc_encoder_set_frame(enc, &frame);
}

inline bool encodeFrame(LTCEncoder* enc,
                        const TimecodeFields& tc,
                        FrameRate fps,
                        double speed) {
    if (!enc) return false;
    if (!std::isfinite(speed) || speed <= 0.0) speed = 1.0;

    setEncoderTimecode(enc, tc, fps);

    int err = 0;
    for (int byte = 0; byte < 10; ++byte) {
        err |= ltc_encoder_encode_byte(enc, byte, speed);
    }
    return err == 0;
}

} // namespace ltc_protocol
} // namespace libera_timecode
