#pragma once

#include "../Settings.h"
#include "UdpSender.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace libera_timecode {

class TimecodeEngine;

class ArtnetOutput {
public:
    ArtnetOutput(TimecodeEngine& engine);
    ~ArtnetOutput();

    void applyConfig(const ArtnetSettings& config);
    void stop();
    bool running() const { return running_.load(); }
    std::string status() const;

private:
    void threadMain();

    TimecodeEngine& engine_;
    UdpSender sender_;

    mutable std::mutex mutex_;
    ArtnetSettings config_;
    std::string lastError_;
    uint64_t packetsSent_{0};

    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> configDirty_{false};
    std::thread thread_;
};

} // namespace libera_timecode
