#pragma once

#include "Timecode.h"

#include <string>

namespace libera_timecode {

struct WindowSettings {
    int x{-1};
    int y{-1};
    int w{720};
    int h{540};
};

struct SmpteSettings {
    bool enabled{false};
    FrameRate fps{FrameRate::fps_30_NDF};
    std::string audioDevice; // empty = default
    int sampleRate{48000};
    float level{0.5f};       // 0..1, peak amplitude
    // 0 = mono left, 1 = mono right, 2 = both channels
    int channelMode{2};
};

struct MidiSettings {
    bool enabled{false};
    FrameRate fps{FrameRate::fps_30_NDF};
    std::string portName;    // empty = first available
    bool createVirtualPort{true}; // macOS/Linux can host their own port
};

struct ArtnetSettings {
    bool enabled{false};
    FrameRate fps{FrameRate::fps_30_NDF};
    std::string targetIp{"255.255.255.255"};
    int port{6454};
};

struct SntcSettings {
    bool enabled{false};
    FrameRate fps{FrameRate::fps_30_NDF};
    std::string targetIp{"255.255.255.255"};
    int port{8405};
    std::string senderId{"LIBE"}; // exactly 4 ASCII characters
};

struct AppSettings {
    WindowSettings window;
    double tapJumpSeconds{10.0};
    SmpteSettings smpte;
    MidiSettings midi;
    ArtnetSettings artnet;
    SntcSettings sntc;
};

void loadSettings(AppSettings& out);
void saveSettings(const AppSettings& in);

} // namespace libera_timecode
