#pragma once

#include "Timecode.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace libera_timecode {

// Snapshot returned to outputs/UI. Cheap to copy.
struct TransportSnapshot {
    double playheadSeconds{0.0};
    double rate{0.0};         // 0 = paused, playbackRate = play, ±N = scrub speed
    bool playing{false};      // true when transport play is engaged
    bool clockMode{false};    // true if displaying / sending wall clock
};

// Engine owns the transport. All public methods are thread-safe. Outputs poll
// currentSeconds() in their own threads; the UI calls control methods.
class TimecodeEngine {
public:
    using Clock = std::chrono::steady_clock;

    TimecodeEngine();

    // Returns current playhead seconds. In clock mode, returns wall-time
    // seconds-of-day (0..86400) so outputs can encode "actual time".
    double currentSeconds() const;

    TransportSnapshot snapshot() const;

    void play();           // start running at 1.0×
    void pause();          // stop, keep playhead
    bool isPlaying() const;

    // Stop button. If currently playing, pauses. If already paused and
    // playhead != 0, jumps to 0. (Mirrors Liberation's transport behaviour.)
    void stopOrRewindToZero();

    void seek(double seconds);    // jump to absolute time (>= 0)
    void nudge(double deltaSeconds);

    // Scrubbing. Press starts a hold; release ends it.
    // While held, the engine ramps the scrub rate upwards (1× → 8×) the
    // longer the press is held; sign depends on direction.
    enum class ScrubDirection { Forward, Backward };
    void scrubPress(ScrubDirection dir);
    void scrubRelease(ScrubDirection dir);
    void scrubTick(); // call every UI frame; updates rate based on hold time

    // True if no scrub release has consumed the press yet (used to detect
    // tap-vs-hold for the 10-second jump).
    bool wasTap(ScrubDirection dir) const;

    void setClockMode(bool on);
    bool clockMode() const;

    // Configurable tap-jump amount in seconds (default 10).
    void setTapJumpSeconds(double s);
    double tapJumpSeconds() const;

    // Varispeed: multiplier applied to the play() baseline rate. 1.0 is
    // normal speed, 0.5 = half, 2.0 = double. During playback, a new rate
    // takes effect immediately without disturbing the current playhead
    // position. Active scrub holds keep their scrub rate until release.
    void setPlaybackRate(double r);
    double playbackRate() const;

private:
    mutable std::mutex mutex_;

    // Anchor model: at time `anchor_`, the playhead was `anchorSeconds_`,
    // and it advances at `rate_` per second of wall time.
    double anchorSeconds_{0.0};
    Clock::time_point anchor_{Clock::now()};
    double rate_{0.0};

    bool playing_{false};
    bool clockMode_{false};
    double tapJumpSeconds_{10.0};
    double playbackRate_{1.0};

    struct ScrubState {
        bool held{false};
        Clock::time_point pressedAt{};
        bool moved{false};   // true once we've started scrubbing (vs still tap territory)
    };
    ScrubState scrubFwd_;
    ScrubState scrubBwd_;

    // Set rate while holding the lock. Captures current playhead as anchor.
    void setRateLocked(double newRate);
    double currentSecondsLocked() const;
    static double wallClockSecondsOfDay();
};

} // namespace libera_timecode
