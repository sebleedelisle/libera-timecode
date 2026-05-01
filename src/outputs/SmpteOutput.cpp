#include "SmpteOutput.h"

#include "LtcProtocol.h"
#include "OutputTiming.h"

#include "../ThreadQos.h"
#include "../TimecodeEngine.h"

#include <RtAudio.h>
#include <ltc.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace libera_timecode {

SmpteOutput::SmpteOutput(TimecodeEngine& engine) : engine_(engine) {}

SmpteOutput::~SmpteOutput() { stop(); }

void SmpteOutput::setLtcSampleProbe(LtcSampleProbe probe) {
    std::lock_guard<std::mutex> lock(ltcProbeMutex_);
    ltcSampleProbe_ = std::move(probe);
    ltcSampleProbeEnabled_.store(static_cast<bool>(ltcSampleProbe_), std::memory_order_release);
}

std::vector<std::string> SmpteOutput::availableAudioDevices() {
    std::vector<std::string> names;
    try {
        RtAudio audio;
        auto ids = audio.getDeviceIds();
        for (auto id : ids) {
            auto info = audio.getDeviceInfo(id);
            if (info.outputChannels > 0) {
                names.push_back(info.name);
            }
        }
    } catch (...) {
        // ignore
    }
    return names;
}

void SmpteOutput::applyConfig(const SmpteSettings& config) {
    // Live params (level, channelMode) are read from config_ inside the audio
    // callback every block, so they don't need a stream restart. Restarting
    // RtAudio is only needed for sampleRate, audioDevice, or fps changes.
    callbackLevel_.store(config.level, std::memory_order_relaxed);
    callbackChannelMode_.store(config.channelMode, std::memory_order_relaxed);

    bool streamChanged = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streamChanged = (config.sampleRate  != config_.sampleRate)
                     || (config.audioDevice != config_.audioDevice)
                     || (config.fps         != config_.fps);
        config_ = config;
        if (streamChanged) configDirty_ = true;
    }
    if (config.enabled && !running_.load()) {
        shouldStop_ = false;
        running_ = true;
        thread_ = std::thread([this] { controlThreadMain(); });
    } else if (!config.enabled && running_.load()) {
        stop();
    }
}

void SmpteOutput::stop() {
    shouldStop_ = true;
    if (thread_.joinable()) thread_.join();
    closeStream();
    running_ = false;
}

std::string SmpteOutput::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) return "Disabled";
    if (!lastError_.empty()) return "Error: " + lastError_;
    if (!running_.load())    return "Idle";
    return "LTC -> " + openDeviceLabel_;
}

bool SmpteOutput::openStream(const SmpteSettings& cfg) {
    closeStream();

    try {
        audio_ = std::make_unique<RtAudio>();
    } catch (std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = std::string("RtAudio init: ") + e.what();
        return false;
    }

    auto ids = audio_->getDeviceIds();
    unsigned int chosen = audio_->getDefaultOutputDevice();
    std::string chosenLabel = "Default Output";
    for (auto id : ids) {
        try {
            auto info = audio_->getDeviceInfo(id);
            if (!cfg.audioDevice.empty() && info.name == cfg.audioDevice && info.outputChannels > 0) {
                chosen = id;
                chosenLabel = info.name;
                break;
            }
            if (id == chosen && info.outputChannels > 0) {
                chosenLabel = info.name;
            }
        } catch (...) {
            // ignore individual device probe failures
        }
    }

    RtAudio::StreamParameters params;
    params.deviceId = chosen;
    params.nChannels = 2;
    params.firstChannel = 0;

    unsigned int sampleRate = (cfg.sampleRate > 0) ? cfg.sampleRate : 48000;
    unsigned int bufferFrames = 256;

    auto err = audio_->openStream(&params, nullptr, RTAUDIO_FLOAT32, sampleRate,
                                  &bufferFrames, &SmpteOutput::audioCallback, this);
    if (err != RTAUDIO_NO_ERROR) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = audio_->getErrorText();
        audio_.reset();
        return false;
    }

    const double ltcFps = ltc_protocol::effectiveFps(cfg.fps);
    enc_ = ltc_encoder_create(sampleRate, ltcFps, ltc_protocol::standardForFps(cfg.fps), 0);
    if (!enc_) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = "ltc_encoder_create failed";
        audio_->stopStream();
        audio_->closeStream();
        audio_.reset();
        return false;
    }
    if (ltc_encoder_set_buffersize(enc_, sampleRate, ltcFps * 0.5) != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = "ltc_encoder_set_buffersize failed";
        ltc_encoder_free(enc_);
        enc_ = nullptr;
        audio_->stopStream();
        audio_->closeStream();
        audio_.reset();
        return false;
    }
    ltcBuf_.assign(std::max<std::size_t>(ltc_encoder_get_buffersize(enc_), 1), 128);
    ltcBufFill_ = ltcBufRead_ = 0;
    frameNumberLastEncoded_ = -1;
    ltcTimelineValid_ = false;
    ltcTimelineActive_ = false;
    ltcTimelineFrameIndex_ = 0;
    ltcTimelineFps_ = cfg.fps;
    ltcProbeSampleCursor_ = 0;
    channelsOpen_ = 2;
    callbackFpsIndex_.store(indexOfFrameRate(cfg.fps), std::memory_order_release);

    err = audio_->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = audio_->getErrorText();
        ltc_encoder_free(enc_);
        enc_ = nullptr;
        audio_->closeStream();
        audio_.reset();
        ltcBuf_.clear();
        ltcBufFill_ = ltcBufRead_ = 0;
        ltcTimelineValid_ = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
        openDeviceLabel_ = chosenLabel;
    }
    return true;
}

