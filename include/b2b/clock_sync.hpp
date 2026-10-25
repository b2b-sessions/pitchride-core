// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <deque>
#include <mutex>

#include "b2b/time.hpp"

namespace b2b {

// Estimates the offset between our clock and the leader's clock using
// NTP-style ping/pong timestamps:
//
//   t1  we send a ping          (our clock)
//   t2  leader receives it      (leader clock)
//   t3  leader sends the pong   (leader clock)
//   t4  we receive the pong     (our clock)
//
//   offset = ((t2 - t1) + (t3 - t4)) / 2     leader clock minus our clock
//   rtt    = (t4 - t1) - (t3 - t2)           network round trip
//
// Pings that hit network jitter have a large rtt and a skewed offset, so we
// keep a window of recent samples and trust only the fastest ones. We also
// estimate drift (the clocks ticking at slightly different rates) so older
// samples can be projected forward to "now".
//
// Known limitation (true of NTP too): if the path is consistently slower in one
// direction than the other, the estimate is off by half that difference. This
// does not affect musical accuracy (events land on exact beats either way); it
// only shifts how far behind the leader the follower actually plays.
class ClockSync {
public:
    void addSample(Nanos t1, Nanos t2, Nanos t3, Nanos t4);

    bool hasEstimate() const { return ready_.load(std::memory_order_acquire); }

    // Leader clock minus our clock. Safe to call from the audio thread.
    Nanos offset() const { return offset_.load(std::memory_order_acquire); }

    Nanos bestRtt() const { return bestRtt_.load(std::memory_order_relaxed); }

    // Estimated rate difference between the clocks, in parts per million.
    double driftPpm() const { return driftPpm_.load(std::memory_order_relaxed); }
    int sampleCount() const { return count_.load(std::memory_order_relaxed); }

    // Number of samples required before the estimate is considered usable.
    static constexpr int kMinSamples = 8;

private:
    struct Sample {
        Nanos offset;
        Nanos rtt;
        Nanos localTime; // t4, when we received the pong
    };

    struct Bucket {
        Nanos start;
        Sample best; // fastest sample in this time bucket
    };

    static constexpr size_t kWindow = 64;

    std::mutex mutex_;
    std::deque<Sample> window_;
    std::deque<Bucket> buckets_;

    std::atomic<Nanos> offset_{0};
    std::atomic<Nanos> bestRtt_{0};
    std::atomic<double> driftPpm_{0.0};
    std::atomic<int> count_{0};
    std::atomic<bool> ready_{false};
};

} // namespace b2b
