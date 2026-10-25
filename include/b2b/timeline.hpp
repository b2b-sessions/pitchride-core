// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "b2b/time.hpp"

namespace b2b {

// The shared musical timeline: which beat it is at any moment of the shared clock.
// Both peers hold an identical copy, so "beat 64.0" means the same musical
// moment for everyone, regardless of network delay.
struct Timeline {
    Nanos t0 = 0;       // shared-clock time of beat 0
    double bpm = 124.0;

    double beatAt(Nanos sharedTime) const {
        return static_cast<double>(sharedTime - t0) * bpm / (60.0 * 1e9);
    }

    Nanos timeAtBeat(double beat) const {
        return t0 + static_cast<Nanos>(beat * 60.0 * 1e9 / bpm);
    }

    double nanosPerBeat() const { return 60.0 * 1e9 / bpm; }
};

} // namespace b2b
