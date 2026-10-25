// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace b2b {
namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr Nanos kHardResync = 20 * kNanosPerMs; // further off than this: jump
constexpr int kSlewDivisor = 8;                  // otherwise close 1/8 of the gap per block

} // namespace

Engine::Engine(Session& session, EventLog& log, EngineConfig config)
    : session_(session), log_(log), config_(config) {
    pending_.reserve(1024);
}

void Engine::setScript(std::vector<ScriptAction> script) {
    std::sort(script.begin(), script.end(),
              [](const ScriptAction& a, const ScriptAction& b) { return a.beat < b.beat; });
    script_ = std::move(script);
    scriptIndex_ = 0;
}

int Engine::controlIndex(const ControlEvent& ev) {
    if (ev.key == kKeyGainA) {
        return kChannelAGain;
    }
    if (ev.key == kKeyGainB) {
        return kChannelBGain;
    }
    return -1;
}

void Engine::applyEvent(const ControlEvent& ev, Nanos sharedNow, Nanos trueTime) {
    const int control = controlIndex(ev);
    if (control >= 0) {
        gain_[control] = static_cast<float>(ev.value);
    }
    const Timeline tl = session_.timeline();
    const double lateMs = std::max(0.0, toMs(sharedNow - ev.time));
    const double sampleMs = 1000.0 / config_.sampleRate;
    if (lateMs > sampleMs) {
        lateEvents_++;
    }
    appliedEvents_++;
    log_.add({'E', static_cast<int64_t>(ev.seq), tl.beatAt(ev.time), control, ev.value, trueTime, lateMs});
}

void Engine::trigger(int channel, int64_t index, double freq, Nanos trueTime, double beat) {
    Voice& v = voices_[channel];
    v.remaining = config_.sampleRate / 50; // 20 ms click
    v.phase = 0;
    v.freq = freq;
    v.channel = channel;
    log_.add({'C', index, beat, channel, gain_[channel], trueTime, 0.0});
}

void Engine::render(float* out, int frames, Nanos localTimeOfFirstSample, Nanos trueTimeOfFirstSample) {
    std::fill(out, out + frames * 2, 0.0f);
    if (!session_.ready()) {
        return;
    }

    const Timeline tl = session_.timeline();
    const double nsPerSample = 1e9 / config_.sampleRate;

    // Where on the shared timeline should the first sample of this block be?
    const Nanos target = session_.sharedFromLocal(localTimeOfFirstSample) - session_.renderDelay();
    if (!started_) {
        renderPos_ = target;
        lastBeat_ = tl.beatAt(target);
        started_ = true;
    } else {
        const Nanos error = target - renderPos_;
        if (std::llabs(error) > kHardResync) {
            log_.add({'R', 0, tl.beatAt(renderPos_), 0, 0, trueTimeOfFirstSample, toMs(error)});
            renderPos_ = target;
            lastBeat_ = tl.beatAt(target);
        } else {
            renderPos_ += error / kSlewDivisor;
        }
    }

    // Follower: collect events that arrived from the network.
    ControlEvent incoming;
    while (session_.popReceivedEvent(incoming)) {
        auto pos = std::upper_bound(pending_.begin(), pending_.end(), incoming,
                                    [](const ControlEvent& a, const ControlEvent& b) { return a.time < b.time; });
        pending_.insert(pos, incoming);
    }

    const float leftGain = std::min(1.0f, 1.0f - config_.pan);
    const float rightGain = std::min(1.0f, 1.0f + config_.pan);

    for (int i = 0; i < frames; ++i) {
        const Nanos t = renderPos_ + static_cast<Nanos>(i * nsPerSample);
        const Nanos trueT = trueTimeOfFirstSample + static_cast<Nanos>(i * nsPerSample);
        const double beat = tl.beatAt(t);

        // Leader: scripted DJ moves happen live, then get sent stamped with
        // the exact shared-clock time they happened at.
        while (scriptIndex_ < script_.size() && script_[scriptIndex_].beat <= beat) {
            const ScriptAction& a = script_[scriptIndex_++];
            ControlEvent ev;
            ev.seq = nextSeq_++;
            ev.time = tl.timeAtBeat(a.beat);
            ev.key = a.control == kChannelAGain ? kKeyGainA : kKeyGainB;
            ev.value = a.value;
            applyEvent(ev, t, trueT);
            session_.sendEvent(ev);
        }

        // Follower: apply received moves on the exact sample they belong to.
        while (!pending_.empty() && pending_.front().time <= t) {
            applyEvent(pending_.front(), t, trueT);
            pending_.erase(pending_.begin());
        }

        // Clicks: channel A on every beat, channel B on every off-beat.
        if (beat >= 0 && std::floor(beat) > std::floor(lastBeat_)) {
            const auto index = static_cast<int64_t>(std::floor(beat));
            trigger(0, index, index % 4 == 0 ? 1760.0 : 1320.0, trueT, beat);
        }
        if (beat >= 0.5 && std::floor(beat - 0.5) > std::floor(lastBeat_ - 0.5)) {
            const auto index = static_cast<int64_t>(std::floor(beat - 0.5));
            trigger(1, index, 660.0, trueT, beat);
        }
        lastBeat_ = beat;

        float sample = 0.0f;
        for (Voice& v : voices_) {
            if (v.remaining > 0) {
                const double env = static_cast<double>(v.remaining) / (config_.sampleRate / 50);
                sample += static_cast<float>(std::sin(v.phase) * env * env * 0.4) * gain_[v.channel];
                v.phase += kTwoPi * v.freq / config_.sampleRate;
                v.remaining--;
            }
        }
        out[2 * i] = sample * leftGain;
        out[2 * i + 1] = sample * rightGain;
    }

    renderPos_ += static_cast<Nanos>(frames * nsPerSample);
}

} // namespace b2b
