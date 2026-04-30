#include "LiberaApp.h"
#include "Paths.hpp"
#include "Settings.h"
#include "Timecode.h"
#include "TimecodeEngine.h"
#include "fonts/IconsForkAwesome.h"
#include "outputs/ArtnetOutput.h"
#include "outputs/MidiOutput.h"
#include "outputs/SmpteOutput.h"
#include "outputs/SntcOutput.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace libera_timecode;

// Mono-spaced timecode readout with a black panel background. Digits are
// laid out on a fixed-pitch grid; ':' uses a narrower cell.
void drawReadout(const LiberaApp& app, const TransportSnapshot& snap, FrameRate displayFps,
                 int displayFrameOverride) {
    ImFont* font = app.fontReadout ? app.fontReadout : ImGui::GetFont();

    TimecodeFields tc = secondsToFields(snap.playheadSeconds, displayFps);
    if (displayFrameOverride >= 0) tc.frames = displayFrameOverride;

    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d:%02d",
                  tc.hours, tc.minutes, tc.seconds, tc.frames);

    const float fontSize = font->LegacySize * ImGui::GetStyle().FontScaleMain;

    // Measure widest digit and ':' to build a fixed grid.
    float digitWidth = 0.0f;
    for (char c = '0'; c <= '9'; ++c) {
        const char s[2] = { c, 0 };
        const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s);
        digitWidth = std::max(digitWidth, sz.x);
    }
    const ImVec2 colonSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ":");
    const float colonWidth = colonSize.x;

    float totalWidth = 0.0f;
    for (const char* p = text; *p; ++p) {
        totalWidth += (*p == ':') ? colonWidth : digitWidth;
    }
    const float lineHeight = fontSize * 1.15f;

    const float panelHeight = lineHeight + 24.0f;
    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 panelMin = ImGui::GetCursorScreenPos();
    const ImVec2 panelMax(panelMin.x + panelWidth, panelMin.y + panelHeight);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 255), 6.0f);
    dl->AddRect(panelMin, panelMax, IM_COL32(40, 50, 60, 255), 6.0f, 0, 1.0f);

    const float startX = panelMin.x + (panelWidth - totalWidth) * 0.5f;
    const float baselineY = panelMin.y + (panelHeight - fontSize) * 0.5f;

    const ImU32 colorPlaying = IM_COL32(120, 230, 150, 255);
    const ImU32 colorPaused  = IM_COL32(190, 200, 210, 255);
    const ImU32 colorClock   = IM_COL32(255, 200, 90,  255);
    ImU32 color = colorPaused;
    if (snap.clockMode) color = colorClock;
    else if (snap.playing) color = colorPlaying;
    else if (std::abs(snap.rate) > 0.01) color = IM_COL32(110, 180, 255, 255);

    float x = startX;
    for (const char* p = text; *p; ++p) {
        const float advance = (*p == ':') ? colonWidth : digitWidth;
        const char s[2] = { *p, 0 };
        const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s);
        const float cellOffset = (advance - sz.x) * 0.5f;
        dl->AddText(font, fontSize, ImVec2(x + cellOffset, baselineY), color, s);
        x += advance;
    }

    ImGui::Dummy(ImVec2(panelWidth, panelHeight));
}

// ImGui doesn't natively support arbitrary scaled icon buttons; we draw our
// own with a circle background and a centered icon glyph.
bool circleButton(const char* idStr, const char* iconText,
                  bool primary, bool active, float size,
                  ImFont* iconFont = nullptr) {
    ImGui::PushID(idStr);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##btn", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 center(cur.x + size * 0.5f, cur.y + size * 0.5f);
    const float radius = size * 0.5f - 1.0f;

    ImU32 fill;
    if (primary) {
        if (held)         fill = IM_COL32(60, 140, 220, 255);
        else if (hovered) fill = IM_COL32(90, 180, 250, 255);
        else if (active)  fill = IM_COL32(70, 150, 230, 255);
        else              fill = IM_COL32(50, 110, 180, 255);
    } else if (active) {
        if (hovered)      fill = IM_COL32(220, 160, 60, 255);
        else              fill = IM_COL32(200, 140, 50, 255);
    } else {
        if (held)         fill = IM_COL32(60, 70, 90, 255);
        else if (hovered) fill = IM_COL32(80, 90, 110, 255);
        else              fill = IM_COL32(45, 52, 65, 255);
    }
    dl->AddCircleFilled(center, radius, fill, 32);
    dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 60), 32, 1.0f);

    if (iconFont) ImGui::PushFont(iconFont);
    const ImVec2 sz = ImGui::CalcTextSize(iconText);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f),
                IM_COL32(245, 250, 255, 255), iconText);
    if (iconFont) ImGui::PopFont();

    ImGui::PopID();
    return clicked;
}

