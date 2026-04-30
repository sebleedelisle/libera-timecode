#include "ArtnetOutput.h"

#include "../TimecodeEngine.h"

#include <chrono>
#include <cstring>

namespace libera_timecode {

namespace {

uint8_t artnetTypeForFps(FrameRate r) {
    // 0 Film(24), 1 EBU(25), 2 DF(29.97), 3 SMPTE(30 NDF)
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
    return "Sending to " + config_.targetIp + ":" + std::to_string(config_.port);
}

void ArtnetOutput::threadMain() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
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

        const double fps = frameRateInfo(cfg.fps).nominalFps;
        const auto fpsInterval = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / std::max(1.0, fps)));
        const auto pollInterval = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / std::max(1.0, fps * 4.0)));

        // Send when the frame number advances, or once per frame interval to
        // keep the receiver alive while paused.
        if (sender_.ok()) {
            const TimecodeFields tc = secondsToFields(engine_.currentSeconds(), cfg.fps);
            const int fn = ((tc.hours * 60 + tc.minutes) * 60 + tc.seconds)
                           * tc.integerFps + tc.frames;
            const auto now = clock::now();
            const bool frameChanged = (fn != lastSentFrame);
            const bool keepalive = (now - lastSendTime >= fpsInterval);
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

        next += pollInterval;
        const auto now = clock::now();
        if (next < now) next = now + pollInterval;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, next, [this] {
            return shouldStop_.load() || configDirty_.load();
        });
    }
}

} // namespace libera_timecode
