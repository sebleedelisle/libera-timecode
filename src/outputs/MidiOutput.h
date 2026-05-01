#pragma once

#include "../Settings.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
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
    using MessageProbe = std::function<void(const unsigned char* data,
                                            std::size_t size,
                                            std::chrono::steady_clock::time_point sentAt)>;

    MidiOutput(TimecodeEngine& engine);
    ~MidiOutput();

    void applyConfig(const MidiSettings& config);
    void setMessageProbe(MessageProbe probe);
    void stop();
    bool running() const { return running_.load(); }
    std::string status() const;

    // Returns the names of available MIDI output ports for the settings UI.
    static std::vector<std::string> availablePorts();

private:
    void threadMain();
    void notifyMessageProbe(const unsigned char* data,
                            std::size_t size,
                            std::chrono::steady_clock::time_point sentAt);

    TimecodeEngine& engine_;
    std::unique_ptr<RtMidiOut> midi_;

    mutable std::mutex mutex_;
    MidiSettings config_;
    std::string lastError_;
    std::string openPortLabel_;

    mutable std::mutex probeMutex_;
    MessageProbe messageProbe_;
    std::atomic<bool> messageProbeEnabled_{false};

    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> configDirty_{false};
    std::thread thread_;
};

} // namespace libera_timecode
