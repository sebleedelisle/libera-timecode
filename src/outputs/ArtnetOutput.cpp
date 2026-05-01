#include "ArtnetOutput.h"

#include "OutputTiming.h"
#include "TimecodeProtocol.h"

#include "../ThreadQos.h"
#include "../TimecodeEngine.h"

#include <chrono>
#include <cstring>

namespace libera_timecode {

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
                const auto pkt = timecode_protocol::buildArtnetTimecodePacket(tc, cfg.fps);
                if (!sender_.send(pkt.data(), pkt.size())) {
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
