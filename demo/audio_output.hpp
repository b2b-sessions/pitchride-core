// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>

// Thin wrapper around miniaudio for real speaker/headphone output.
// Only built when miniaudio is available (see tools/fetch_miniaudio.sh).
class AudioOutput {
public:
    // Called on the audio thread: fill `frames` interleaved stereo float samples.
    using Callback = std::function<void(float* out, int frames)>;

    AudioOutput();
    ~AudioOutput();

    bool start(int sampleRate, int periodFrames, Callback callback);
    void stop();

    // Approximate delay between rendering a sample and it being heard.
    double latencySeconds() const;

    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
