#include "Timecode.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace libera_timecode {

namespace {

constexpr std::array<FrameRateInfo, 7> kFrameRates{{
    {"23.976",    24000.0 / 1001.0, 24, false},
    {"24",        24.0,             24, false},
    {"25",        25.0,             25, false},
    {"29.97 DF",  30000.0 / 1001.0, 30, true},
    {"29.97 NDF", 30000.0 / 1001.0, 30, false},
    {"30 DF",     30.0,             30, true},
    {"30 NDF",    30.0,             30, false},
}};

} // namespace

const FrameRateInfo& frameRateInfo(FrameRate rate) {
    return kFrameRates[static_cast<int>(rate)];
}

const char* frameRateLabel(FrameRate rate) {
    return kFrameRates[static_cast<int>(rate)].label;
}

int numFrameRates() { return static_cast<int>(kFrameRates.size()); }

FrameRate frameRateAt(int index) {
    if (index < 0) index = 0;
    if (index >= numFrameRates()) index = numFrameRates() - 1;
    return static_cast<FrameRate>(index);
}

int indexOfFrameRate(FrameRate rate) {
    return static_cast<int>(rate);
}

TimecodeFields secondsToFields(double seconds, FrameRate rate) {
    if (seconds < 0.0 || !std::isfinite(seconds)) seconds = 0.0;
    const auto& info = frameRateInfo(rate);

    const long long frameNumber =
        static_cast<long long>(std::floor(seconds * info.nominalFps + 1e-9));
    return frameIndexToFields(frameNumber, rate);
}

TimecodeFields frameIndexToFields(long long frameNumber, FrameRate rate) {
    if (frameNumber < 0) frameNumber = 0;
    const auto& info = frameRateInfo(rate);

    TimecodeFields out;
    out.dropFrame = info.dropFrame;
    out.integerFps = info.integerFps;

    if (!info.dropFrame) {
        // Plain breakdown using nominal fps.
        const long long fps = info.integerFps;
        out.frames  = static_cast<int>(frameNumber % fps);
        const long long ts = frameNumber / fps;
        out.seconds = static_cast<int>(ts % 60);
        out.minutes = static_cast<int>((ts / 60) % 60);
        out.hours   = static_cast<int>((ts / 3600) % 24);
        return out;
    }

    // Drop-frame algorithm (works for both 29.97 DF and 30 DF).
    // We're encoding wall-time seconds into a counted-frame stream that drops
    // 2 frames at the start of every minute except every 10th minute.
    const int fps = info.integerFps;                 // 30
    const int dropPerMinute = (fps == 30) ? 2 : 4;   // 4 for 60 DF if ever needed
    const long long framesPerMin = (long long)fps * 60 - dropPerMinute;
    const long long framesPer10Min = (long long)fps * 60 * 10 - dropPerMinute * 9;

    const long long d = frameNumber / framesPer10Min;
    const long long m = frameNumber % framesPer10Min;

    if (m > dropPerMinute) {
        frameNumber += (long long)dropPerMinute * 9 * d
                     + (long long)dropPerMinute * ((m - dropPerMinute) / framesPerMin);
    } else {
        frameNumber += (long long)dropPerMinute * 9 * d;
    }

    out.frames  = static_cast<int>(frameNumber % fps);
    const long long ts = frameNumber / fps;
    out.seconds = static_cast<int>(ts % 60);
    out.minutes = static_cast<int>((ts / 60) % 60);
    out.hours   = static_cast<int>((ts / 3600) % 24);
    return out;
}

std::string formatFields(const TimecodeFields& fields, bool useDfPunctuation) {
    char buf[32];
    const char fsep = (useDfPunctuation && fields.dropFrame) ? ';' : ':';
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d",
                  fields.hours, fields.minutes, fields.seconds, fsep, fields.frames);
    return std::string(buf);
}

} // namespace libera_timecode
