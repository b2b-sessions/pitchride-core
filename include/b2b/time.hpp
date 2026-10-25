// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace b2b {

// All times are signed 64-bit nanoseconds.
using Nanos = int64_t;

constexpr Nanos kNanosPerMs = 1'000'000;
constexpr Nanos kNanosPerSec = 1'000'000'000;

inline double toMs(Nanos ns) { return static_cast<double>(ns) / 1e6; }
inline Nanos fromMs(double ms) { return static_cast<Nanos>(ms * 1e6); }

// The machine's real monotonic clock. When two instances run on one computer,
// this is the shared "ground truth" the analysis script measures against.
inline Nanos trueNow() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// A machine's local clock.
//
// In real use offset and drift are zero and this is just the monotonic clock.
// For testing on one computer we add a fake offset and drift so each instance
// behaves like a different machine whose clock disagrees with the other one.
class LocalClock {
public:
    explicit LocalClock(Nanos fakeOffset = 0, double fakeDriftPpm = 0.0)
        : offset_(fakeOffset), drift_(fakeDriftPpm * 1e-6), origin_(trueNow()) {}

    Nanos now() const { return fromTrue(trueNow()); }

    // Ground truth -> this clock.
    Nanos fromTrue(Nanos t) const {
        return t + offset_ + static_cast<Nanos>(static_cast<double>(t - origin_) * drift_);
    }

    // This clock -> ground truth (inverse of fromTrue).
    Nanos toTrue(Nanos local) const {
        const double x = static_cast<double>(local - offset_ - origin_) / (1.0 + drift_);
        return origin_ + static_cast<Nanos>(x);
    }

    Nanos fakeOffset() const { return offset_; }
    double fakeDriftPpm() const { return drift_ * 1e6; }
    Nanos origin() const { return origin_; }

private:
    Nanos offset_;
    double drift_;
    Nanos origin_;
};

} // namespace b2b
