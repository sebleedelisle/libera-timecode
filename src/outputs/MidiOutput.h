#pragma once

#include "../Settings.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RtMidiOut;

namespace libera_timecode {

class TimecodeEngine;

class MidiOutput {
public:
    MidiOutput(TimecodeEngine& engine);
    ~MidiOutput();

    void applyConfig(const MidiSettings& config);
    void stop();
    bool running() const { return running_.load(); }
    std::string status() const;

    // Returns the names of available MIDI output ports for the settings UI.
    static std::vector<std::string> availablePorts();

private:
    void threadMain();

    TimecodeEngine& engine_;
    std::unique_ptr<RtMidiOut> midi_;

    mutable std::mutex mutex_;
    MidiSettings config_;
    std::string lastError_;
    std::string openPortLabel_;

    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> configDirty_{false};
    std::thread thread_;
};

} // namespace libera_timecode
