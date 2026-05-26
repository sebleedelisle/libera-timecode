#include "Settings.h"

#include "Paths.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace libera_timecode {

namespace {

using json = nlohmann::json;

const char* fpsToString(FrameRate r) { return frameRateLabel(r); }

FrameRate stringToFps(const std::string& s, FrameRate fallback) {
    for (int i = 0; i < numFrameRates(); ++i) {
        if (s == frameRateLabel(frameRateAt(i))) return frameRateAt(i);
    }
    return fallback;
}

template <typename T>
T getOr(const json& j, const char* key, const T& fallback) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    try { return it->get<T>(); } catch (...) { return fallback; }
}

void readSmpte(const json& j, SmpteSettings& s) {
    s.enabled      = getOr(j, "enabled",      s.enabled);
    s.fps          = stringToFps(getOr<std::string>(j, "fps", fpsToString(s.fps)), s.fps);
    s.audioDevice  = getOr(j, "audio_device", s.audioDevice);
    s.sampleRate   = getOr(j, "sample_rate",  s.sampleRate);
    s.level        = getOr(j, "level",        s.level);
    s.channelMode  = getOr(j, "channel_mode", s.channelMode);
}
json writeSmpte(const SmpteSettings& s) {
    return json{
        {"enabled",      s.enabled},
        {"fps",          fpsToString(s.fps)},
        {"audio_device", s.audioDevice},
        {"sample_rate",  s.sampleRate},
        {"level",        s.level},
        {"channel_mode", s.channelMode},
    };
}

void readMidi(const json& j, MidiSettings& s) {
    s.enabled            = getOr(j, "enabled",             s.enabled);
    s.fps                = stringToFps(getOr<std::string>(j, "fps", fpsToString(s.fps)), s.fps);
    s.portName           = getOr(j, "port_name",           s.portName);
    s.createVirtualPort  = getOr(j, "create_virtual_port", s.createVirtualPort);
}
json writeMidi(const MidiSettings& s) {
    return json{
        {"enabled",             s.enabled},
        {"fps",                 fpsToString(s.fps)},
        {"port_name",           s.portName},
        {"create_virtual_port", s.createVirtualPort},
    };
}

void readArtnet(const json& j, ArtnetSettings& s) {
    s.enabled    = getOr(j, "enabled",    s.enabled);
    s.fps        = stringToFps(getOr<std::string>(j, "fps", fpsToString(s.fps)), s.fps);
    s.targetIp   = getOr(j, "target_ip",  s.targetIp);
    s.port       = getOr(j, "port",       s.port);
}
json writeArtnet(const ArtnetSettings& s) {
    return json{
        {"enabled",   s.enabled},
        {"fps",       fpsToString(s.fps)},
        {"target_ip", s.targetIp},
        {"port",      s.port},
    };
}

void readSntc(const json& j, SntcSettings& s) {
    s.enabled    = getOr(j, "enabled",    s.enabled);
    s.fps        = stringToFps(getOr<std::string>(j, "fps", fpsToString(s.fps)), s.fps);
    s.targetIp   = getOr(j, "target_ip",  s.targetIp);
    s.port       = getOr(j, "port",       s.port);
    s.senderId   = getOr(j, "sender_id",  s.senderId);
}
json writeSntc(const SntcSettings& s) {
    return json{
        {"enabled",   s.enabled},
        {"fps",       fpsToString(s.fps)},
        {"target_ip", s.targetIp},
        {"port",      s.port},
        {"sender_id", s.senderId},
    };
}

} // namespace

void loadSettings(AppSettings& out) {
    std::ifstream in(settingsFilePath());
    if (!in) return;
    json j;
    try { in >> j; } catch (...) { return; }

    if (auto it = j.find("window"); it != j.end()) {
        out.window.x = getOr(*it, "x", out.window.x);
        out.window.y = getOr(*it, "y", out.window.y);
        out.window.w = getOr(*it, "w", out.window.w);
        out.window.h = getOr(*it, "h", out.window.h);
    }
    out.tapJumpSeconds = getOr(j, "tap_jump_seconds", out.tapJumpSeconds);
    out.playbackRate   = getOr(j, "playback_rate",    out.playbackRate);
    out.showStatusPanel = getOr(j, "show_status_panel", out.showStatusPanel);
    if (auto it = j.find("smpte");  it != j.end()) readSmpte(*it,  out.smpte);
    if (auto it = j.find("midi");   it != j.end()) readMidi(*it,   out.midi);
    if (auto it = j.find("artnet"); it != j.end()) readArtnet(*it, out.artnet);
    if (auto it = j.find("sntc");   it != j.end()) readSntc(*it,   out.sntc);
}

void saveSettings(const AppSettings& in) {
    json j;
    j["window"] = {
        {"x", in.window.x},
        {"y", in.window.y},
        {"w", in.window.w},
        {"h", in.window.h},
    };
    j["tap_jump_seconds"] = in.tapJumpSeconds;
    j["playback_rate"]    = in.playbackRate;
    j["show_status_panel"] = in.showStatusPanel;
    j["smpte"]  = writeSmpte(in.smpte);
    j["midi"]   = writeMidi(in.midi);
    j["artnet"] = writeArtnet(in.artnet);
    j["sntc"]   = writeSntc(in.sntc);

    std::ofstream out(settingsFilePath(), std::ios::trunc);
    if (out) out << j.dump(2);
}

} // namespace libera_timecode
