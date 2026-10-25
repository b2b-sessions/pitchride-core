// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "b2b/time.hpp"

namespace b2b {

// Records what happened and exactly when (ground-truth time), for analysis.
//
// Row kinds:
//   C  click played          seq = beat index, ch = channel, value = gain
//   E  event applied         seq = event seq,  ch = control, value = new value,
//                            extra = how late it was applied, in ms (0 = exactly on time)
//   S  clock sync status     value = best round trip (ms), extra = estimated offset (ns)
//   R  render resync         extra = correction (ms)
//
// Prototype note: this uses a mutex, which is fine for testing. A production
// audio path would log through a lock-free queue instead.
class EventLog {
public:
    struct Row {
        char kind;
        int64_t seq;
        double beat;
        int channel;
        double value;
        Nanos trueNs;
        double extra;
    };

    EventLog();

    void setHeader(const std::string& key, const std::string& value);
    void add(const Row& row);
    bool writeCsv(const std::string& path) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> header_;
    std::vector<Row> rows_;
};

} // namespace b2b
