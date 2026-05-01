#include "MidiOutput.h"

#include "OutputTiming.h"
#include "TimecodeProtocol.h"

#include "../ThreadQos.h"
#include "../TimecodeEngine.h"

#include <RtMidi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace libera_timecode {

namespace {

std::vector<unsigned char> fullFrameMessage(const TimecodeFields& tc, FrameRate fps) {
    const auto bytes = timecode_protocol::buildMidiFullFrame(tc, fps);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> quarterFrameMessage(int piece,
                                               const TimecodeFields& tc,
                                               FrameRate fps) {
    const auto bytes = timecode_protocol::buildMidiQuarterFrame(piece, tc, fps);
    return {bytes.begin(), bytes.end()};
}

void sendMidiMessage(RtMidiOut& out, const std::vector<unsigned char>& msg) {
    out.sendMessage(&msg);
}

} // namespace

MidiOutput::MidiOutput(TimecodeEngine& engine) : engine_(engine) {}

MidiOutput::~MidiOutput() { stop(); }

void MidiOutput::setMessageProbe(MessageProbe probe) {
    std::lock_guard<std::mutex> lock(probeMutex_);
    messageProbe_ = std::move(probe);
    messageProbeEnabled_.store(static_cast<bool>(messageProbe_), std::memory_order_release);
}

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
    // Reopening a MIDI port is expensive; only do it when the port itself
    // changed. fps / live fields are picked up at the next poll tick.
    bool portChanged = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        portChanged = (config.portName          != config_.portName)
                   || (config.createVirtualPort != config_.createVirtualPort);
        config_ = config;
        if (portChanged) configDirty_ = true;
    }
    if (portChanged) cv_.notify_all();

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

void MidiOutput::notifyMessageProbe(const unsigned char* data,
                                    std::size_t size,
                                    std::chrono::steady_clock::time_point sentAt) {
    if (!messageProbeEnabled_.load(std::memory_order_acquire)) return;

    MessageProbe probe;
    {
        std::lock_guard<std::mutex> lock(probeMutex_);
        probe = messageProbe_;
    }
    if (probe) probe(data, size, sentAt);
}

void MidiOutput::threadMain() {
    setHighPriorityForOutputThread();
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
            next = clock::now();
        }

        const auto snap = engine_.snapshot();
        const bool stateChanged = (snap.clockMode != lastClockMode) || (snap.playing != lastPlaying);

        // MTC quarter-frame messages span 2 frames; all 8 pieces must encode
        // the same TC value (sampled at piece 0). Re-sample only on piece 0.
        if (piece == 0 || stateChanged || !haveLocked) {
            const TimecodeFields nowTc = secondsToFields(snap.playheadSeconds, cfg.fps);
            const int nowFn = output_timing::labelFrameNumber(nowTc);

            // Expected step between locked captures during continuous playback
            // is two frames at varispeed-adjusted quarter-frame cadence.
            const int expectedStep = (snap.rate < -output_timing::kMinActiveRate) ? -2 : 2;
            const bool bigJump = haveLocked
                              && std::abs((nowFn - lockedFrameNumber) - expectedStep) > 2;

            if (stateChanged || bigJump || !haveLocked) {
                try {
                    const auto msg = fullFrameMessage(nowTc, cfg.fps);
                    sendMidiMessage(*midi_, msg);
                    notifyMessageProbe(msg.data(), msg.size(), clock::now());
                }
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
            try {
                const auto msg = quarterFrameMessage(piece, lockedTc, cfg.fps);
                sendMidiMessage(*midi_, msg);
                notifyMessageProbe(msg.data(), msg.size(), clock::now());
            }
            catch (RtMidiError& e) {
                std::lock_guard<std::mutex> lock(mutex_);
                lastError_ = e.what();
            }
            piece = (piece + 1) & 0x07;
        }

        const auto interval = output_timing::quarterFrameInterval(snap, cfg.fps);
        next += interval;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, next, [this] {
            return shouldStop_.load() || configDirty_.load();
        });
        const auto now = clock::now();
        if (now > next + interval) next = now;
    }

    midi_.reset();
}

} // namespace libera_timecode
