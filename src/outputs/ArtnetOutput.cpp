#include "ArtnetOutput.h"

#include "OutputTiming.h"

#include "../ThreadQos.h"
#include "../TimecodeEngine.h"

#include <chrono>
#include <cstring>

namespace libera_timecode {

namespace {

uint8_t artnetTypeForFps(FrameRate r) {
    // Art-Net spec only defines four codes:
    //   0 Film (24fps)   1 EBU (25fps)   2 DF (29.97 DF)   3 SMPTE (30 NDF)
    // 23.976 maps to Film; 29.97 NDF and 30 DF have no exact code, so we
    // pick the closest match (NDF flavors → SMPTE 30, DF flavors → DF 29.97).
    switch (r) {
    case FrameRate::fps_23_976:
    case FrameRate::fps_24:        return 0;
    case FrameRate::fps_25:        return 1;
    case FrameRate::fps_29_97_DF:
    case FrameRate::fps_30_DF:     return 2;
    case FrameRate::fps_29_97_NDF:
    case FrameRate::fps_30_NDF:    return 3;
    }
    return 3;
}

} // namespace

ArtnetOutput::ArtnetOutput(TimecodeEngine& engine) : engine_(engine) {}

ArtnetOutput::~ArtnetOutput() { stop(); }

void ArtnetOutput::applyConfig(const ArtnetSettings& config) {
    // Only mark the socket dirty when fields that affect socket setup change.
    // Frame-rate / fps changes are picked up live on the next poll tick
    // without any teardown.
    bool socketChanged = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socketChanged = (config.targetIp != config_.targetIp)
                     || (config.port     != config_.port);
        config_ = config;
        if (socketChanged) configDirty_ = true;
    }
    if (socketChanged) cv_.notify_all();

    if (config.enabled && !running_.load()) {
        shouldStop_ = false;
        running_ = true;
        thread_ = std::thread([this] { threadMain(); });
    } else if (!config.enabled && running_.load()) {
        stop();
    }
}

void ArtnetOutput::stop() {
    shouldStop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    sender_.close();
}

std::string ArtnetOutput::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) return "Disabled";
    if (!lastError_.empty())  return "Error: " + lastError_;
    if (!running_.load())     return "Idle";
    return "-> " + config_.targetIp + ":" + std::to_string(config_.port);
}

void ArtnetOutput::threadMain() {
    setHighPriorityForOutputThread();
    using clock = std::chrono::steady_clock;
    auto lastSendTime = clock::now() - std::chrono::seconds(1);
    int lastSentFrame = -2;

    while (!shouldStop_.load()) {
        ArtnetSettings cfg;
        bool dirty;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
            dirty = configDirty_.exchange(false);
        }

        if (dirty || !sender_.ok()) {
            std::string err;
            if (!sender_.configure(cfg.targetIp, cfg.port, err)) {
                std::lock_guard<std::mutex> lock(mutex_);
                lastError_ = err;
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                lastError_.clear();
            }
            lastSentFrame = -2;
        }

        const auto snap = engine_.snapshot();
        const auto keepaliveInterval = output_timing::frameKeepaliveInterval(snap, cfg.fps);

        if (sender_.ok()) {
            const TimecodeFields tc = secondsToFields(snap.playheadSeconds, cfg.fps);
            const int fn = output_timing::labelFrameNumber(tc);
            const auto now = clock::now();
            const bool frameChanged = (fn != lastSentFrame);
            const bool keepalive = (now - lastSendTime > keepaliveInterval);
            if (frameChanged || keepalive) {
                uint8_t pkt[19];
                std::memcpy(pkt, "Art-Net\0", 8);
                pkt[8]  = 0x00; pkt[9]  = 0x97;
                pkt[10] = 0;    pkt[11] = 14;
                pkt[12] = 0;    pkt[13] = 0;
                pkt[14] = static_cast<uint8_t>(tc.frames);
                pkt[15] = static_cast<uint8_t>(tc.seconds);
                pkt[16] = static_cast<uint8_t>(tc.minutes);
                pkt[17] = static_cast<uint8_t>(tc.hours);
                pkt[18] = artnetTypeForFps(cfg.fps);
                if (!sender_.send(pkt, sizeof(pkt))) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lastError_ = "sendto failed";
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lastError_.clear();
                    ++packetsSent_;
                }
                lastSentFrame = fn;
                lastSendTime = now;
            }
        }

        const auto next = clock::now() + output_timing::nextFrameBoundaryDelay(snap, cfg.fps);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, next, [this] {
            return shouldStop_.load() || configDirty_.load();
        });
    }
}

} // namespace libera_timecode
