// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/event_log.hpp"

#include <cinttypes>
#include <cstdio>

namespace b2b {

EventLog::EventLog() { rows_.reserve(1 << 16); }

void EventLog::setHeader(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : header_) {
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    }
    header_.emplace_back(key, value);
}

void EventLog::add(const Row& row) {
    std::lock_guard<std::mutex> lock(mutex_);
    rows_.push_back(row);
}

bool EventLog::writeCsv(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        return false;
    }
    for (const auto& [key, value] : header_) {
        std::fprintf(f, "# %s=%s\n", key.c_str(), value.c_str());
    }
    std::fprintf(f, "kind,seq,beat,channel,value,true_ns,extra\n");
    for (const auto& r : rows_) {
        std::fprintf(f, "%c,%" PRId64 ",%.6f,%d,%.6f,%" PRId64 ",%.6f\n", r.kind, r.seq, r.beat,
                     r.channel, r.value, r.trueNs, r.extra);
    }
    std::fclose(f);
    return true;
}

} // namespace b2b
