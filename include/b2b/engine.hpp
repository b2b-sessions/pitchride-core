// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "b2b/event_log.hpp"
#include "b2b/session.hpp"
#include "b2b/time.hpp"

namespace b2b {

// Controls used by the test engine.
enum Control : uint16_t {
    kChannelAGain = 0, // on-beat clicks
    kChannelBGain = 1, // off-beat clicks
};
inline constexpr const char* kKeyGainA = "[Metronome],gainA";
inline constexpr const char* kKeyGainB = "[Metronome],gainB";

// A scripted "DJ move" for the leader: set a control at a given beat.
struct ScriptAction {
    double beat;
    uint16_t control;
    float value;
};

struct EngineConfig {
    int sampleRate = 48000;
    float pan = 0.0f; // -1 = left, 0 = centre, 1 = right
};

// A stand-in for the real DJ engine (Mixxx, later): a two-channel metronome.
//   Channel A clicks on every beat (higher pitch on the first beat of each bar).
//   Channel B clicks on every off-beat.
//
// It shows the core idea in miniature:
//   - The leader renders "live" on the shared clock and its moves are sent
//     tagged with the exact beat they happened on.
//   - The follower renders `delay` behind the shared clock, so each move
//     arrives before it is needed and is applied on the exact same sample of
//     the music as on the leader.
class Engine {
public:
    Engine(Session& session, EventLog& log, EngineConfig config);

    // Leader only: DJ moves to perform during the session.
    void setScript(std::vector<ScriptAction> script);

    // Render interleaved stereo audio.
    //   localTimeOfFirstSample: our local clock time at which out[0] will be heard
    //   trueTimeOfFirstSample:  same moment in ground-truth time (for logging only)
    void render(float* out, int frames, Nanos localTimeOfFirstSample, Nanos trueTimeOfFirstSample);

    uint64_t lateEvents() const { return lateEvents_; }
    uint64_t appliedEvents() const { return appliedEvents_; }

private:
    struct Voice {
        int remaining = 0; // samples left in the click
        double phase = 0;
        double freq = 1000;
        int channel = 0;
    };

    static int controlIndex(const ControlEvent& ev);
    void applyEvent(const ControlEvent& ev, Nanos sharedNow, Nanos trueTime);
    void trigger(int channel, int64_t index, double freq, Nanos trueTime, double beat);

    Session& session_;
    EventLog& log_;
    EngineConfig config_;

    bool started_ = false;
    Nanos renderPos_ = 0; // shared-timeline time of the next sample we render
    double lastBeat_ = -1e9;
    float gain_[2] = {1.0f, 1.0f};
    Voice voices_[2];

    std::vector<ScriptAction> script_;
    size_t scriptIndex_ = 0;
    uint32_t nextSeq_ = 1;

    std::vector<ControlEvent> pending_; // follower: received, not yet applied (sorted by time)

    uint64_t lateEvents_ = 0;
    uint64_t appliedEvents_ = 0;
};

} // namespace b2b