bool modeToggleButton(const char* label, bool& enabled) {
    const ImVec2 size(120.0f, 38.0f);
    if (enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.32f, 0.78f, 0.50f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.85f, 0.56f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.28f, 0.72f, 0.46f, 1.00f));
    }
    const bool clicked = ImGui::Button(label, size);
    if (enabled) ImGui::PopStyleColor(3);
    if (clicked) enabled = !enabled;
    return clicked;
}

void fpsCombo(const char* idStr, FrameRate& fps) {
    int idx = indexOfFrameRate(fps);
    if (ImGui::BeginCombo(idStr, frameRateLabel(fps))) {
        for (int i = 0; i < numFrameRates(); ++i) {
            const bool selected = (i == idx);
            if (ImGui::Selectable(frameRateLabel(frameRateAt(i)), selected)) {
                fps = frameRateAt(i);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void inputText(const char* label, std::string& s, size_t maxLen = 256) {
    std::vector<char> buf(maxLen, 0);
    std::strncpy(buf.data(), s.c_str(), maxLen - 1);
    if (ImGui::InputText(label, buf.data(), maxLen)) {
        s = buf.data();
    }
}

struct OpenSettings {
    bool smpte{false};
    bool midi{false};
    bool artnet{false};
    bool sntc{false};
};

void drawSmpteSettings(SmpteSettings& s, bool& dirty) {
    ImGui::Text("Audio output for LTC.");
    ImGui::Separator();

    if (ImGui::Checkbox("Enabled##smpte", &s.enabled)) dirty = true;
    ImGui::SetNextItemWidth(160.0f);
    {
        FrameRate prev = s.fps;
        fpsCombo("Frame rate##smpte", s.fps);
        if (s.fps != prev) dirty = true;
    }

    ImGui::Spacing();
    ImGui::Text("Audio device");
    auto devices = SmpteOutput::availableAudioDevices();
    if (ImGui::BeginCombo("##smpte-device", s.audioDevice.empty() ? "(default)" : s.audioDevice.c_str())) {
        if (ImGui::Selectable("(default)", s.audioDevice.empty())) { s.audioDevice.clear(); dirty = true; }
        for (const auto& name : devices) {
            const bool selected = (s.audioDevice == name);
            if (ImGui::Selectable(name.c_str(), selected)) { s.audioDevice = name; dirty = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Sample rate", &s.sampleRate, 0, 0)) dirty = true;
    if (s.sampleRate < 8000)   s.sampleRate = 8000;
    if (s.sampleRate > 192000) s.sampleRate = 192000;

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("Output level", &s.level, 0.0f, 1.0f, "%.2f")) dirty = true;

    const char* channelLabels[] = {"Left only", "Right only", "Both channels"};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Channels", &s.channelMode, channelLabels, 3)) dirty = true;
}

void drawMidiSettings(MidiSettings& s, bool& dirty) {
    ImGui::Text("Sends MIDI Time Code (MTC) quarter-frame messages.");
    ImGui::Separator();
    if (ImGui::Checkbox("Enabled##midi", &s.enabled)) dirty = true;
    ImGui::SetNextItemWidth(160.0f);
    {
        FrameRate prev = s.fps;
        fpsCombo("Frame rate##midi", s.fps);
        if (s.fps != prev) dirty = true;
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Create virtual port (recommended)", &s.createVirtualPort)) dirty = true;

    ImGui::Text("Or pick an existing port:");
    auto ports = MidiOutput::availablePorts();
    if (ImGui::BeginCombo("##midi-port", s.portName.empty() ? "(virtual)" : s.portName.c_str())) {
        if (ImGui::Selectable("(virtual)", s.portName.empty())) { s.portName.clear(); dirty = true; }
        for (const auto& name : ports) {
            const bool selected = (s.portName == name);
            if (ImGui::Selectable(name.c_str(), selected)) { s.portName = name; dirty = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void drawArtnetSettings(ArtnetSettings& s, bool& dirty) {
    ImGui::Text("Sends ArtTimeCode (opcode 0x9700) over UDP.");
    ImGui::Separator();
    if (ImGui::Checkbox("Enabled##artnet", &s.enabled)) dirty = true;
    ImGui::SetNextItemWidth(160.0f);
    {
        FrameRate prev = s.fps;
        fpsCombo("Frame rate##artnet", s.fps);
        if (s.fps != prev) dirty = true;
    }

    ImGui::SetNextItemWidth(220.0f);
    {
        std::string before = s.targetIp;
        inputText("Target IP", s.targetIp);
        if (s.targetIp != before) dirty = true;
    }
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Port", &s.port, 0, 0)) dirty = true;
    if (s.port < 1)     s.port = 1;
    if (s.port > 65535) s.port = 65535;
    ImGui::TextDisabled("Default 6454, broadcast 255.255.255.255 reaches all nodes.");
}

void drawSntcSettings(SntcSettings& s, bool& dirty) {
    ImGui::Text("Sends Depence-style SNTC packets over UDP.");
    ImGui::Separator();
    if (ImGui::Checkbox("Enabled##sntc", &s.enabled)) dirty = true;
    ImGui::SetNextItemWidth(160.0f);
    {
        FrameRate prev = s.fps;
        fpsCombo("Frame rate##sntc", s.fps);
        if (s.fps != prev) dirty = true;
    }

    ImGui::SetNextItemWidth(220.0f);
    {
        std::string before = s.targetIp;
        inputText("Target IP", s.targetIp);
        if (s.targetIp != before) dirty = true;
    }
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Port", &s.port, 0, 0)) dirty = true;
    if (s.port < 1)     s.port = 1;
    if (s.port > 65535) s.port = 65535;

    char idBuf[5] = {' ', ' ', ' ', ' ', 0};
    for (size_t i = 0; i < 4 && i < s.senderId.size(); ++i) idBuf[i] = s.senderId[i];
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputText("Sender ID (4 chars)", idBuf, sizeof(idBuf))) {
        s.senderId = std::string(idBuf, 4);
        dirty = true;
    }
}

} // namespace

int main() {
    AppSettings settings;
    loadSettings(settings);

    LiberaApp app;
    LiberaAppConfig appConfig;
    appConfig.title = "Libera Timecode";
    appConfig.width = settings.window.w;
    appConfig.height = settings.window.h;
    appConfig.windowX = settings.window.x;
    appConfig.windowY = settings.window.y;
    if (!app.init(appConfig)) return 1;

    TimecodeEngine engine;
    engine.setTapJumpSeconds(settings.tapJumpSeconds);

    SmpteOutput  smpte (engine);
    MidiOutput   midi  (engine);
    ArtnetOutput artnet(engine);
    SntcOutput   sntc  (engine);

    smpte.applyConfig(settings.smpte);
    midi.applyConfig(settings.midi);
    artnet.applyConfig(settings.artnet);
    sntc.applyConfig(settings.sntc);

    bool settingsDirty = false;

    OpenSettings open;

    while (app.beginFrame()) {
        engine.scrubTick();
        const TransportSnapshot snap = engine.snapshot();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Libera Timecode", nullptr,
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---------- Mode buttons row ----------
        struct ModeRow {
            const char* label;
            bool* enabled;
            bool* openSettings;
            std::string status;
            FrameRate* fps;
        };
        std::array<ModeRow, 4> rows{{
            {"SMPTE",   &settings.smpte.enabled,  &open.smpte,  smpte.status(),  &settings.smpte.fps},
            {"MIDI",    &settings.midi.enabled,   &open.midi,   midi.status(),   &settings.midi.fps},
            {"Art-Net", &settings.artnet.enabled, &open.artnet, artnet.status(), &settings.artnet.fps},
            {"Depence", &settings.sntc.enabled,   &open.sntc,   sntc.status(),   &settings.sntc.fps},
        }};

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 8.0f));
        for (size_t i = 0; i < rows.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool wasEnabled = *rows[i].enabled;
            if (modeToggleButton(rows[i].label, *rows[i].enabled)) {
                if (wasEnabled != *rows[i].enabled) settingsDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FK_COG, ImVec2(34.0f, 38.0f))) {
                *rows[i].openSettings = true;
            }
            ImGui::PopID();
            if (i + 1 < rows.size()) ImGui::SameLine();
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();

        // ---------- Big readout ----------
        // Frame display while paused: protocol fps if any output is enabled, else 30 NDF.
        // Clock mode: animate frames against a 30 fps wall-clock visual.
        FrameRate displayFps = FrameRate::fps_30_NDF;
        if (settings.smpte.enabled)       displayFps = settings.smpte.fps;
        else if (settings.midi.enabled)   displayFps = settings.midi.fps;
        else if (settings.artnet.enabled) displayFps = settings.artnet.fps;
        else if (settings.sntc.enabled)   displayFps = settings.sntc.fps;

        int displayFrameOverride = -1;
        if (snap.clockMode) {
            // Animate visual frames at the display fps from sub-second portion of wall time.
            const double s = snap.playheadSeconds;
            const double frac = s - std::floor(s);
            const auto& info = frameRateInfo(displayFps);
            displayFrameOverride = static_cast<int>(std::floor(frac * info.nominalFps)) % info.integerFps;
        }

        drawReadout(app, snap, displayFps, displayFrameOverride);

        // ---------- Status lines ----------
        ImGui::Spacing();
        ImGui::BeginChild("##status-lines", ImVec2(0, 100.0f), true);
        for (const auto& row : rows) {
            ImGui::TextColored(*row.enabled ? ImVec4(0.7f, 0.95f, 0.75f, 1.0f) : ImVec4(0.55f, 0.6f, 0.65f, 1.0f),
                               "%s", row.label);
            ImGui::SameLine(120.0f);
            ImGui::TextDisabled("%s @ %s", row.status.c_str(), frameRateLabel(*row.fps));
        }
        ImGui::EndChild();

        // ---------- Transport ----------
        ImGui::Spacing();
        const float btnSize = 56.0f;
        const float spacing = 12.0f;
        const float totalWidth = btnSize * 5.0f + spacing * 4.0f;
        const float startX = (ImGui::GetContentRegionAvail().x - totalWidth) * 0.5f;
        ImGui::Dummy(ImVec2(startX, 1));
        ImGui::SameLine();

        const bool clockMode = snap.clockMode;
        const bool playing = snap.playing;

        // Rewind
        ImGui::PushID("rew");
        const ImVec2 rewPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##rew", ImVec2(btnSize, btnSize));
        if (!clockMode) {
            if (ImGui::IsItemActivated())   engine.scrubPress(TimecodeEngine::ScrubDirection::Backward);
            if (ImGui::IsItemDeactivated()) engine.scrubRelease(TimecodeEngine::ScrubDirection::Backward);
        }
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(rewPos.x + btnSize * 0.5f, rewPos.y + btnSize * 0.5f);
            const ImU32 col = clockMode ? IM_COL32(60, 65, 75, 255)
                              : ImGui::IsItemActive() ? IM_COL32(60, 140, 220, 255)
                              : ImGui::IsItemHovered() ? IM_COL32(80, 90, 110, 255)
                              : IM_COL32(45, 52, 65, 255);
            dl->AddCircleFilled(c, btnSize * 0.5f - 1.0f, col, 32);
            ImGui::PushFont(app.fontMedium);
            const ImVec2 sz = ImGui::CalcTextSize(ICON_FK_FAST_BACKWARD);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                        IM_COL32(245, 250, 255, 255), ICON_FK_FAST_BACKWARD);
            ImGui::PopFont();
        }
        ImGui::PopID();
        ImGui::SameLine(0, spacing);

        // Fast forward
        ImGui::PushID("ff");
        const ImVec2 ffPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ff", ImVec2(btnSize, btnSize));
        if (!clockMode) {
            if (ImGui::IsItemActivated())   engine.scrubPress(TimecodeEngine::ScrubDirection::Forward);
            if (ImGui::IsItemDeactivated()) engine.scrubRelease(TimecodeEngine::ScrubDirection::Forward);
        }
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(ffPos.x + btnSize * 0.5f, ffPos.y + btnSize * 0.5f);
            const ImU32 col = clockMode ? IM_COL32(60, 65, 75, 255)
                              : ImGui::IsItemActive() ? IM_COL32(60, 140, 220, 255)
                              : ImGui::IsItemHovered() ? IM_COL32(80, 90, 110, 255)
                              : IM_COL32(45, 52, 65, 255);
            dl->AddCircleFilled(c, btnSize * 0.5f - 1.0f, col, 32);
            ImGui::PushFont(app.fontMedium);
            const ImVec2 sz = ImGui::CalcTextSize(ICON_FK_FAST_FORWARD);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                        IM_COL32(245, 250, 255, 255), ICON_FK_FAST_FORWARD);
            ImGui::PopFont();
        }
        ImGui::PopID();
        ImGui::SameLine(0, spacing);

        // Stop / back-to-zero
        const bool atZero = snap.playheadSeconds < 0.001;
        const char* stopIcon = (playing || atZero) ? ICON_FK_STOP : ICON_FK_STEP_BACKWARD;
        if (circleButton("stop", stopIcon, false, false, btnSize, app.fontMedium)) {
            if (!clockMode) engine.stopOrRewindToZero();
        }
        ImGui::SameLine(0, spacing);

        // Play / Pause
        const char* playIcon = playing ? ICON_FK_PAUSE : ICON_FK_PLAY;
        if (circleButton("play", playIcon, true, playing, btnSize, app.fontMedium)) {
            if (!clockMode) {
                if (playing) engine.pause();
                else         engine.play();
            }
        }
        ImGui::SameLine(0, spacing);

        // Clock mode toggle
        if (circleButton("clk", ICON_FK_CLOCK_O, false, clockMode, btnSize, app.fontMedium)) {
            engine.setClockMode(!clockMode);
        }

        // Help line
        ImGui::Spacing();
        ImGui::TextDisabled("Tap rewind/FF to jump %.0fs. Hold to scrub (ramps to 8x). Clock icon: send wall time.",
                            settings.tapJumpSeconds);

        ImGui::End();

        // ---------- Settings popups ----------
        auto runPopup = [&](const char* title, bool& openFlag, auto drawer) {
            if (!openFlag) return;
            const std::string idStr = std::string(title) + " Settings";
            ImGui::SetNextWindowSize(ImVec2(460.0f, 360.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(idStr.c_str(), &openFlag, ImGuiWindowFlags_NoCollapse)) {
                drawer();
            }
            ImGui::End();
        };

        runPopup("SMPTE",   open.smpte,   [&] { drawSmpteSettings (settings.smpte,  settingsDirty); });
        runPopup("MIDI",    open.midi,    [&] { drawMidiSettings  (settings.midi,   settingsDirty); });
        runPopup("Art-Net", open.artnet,  [&] { drawArtnetSettings(settings.artnet, settingsDirty); });
        runPopup("Depence", open.sntc,    [&] { drawSntcSettings  (settings.sntc,   settingsDirty); });

        // ---------- Persist & live-restart on changes ----------
        if (settingsDirty) {
            smpte.applyConfig(settings.smpte);
            midi.applyConfig(settings.midi);
            artnet.applyConfig(settings.artnet);
            sntc.applyConfig(settings.sntc);
            saveSettings(settings);
            settingsDirty = false;
        }

        // Persist window geometry when it changes.
        int wx, wy, ww, wh;
        app.getWindowGeometry(wx, wy, ww, wh);
        if (wx != settings.window.x || wy != settings.window.y ||
            ww != settings.window.w || wh != settings.window.h) {
            settings.window.x = wx; settings.window.y = wy;
            settings.window.w = ww; settings.window.h = wh;
            saveSettings(settings);
        }

        app.endFrame();
    }

    smpte.stop();
    midi.stop();
    artnet.stop();
    sntc.stop();
    app.shutdown();
    return 0;
}
