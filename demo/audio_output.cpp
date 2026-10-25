// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio_output.hpp"

#ifdef B2B_HAVE_AUDIO

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

struct AudioOutput::Impl {
    ma_device device{};
    bool running = false;
    Callback callback;

    static void dataCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(device->pUserData);
        self->callback(static_cast<float*>(output), static_cast<int>(frames));
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { stop(); }

bool AudioOutput::available() { return true; }

bool AudioOutput::start(int sampleRate, int periodFrames, Callback callback) {
    impl_->callback = std::move(callback);
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.periodSizeInFrames = static_cast<ma_uint32>(periodFrames);
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = impl_.get();
    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
        return false;
    }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        return false;
    }
    impl_->running = true;
    return true;
}

void AudioOutput::stop() {
    if (impl_ && impl_->running) {
        ma_device_uninit(&impl_->device);
        impl_->running = false;
    }
}

double AudioOutput::latencySeconds() const {
    const auto& pb = impl_->device.playback;
    return static_cast<double>(pb.internalPeriodSizeInFrames) * pb.internalPeriods /
           static_cast<double>(impl_->device.sampleRate);
}

#else // no miniaudio: stub

struct AudioOutput::Impl {};
AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() = default;
bool AudioOutput::available() { return false; }
bool AudioOutput::start(int, int, Callback) { return false; }
void AudioOutput::stop() {}
double AudioOutput::latencySeconds() const { return 0.0; }

#endif
