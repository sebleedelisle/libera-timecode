#include "SntcOutput.h"

#include "OutputTiming.h"

#include "../ThreadQos.h"
#include "../TimecodeEngine.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace libera_timecode {

namespace {

// 16-byte SNTC wire format observed in captures (see Liberation's
// TimeSourceSNTC parseCapturedBinarySntc):
//   [0..3]  "SNTC"
//   [4]     frame rate (raw byte)
//   [5..8]  HH MM SS FF
//   [9]     mode/rate code (we set to 0)
//   [10..13] sender id (4 ASCII chars)
//   [14]    trailer (0x02)
//   [15]    checksum (algorithm unknown — XOR of [0..14] is our best guess)
void buildSntcPacket(uint8_t* out, const TimecodeFields& tc, int frameRateByte,
                     const std::string& senderId) {
    std::memcpy(out, "SNTC", 4);
    out[4] = static_cast<uint8_t>(frameRateByte);
    out[5] = static_cast<uint8_t>(tc.hours);
    out[6] = static_cast<uint8_t>(tc.minutes);
    out[7] = static_cast<uint8_t>(tc.seconds);
    out[8] = static_cast<uint8_t>(tc.frames);
    out[9] = 0;
    char id[4] = {' ', ' ', ' ', ' '};
    for (size_t i = 0; i < 4 && i < senderId.size(); ++i) {
        const char c = senderId[i];
        id[i] = (c >= 32 && c < 127) ? c : ' ';
    }
    out[10] = static_cast<uint8_t>(id[0]);
    out[11] = static_cast<uint8_t>(id[1]);
    out[12] = static_cast<uint8_t>(id[2]);
    out[13] = static_cast<uint8_t>(id[3]);
    out[14] = 0x02;
    uint8_t cs = 0;
    for (int i = 0; i < 15; ++i) cs ^= out[i];
    out[15] = cs;
}

} // namespace

SntcOutput::SntcOutput(TimecodeEngine& engine) : engine_(engine) {}

SntcOutput::~SntcOutput() { stop(); }

void SntcOutput::applyConfig(const SntcSettings& config) {
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

void SntcOutput::stop() {
    shouldStop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    sender_.close();
}

std::string SntcOutput::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) return "Disabled";
    if (!lastError_.empty()) return "Error: " + lastError_;
    if (!running_.load())    return "Idle";
    return "-> " + config_.targetIp + ":" + std::to_string(config_.port);
}

void SntcOutput::threadMain() {
    setHighPriorityForOutputThread();
    using clock = std::chrono::steady_clock;
    auto lastSendTime = clock::now() - std::chrono::seconds(1);
    int lastSentFrame = -2;

    while (!shouldStop_.load()) {
        SntcSettings cfg;
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
                const int rateByte = std::clamp(frameRateInfo(cfg.fps).integerFps, 1, 240);
                uint8_t pkt[16];
                buildSntcPacket(pkt, tc, rateByte, cfg.senderId);
                if (!sender_.send(pkt, sizeof(pkt))) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lastError_ = "sendto failed";
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lastError_.clear();
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
