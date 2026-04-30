#include "MidiOutput.h"

#include "../TimecodeEngine.h"

#include <RtMidi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace libera_timecode {

namespace {

uint8_t mtcRateCode(FrameRate r) {
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

void sendFullFrame(RtMidiOut& out, const TimecodeFields& tc, uint8_t rateCode) {
    // F0 7F 7F 01 01 hh mm ss ff F7
    std::vector<unsigned char> msg{
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        static_cast<unsigned char>(((rateCode & 0x03) << 5) | (tc.hours & 0x1F)),
        static_cast<unsigned char>(tc.minutes & 0x3F),
        static_cast<unsigned char>(tc.seconds & 0x3F),
        static_cast<unsigned char>(tc.frames  & 0x1F),
        0xF7,
    };
    out.sendMessage(&msg);
}

void sendQuarterFrame(RtMidiOut& out, int piece, const TimecodeFields& tc, uint8_t rateCode) {
    // piece 0..7 chooses which nibble. F1 0xMV where M = piece, V = nibble value.
    uint8_t value = 0;
    switch (piece) {
    case 0: value = tc.frames  & 0x0F; break;
    case 1: value = (tc.frames  >> 4) & 0x01; break;
    case 2: value = tc.seconds & 0x0F; break;
    case 3: value = (tc.seconds >> 4) & 0x03; break;
    case 4: value = tc.minutes & 0x0F; break;
    case 5: value = (tc.minutes >> 4) & 0x03; break;
    case 6: value = tc.hours   & 0x0F; break;
    case 7: value = ((tc.hours  >> 4) & 0x01) | ((rateCode & 0x03) << 1); break;
    }
    std::vector<unsigned char> msg{
        0xF1,
        static_cast<unsigned char>(((piece & 0x07) << 4) | (value & 0x0F)),
    };
    out.sendMessage(&msg);
}

} // namespace

MidiOutput::MidiOutput(TimecodeEngine& engine) : engine_(engine) {}

MidiOutput::~MidiOutput() { stop(); }

std::vector<std::string> MidiOutput::availablePorts() {
    std::vector<std::string> ports;
    try {
        RtMidiOut probe;
        const unsigned int n = probe.getPortCount();
        ports.reserve(n);
        for (unsigned int i = 0; i < n; ++i) {
            ports.push_back(probe.getPortName(i));
        }
    } catch (...) {
        // ignore
    }
    return ports;
}

void MidiOutput::applyConfig(const MidiSettings& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        configDirty_ = true;
    }
    cv_.notify_all();

    if (config.enabled && !running_.load()) {
        shouldStop_ = false;
        running_ = true;
        thread_ = std::thread([this] { threadMain(); });
    } else if (!config.enabled && running_.load()) {
        stop();
    }
}

void MidiOutput::stop() {
    shouldStop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

std::string MidiOutput::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) return "Disabled";
    if (!lastError_.empty()) return "Error: " + lastError_;
    if (!running_.load())    return "Idle";
    return "Sending MTC -> " + openPortLabel_;
}

void MidiOutput::threadMain() {
    using clock = std::chrono::steady_clock;

    auto openPort = [this](const MidiSettings& cfg) {
        midi_.reset();
        try {
            midi_ = std::make_unique<RtMidiOut>();
        } catch (RtMidiError& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = e.what();
            return false;
        }

        try {
            if (!cfg.portName.empty()) {
                const unsigned int n = midi_->getPortCount();
                bool found = false;
                for (unsigned int i = 0; i < n; ++i) {
                    if (midi_->getPortName(i) == cfg.portName) {
                        midi_->openPort(i, "Libera Timecode");
                        std::lock_guard<std::mutex> lock(mutex_);
                        openPortLabel_ = cfg.portName;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lastError_ = "MIDI port not found: " + cfg.portName;
                    midi_.reset();
                    return false;
                }
            } else if (cfg.createVirtualPort) {
                midi_->openVirtualPort("Libera Timecode");
                std::lock_guard<std::mutex> lock(mutex_);
                openPortLabel_ = "Libera Timecode (virtual)";
            } else if (midi_->getPortCount() > 0) {
                midi_->openPort(0, "Libera Timecode");
                std::lock_guard<std::mutex> lock(mutex_);
                openPortLabel_ = midi_->getPortName(0);
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                lastError_ = "No MIDI output ports";
                midi_.reset();
                return false;
            }
        } catch (RtMidiError& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = e.what();
            midi_.reset();
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
        return true;
    };

    int piece = 0;
    TimecodeFields lockedTc{}; // captured at piece 0, reused for pieces 0..7
    int lockedFrameNumber = -1;
    bool haveLocked = false;
    bool lastClockMode = false;
    bool lastPlaying = false;

    auto next = clock::now();

    while (!shouldStop_.load()) {
        MidiSettings cfg;
        bool dirty;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
            dirty = configDirty_.exchange(false);
        }

        if (dirty || !midi_) {
            if (!openPort(cfg)) {
                // Backoff briefly then retry, unless shutting down.
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                    return shouldStop_.load() || configDirty_.load();
                });
                continue;
            }
            piece = 0;
            haveLocked = false;
        }

        const auto snap = engine_.snapshot();
        const bool stateChanged = (snap.clockMode != lastClockMode) || (snap.playing != lastPlaying);

        // MTC quarter-frame messages span 2 frames; all 8 pieces must encode
        // the same TC value (sampled at piece 0). Re-sample only on piece 0.
        if (piece == 0 || stateChanged || !haveLocked) {
            const TimecodeFields nowTc = secondsToFields(snap.playheadSeconds, cfg.fps);
            const int nowFn = ((nowTc.hours * 60 + nowTc.minutes) * 60 + nowTc.seconds)
                              * nowTc.integerFps + nowTc.frames;

            // Expected step between locked captures during continuous playback
            // is +2 frames. Anything beyond that is a jump.
            const bool bigJump = haveLocked
                              && std::abs(nowFn - lockedFrameNumber - 2) > 2;

            if (stateChanged || bigJump || !haveLocked) {
                try { sendFullFrame(*midi_, nowTc, mtcRateCode(cfg.fps)); }
                catch (RtMidiError& e) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lastError_ = e.what();
                }
                piece = 0;
            }
            lockedTc = nowTc;
            lockedFrameNumber = nowFn;
            haveLocked = true;
        }
        lastClockMode = snap.clockMode;
        lastPlaying = snap.playing;

        // Only stream quarter-frames when playing or in clock mode.
        if (snap.playing || snap.clockMode) {
            try { sendQuarterFrame(*midi_, piece, lockedTc, mtcRateCode(cfg.fps)); }
            catch (RtMidiError& e) {
                std::lock_guard<std::mutex> lock(mutex_);
                lastError_ = e.what();
            }
            piece = (piece + 1) & 0x07;
        }

        const double fps = frameRateInfo(cfg.fps).nominalFps;
        const auto interval = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / std::max(1.0, fps * 4.0)));
        next += interval;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, next, [this] {
            return shouldStop_.load() || configDirty_.load();
        });
        if (clock::now() > next + std::chrono::seconds(1)) next = clock::now();
    }

    midi_.reset();
}

} // namespace libera_timecode
