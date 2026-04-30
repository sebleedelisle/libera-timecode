#include "TimecodeEngine.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace libera_timecode {

namespace {

constexpr double kTapHoldThresholdSeconds = 0.18;
constexpr double kMaxScrubRate = 16.0;

double scrubRateForHold(double holdSeconds) {
    // Smoothly ramp from 1× to ~8× over ~3 seconds, then continue to cap.
    // 2^(t / 1s) — 1× at 0s, 2× at 1s, 4× at 2s, 8× at 3s.
    const double exp = std::min(holdSeconds, 4.0);
    const double rate = std::pow(2.0, exp);
    return std::min(rate, kMaxScrubRate);
}

} // namespace

TimecodeEngine::TimecodeEngine() = default;

double TimecodeEngine::wallClockSecondsOfDay() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    const auto subsec = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
    return static_cast<double>(local.tm_hour) * 3600.0
         + static_cast<double>(local.tm_min) * 60.0
         + static_cast<double>(local.tm_sec)
         + static_cast<double>(subsec) / 1.0e6;
}

double TimecodeEngine::currentSecondsLocked() const {
    if (clockMode_) {
        return wallClockSecondsOfDay();
    }
    if (rate_ == 0.0) {
        return anchorSeconds_;
    }
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - anchor_).count();
    double v = anchorSeconds_ + rate_ * dt;
    if (v < 0.0) v = 0.0;
    return v;
}

double TimecodeEngine::currentSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentSecondsLocked();
}

TransportSnapshot TimecodeEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TransportSnapshot s;
    s.playheadSeconds = currentSecondsLocked();
    s.rate = clockMode_ ? 1.0 : rate_;
    s.playing = playing_ && !clockMode_;
    s.clockMode = clockMode_;
    return s;
}

void TimecodeEngine::setRateLocked(double newRate) {
    // Capture present time, then change rate so motion is continuous.
    anchorSeconds_ = currentSecondsLocked();
    anchor_ = Clock::now();
    rate_ = newRate;
}

void TimecodeEngine::play() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    playing_ = true;
    setRateLocked(1.0);
}

void TimecodeEngine::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    playing_ = false;
    setRateLocked(0.0);
}

bool TimecodeEngine::isPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_ && !clockMode_;
}

void TimecodeEngine::stopOrRewindToZero() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    if (playing_) {
        playing_ = false;
        setRateLocked(0.0);
        return;
    }
    // Already paused — jump to 0.
    anchorSeconds_ = 0.0;
    anchor_ = Clock::now();
    rate_ = 0.0;
}

void TimecodeEngine::seek(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    if (seconds < 0.0) seconds = 0.0;
    anchorSeconds_ = seconds;
    anchor_ = Clock::now();
    rate_ = playing_ ? 1.0 : 0.0;
}

void TimecodeEngine::nudge(double deltaSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    double next = currentSecondsLocked() + deltaSeconds;
    if (next < 0.0) next = 0.0;
    anchorSeconds_ = next;
    anchor_ = Clock::now();
    rate_ = playing_ ? 1.0 : 0.0;
}

void TimecodeEngine::scrubPress(ScrubDirection dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    auto& s = (dir == ScrubDirection::Forward) ? scrubFwd_ : scrubBwd_;
    s.held = true;
    s.pressedAt = Clock::now();
    s.moved = false;
}

void TimecodeEngine::scrubRelease(ScrubDirection dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;
    auto& s = (dir == ScrubDirection::Forward) ? scrubFwd_ : scrubBwd_;
    if (!s.held) return;

    const double held = std::chrono::duration<double>(Clock::now() - s.pressedAt).count();
    s.held = false;

    if (held < kTapHoldThresholdSeconds && !s.moved) {
        // Treat as tap → jump.
        const double sign = (dir == ScrubDirection::Forward) ? 1.0 : -1.0;
        double next = currentSecondsLocked() + sign * tapJumpSeconds_;
        if (next < 0.0) next = 0.0;
        anchorSeconds_ = next;
        anchor_ = Clock::now();
        rate_ = playing_ ? 1.0 : 0.0;
    } else {
        // Hold ended — return to play/pause baseline.
        setRateLocked(playing_ ? 1.0 : 0.0);
    }
    s.moved = false;
}

void TimecodeEngine::scrubTick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clockMode_) return;

    bool wantScrub = false;
    double scrubRate = 0.0;

    auto consider = [&](ScrubState& s, double sign) {
        if (!s.held) return;
        const double held = std::chrono::duration<double>(Clock::now() - s.pressedAt).count();
        if (held < kTapHoldThresholdSeconds) return; // still tap territory
        s.moved = true;
        const double r = sign * scrubRateForHold(held - kTapHoldThresholdSeconds);
        if (std::abs(r) > std::abs(scrubRate)) scrubRate = r;
        wantScrub = true;
    };
    consider(scrubFwd_, +1.0);
    consider(scrubBwd_, -1.0);

    if (wantScrub) {
        if (rate_ != scrubRate) setRateLocked(scrubRate);
    } else {
        const double baseline = playing_ ? 1.0 : 0.0;
        if (rate_ != baseline) setRateLocked(baseline);
    }
}

bool TimecodeEngine::wasTap(ScrubDirection dir) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& s = (dir == ScrubDirection::Forward) ? scrubFwd_ : scrubBwd_;
    return s.held && !s.moved;
}

void TimecodeEngine::setClockMode(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (on == clockMode_) return;
    if (on) {
        // Save current playhead position (so leaving clock mode restores).
        anchorSeconds_ = currentSecondsLocked();
        anchor_ = Clock::now();
        rate_ = 0.0;
        playing_ = false;
        clockMode_ = true;
    } else {
        clockMode_ = false;
        // Resume paused at the playhead we had when we entered clock mode.
        anchor_ = Clock::now();
        rate_ = 0.0;
        playing_ = false;
    }
}

bool TimecodeEngine::clockMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clockMode_;
}

void TimecodeEngine::setTapJumpSeconds(double s) {
    std::lock_guard<std::mutex> lock(mutex_);
    tapJumpSeconds_ = (s > 0.0) ? s : 10.0;
}

double TimecodeEngine::tapJumpSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tapJumpSeconds_;
}

} // namespace libera_timecode