void SmpteOutput::closeStream() {
    if (audio_) {
        try {
            if (audio_->isStreamRunning()) audio_->stopStream();
            if (audio_->isStreamOpen())    audio_->closeStream();
        } catch (...) {}
        audio_.reset();
    }
    if (enc_) {
        ltc_encoder_free(enc_);
        enc_ = nullptr;
    }
    ltcBuf_.clear();
    ltcBufFill_ = ltcBufRead_ = 0;
    ltcTimelineValid_ = false;
}

void SmpteOutput::notifyLtcSampleProbe(const unsigned char* samples,
                                       std::size_t count,
                                       std::uint64_t firstSample) {
    if (!ltcSampleProbeEnabled_.load(std::memory_order_acquire)) return;

    LtcSampleProbe probe;
    {
        std::lock_guard<std::mutex> lock(ltcProbeMutex_);
        probe = ltcSampleProbe_;
    }
    if (probe) probe(samples, count, firstSample);
}

void SmpteOutput::controlThreadMain() {
    setHighPriorityForOutputThread();
    SmpteSettings cfgLocal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfgLocal = config_;
        configDirty_ = false;
    }

    bool ok = openStream(cfgLocal);
    if (!ok) {
        // Retry loop while enabled
    }

    while (!shouldStop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        bool dirty = configDirty_.exchange(false);
        if (dirty) {
            SmpteSettings cfgNow;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cfgNow = config_;
            }
            ok = openStream(cfgNow);
            cfgLocal = cfgNow;
        } else if (!ok) {
            ok = openStream(cfgLocal);
        }
    }

    closeStream();
}

int SmpteOutput::audioCallback(void* outputBuffer, void* /*inputBuffer*/,
                               unsigned int nFrames, double /*streamTime*/,
                               unsigned int /*status*/, void* userData) {
    auto* self = static_cast<SmpteOutput*>(userData);
    auto* out = static_cast<float*>(outputBuffer);

    const FrameRate fps = frameRateAt(
        self->callbackFpsIndex_.load(std::memory_order_acquire));
    const float level = std::clamp(
        self->callbackLevel_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const int channels = self->channelsOpen_;
    const int channelMode = self->callbackChannelMode_.load(std::memory_order_relaxed);

    for (unsigned int i = 0; i < nFrames; ++i) {
        if (self->ltcBufRead_ >= self->ltcBufFill_) {
            // Need to encode another LTC frame.
            if (self->enc_) {
                const auto snap = self->engine_.snapshot();
                const bool active =
                    output_timing::activeRate(snap) > output_timing::kMinActiveRate;
                const long long targetFrameIndex =
                    ltc_protocol::frameIndexForSeconds(snap.playheadSeconds, fps);

                if (!active) {
                    self->ltcTimelineFrameIndex_ = targetFrameIndex;
                    self->ltcTimelineValid_ = true;
                    self->ltcTimelineActive_ = false;
                    self->ltcTimelineFps_ = fps;
                } else {
                    const long long frameError =
                        targetFrameIndex - self->ltcTimelineFrameIndex_;
                    const long long absFrameError =
                        frameError < 0 ? -frameError : frameError;
                    constexpr long long kResyncToleranceFrames = 12;
                    const bool needsAnchor =
                        !self->ltcTimelineValid_
                     || self->ltcTimelineFps_ != fps
                     || !self->ltcTimelineActive_
                     || absFrameError > kResyncToleranceFrames;

                    if (needsAnchor) {
                        self->ltcTimelineFrameIndex_ = targetFrameIndex;
                        self->ltcTimelineValid_ = true;
                        self->ltcTimelineFps_ = fps;
                    }
                    self->ltcTimelineActive_ = true;
                }

                const TimecodeFields tc =
                    frameIndexToFields(self->ltcTimelineFrameIndex_, fps);
                const double speed = output_timing::ltcSpeedFromSnapshot(snap);
                if (!ltc_protocol::encodeFrame(self->enc_, tc, fps, speed)) {
                    ltc_protocol::encodeFrame(self->enc_, tc, fps, 1.0);
                }

                self->ltcBufFill_ = ltc_encoder_copy_buffer(self->enc_, self->ltcBuf_.data());
                self->ltcBufRead_ = 0;
                if (self->ltcBufFill_ > 0) {
                    const std::uint64_t firstSample = self->ltcProbeSampleCursor_;
                    self->ltcProbeSampleCursor_ += self->ltcBufFill_;
                    self->notifyLtcSampleProbe(self->ltcBuf_.data(),
                                               self->ltcBufFill_,
                                               firstSample);
                }

                if (active) {
                    if (!snap.clockMode && snap.rate < -output_timing::kMinActiveRate) {
                        if (self->ltcTimelineFrameIndex_ > 0) {
                            --self->ltcTimelineFrameIndex_;
                        }
                    } else {
                        ++self->ltcTimelineFrameIndex_;
                    }
                }
            } else {
                self->ltcBufFill_ = self->ltcBufRead_ = 0;
            }
        }

        float sample = 0.0f;
        if (self->ltcBufRead_ < self->ltcBufFill_) {
            const unsigned char raw = self->ltcBuf_[self->ltcBufRead_++];
            sample = (static_cast<float>(raw) - 128.0f) / 128.0f * level;
        }

        // Channel routing: 0 left, 1 right, 2 both
        const float left  = (channelMode == 1) ? 0.0f : sample;
        const float right = (channelMode == 0) ? 0.0f : sample;
        out[i * channels + 0] = left;
        if (channels > 1) out[i * channels + 1] = right;
    }

    return 0;
}

} // namespace libera_timecode
