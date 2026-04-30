#pragma once

#include <cstdint>
#include <string>

namespace libera_timecode {

enum class FrameRate {
    fps_23_976,
    fps_24,
    fps_25,
    fps_29_97_DF,
    fps_29_97_NDF,
    fps_30_DF,
    fps_30_NDF,
};

struct FrameRateInfo {
    const char* label;
    double nominalFps;     // exact rate ticks pass at (23.976, 25, 29.97, 30)
    int integerFps;        // counted-frame ceiling per second (24, 25, 30)
    bool dropFrame;
};

const FrameRateInfo& frameRateInfo(FrameRate rate);
const char* frameRateLabel(FrameRate rate);
int numFrameRates();
FrameRate frameRateAt(int index);
int indexOfFrameRate(FrameRate rate);

struct TimecodeFields {
    int hours{0};
    int minutes{0};
    int seconds{0};
    int frames{0};
    bool dropFrame{false};
    int integerFps{30};
};

// Convert wall-time seconds (>= 0) to HH:MM:SS:FF for the given frame rate.
// Negative input is treated as 0.
TimecodeFields secondsToFields(double seconds, FrameRate rate);

// Format a TimecodeFields as "HH:MM:SS:FF" (drop-frame uses ';' separator
// before the frames field per SMPTE convention if useDfPunctuation is true).
std::string formatFields(const TimecodeFields& fields, bool useDfPunctuation = false);

} // namespace libera_timecode
