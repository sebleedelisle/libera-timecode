#include "SmpteOutput.h"

#include "../TimecodeEngine.h"

#include <RtAudio.h>
#include <ltc.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace libera_timecode {

namespace {

LTC_TV_STANDARD ltcStandardForFps(FrameRate r) {
    switch (r) {
    case FrameRate::fps_23_976:
    case FrameRate::fps_24:        return LTC_TV_FILM_24;
    case FrameRate::fps_25:        return LTC_TV_625_50;
    case FrameRate::fps_29_97_DF:
    case FrameRate::fps_29_97_NDF: return LTC_TV_525_60;
    case FrameRate::fps_30_DF:
    case FrameRate::fps_30_NDF:    return LTC_TV_1125_60;
    }
    return LTC_TV_525_60;
}

double effectiveFpsForLtc(FrameRate r) {
    // libltc encodes at the *exact* rate; for 29.97 we pass 30 plus the DF flag,
    // but we still need correct sample-per-frame allocation. Use the nominal
    // rate so audio length tracks real-time.
    return frameRateInfo(r).nominalFps;
}

} // namespace

SmpteOutput::SmpteOutput(TimecodeEngine& engine) : engine_(engine) {}

SmpteOutput::~SmpteOutput() { stop(); }

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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        configDirty_ = true;
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

    err = audio_->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = audio_->getErrorText();
        audio_->closeStream();
        audio_.reset();
        return false;
    }

    enc_ = ltc_encoder_create(sampleRate, effectiveFpsForLtc(cfg.fps),
                              ltcStandardForFps(cfg.fps), 0);
    if (!enc_) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = "ltc_encoder_create failed";
        audio_->stopStream();
        audio_->closeStream();
        audio_.reset();
        return false;
    }
    ltcBuf_.assign(8192, 128);
    ltcBufFill_ = ltcBufRead_ = 0;
    frameNumberLastEncoded_ = -1;
    channelsOpen_ = 2;

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
}

void SmpteOutput::controlThreadMain() {
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

    SmpteSettings cfg;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        cfg = self->config_;
    }

    const float level = std::clamp(cfg.level, 0.0f, 1.0f);
    const int channels = self->channelsOpen_;
    const int channelMode = cfg.channelMode;

    for (unsigned int i = 0; i < nFrames; ++i) {
        if (self->ltcBufRead_ >= self->ltcBufFill_) {
            // Need to encode another LTC frame.
            if (self->enc_) {
                const double seconds = self->engine_.currentSeconds();
                const TimecodeFields tc = secondsToFields(seconds, cfg.fps);
                SMPTETimecode smpte;
                std::memset(&smpte, 0, sizeof(smpte));
                std::strncpy(smpte.timezone, "+0000", sizeof(smpte.timezone) - 1);
                smpte.years = 0; smpte.months = 1; smpte.days = 1;
                smpte.hours = static_cast<unsigned char>(tc.hours);
                smpte.mins  = static_cast<unsigned char>(tc.minutes);
                smpte.secs  = static_cast<unsigned char>(tc.seconds);
                smpte.frame = static_cast<unsigned char>(tc.frames);
                ltc_encoder_set_timecode(self->enc_, &smpte);
                ltc_encoder_encode_frame(self->enc_);

                self->ltcBufFill_ = ltc_encoder_get_buffer(self->enc_, self->ltcBuf_.data());
                self->ltcBufRead_ = 0;
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
