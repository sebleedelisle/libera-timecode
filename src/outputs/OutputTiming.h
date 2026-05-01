#pragma once

#include "../Timecode.h"
#include "../TimecodeEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace libera_timecode {
namespace output_timing {

constexpr double kMinActiveRate = 1.0e-6;

inline int labelFrameNumber(const TimecodeFields& tc) {
    return ((tc.hours * 60 + tc.minutes) * 60 + tc.seconds)
         * tc.integerFps + tc.frames;
}

inline double activeRate(const TransportSnapshot& snap) {
    if (snap.clockMode) return 1.0;
    const double rate = std::abs(snap.rate);
    return std::isfinite(rate) ? rate : 0.0;
}

inline double activeRateOrOne(const TransportSnapshot& snap) {
    const double rate = activeRate(snap);
    return (rate > kMinActiveRate) ? rate : 1.0;
}

inline std::chrono::steady_clock::duration steadyDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
}

inline std::chrono::steady_clock::duration frameKeepaliveInterval(
    const TransportSnapshot& snap,
    FrameRate fps) {
    const double effectiveFps = frameRateInfo(fps).nominalFps * activeRateOrOne(snap);
    return steadyDuration(1.5 / std::max(1.0, effectiveFps));
}

inline std::chrono::steady_clock::duration quarterFrameInterval(
    const TransportSnapshot& snap,
    FrameRate fps) {
    const double effectiveFps = frameRateInfo(fps).nominalFps * activeRateOrOne(snap);
    return steadyDuration(1.0 / std::max(1.0, effectiveFps * 4.0));
}

inline std::chrono::steady_clock::duration nextFrameBoundaryDelay(
    const TransportSnapshot& snap,
    FrameRate fps) {
    const double rate = activeRate(snap);
    if (rate <= kMinActiveRate) {
        return steadyDuration(0.020);
    }

    const auto& info = frameRateInfo(fps);
    const double framePos = std::max(0.0, snap.playheadSeconds) * info.nominalFps;

    double framesUntilBoundary = 1.0;
    if (snap.rate < -kMinActiveRate) {
        const double frac = framePos - std::floor(framePos);
        framesUntilBoundary = (frac > 1.0e-6) ? frac : 1.0;
    } else {
        framesUntilBoundary = std::floor(framePos + 1.0) - framePos;
        if (framesUntilBoundary < 1.0e-6) framesUntilBoundary = 1.0;
    }

    const double seconds = framesUntilBoundary / (info.nominalFps * rate);
    return steadyDuration(std::clamp(seconds, 0.0005, 0.050));
}

inline double ltcSpeedFromSnapshot(const TransportSnapshot& snap) {
    return 1.0 / activeRateOrOne(snap);
}

} // namespace output_timing
} // namespace libera_timecode
