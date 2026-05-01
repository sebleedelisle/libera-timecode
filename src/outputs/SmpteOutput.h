#pragma once

#include "../Settings.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RtAudio;
struct LTCEncoder;

namespace libera_timecode {

class TimecodeEngine;

class SmpteOutput {
public:
    using LtcSampleProbe = std::function<void(const unsigned char* samples,
                                              std::size_t count,
                                              std::uint64_t firstSample)>;

    SmpteOutput(TimecodeEngine& engine);
    ~SmpteOutput();

    void applyConfig(const SmpteSettings& config);
    void setLtcSampleProbe(LtcSampleProbe probe);
    void stop();
    bool running() const { return running_.load(); }
    std::string status() const;

    static std::vector<std::string> availableAudioDevices();

private:
    static int audioCallback(void* outputBuffer, void* inputBuffer,
                             unsigned int nFrames, double streamTime,
                             unsigned int status, void* userData);

    void controlThreadMain();
    bool openStream(const SmpteSettings& cfg);
    void closeStream();
    void notifyLtcSampleProbe(const unsigned char* samples,
                              std::size_t count,
                              std::uint64_t firstSample);

    TimecodeEngine& engine_;
    std::unique_ptr<RtAudio> audio_;
    LTCEncoder* enc_{nullptr};

    mutable std::mutex mutex_;
    SmpteSettings config_;
    std::string lastError_;
    std::string openDeviceLabel_;

    // Buffer for libltc samples produced per frame (uint8 centered at 128).
    std::vector<unsigned char> ltcBuf_;
    std::size_t ltcBufRead_{0};
    std::size_t ltcBufFill_{0};
    int frameNumberLastEncoded_{-1};
    int channelsOpen_{2};
    std::atomic<float> callbackLevel_{0.5f};
    std::atomic<int> callbackChannelMode_{2};
    std::atomic<int> callbackFpsIndex_{static_cast<int>(FrameRate::fps_30_NDF)};
    bool ltcTimelineValid_{false};
    bool ltcTimelineActive_{false};
    long long ltcTimelineFrameIndex_{0};
    FrameRate ltcTimelineFps_{FrameRate::fps_30_NDF};
    std::uint64_t ltcProbeSampleCursor_{0};

    mutable std::mutex ltcProbeMutex_;
    LtcSampleProbe ltcSampleProbe_;
    std::atomic<bool> ltcSampleProbeEnabled_{false};

    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> configDirty_{false};
    std::thread thread_;
};

} // namespace libera_timecode
