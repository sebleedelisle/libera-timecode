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

class SntcOutput {
public:
    SntcOutput(TimecodeEngine& engine);
    ~SntcOutput();

    void applyConfig(const SntcSettings& config);
    void stop();
    bool running() const { return running_.load(); }
    std::string status() const;

private:
    void threadMain();

    TimecodeEngine& engine_;
    UdpSender sender_;

    mutable std::mutex mutex_;
    SntcSettings config_;
    std::string lastError_;

    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> configDirty_{false};
    std::thread thread_;
};

} // namespace libera_timecode
