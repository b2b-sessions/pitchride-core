// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/clock_sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace b2b {
namespace {

constexpr Nanos kBucketLength = 2 * kNanosPerSec;
constexpr size_t kMaxBuckets = 60;        // two minutes of history
constexpr size_t kMinBucketsForDrift = 10; // need ~20 s before trusting a drift estimate
constexpr double kMaxDriftPpm = 500.0;    // anything beyond this is noise, not a real clock

} // namespace

void ClockSync::addSample(Nanos t1, Nanos t2, Nanos t3, Nanos t4) {
    const Nanos rtt = (t4 - t1) - (t3 - t2);
    if (rtt < 0) {
        return; // impossible sample, discard
    }
    const Nanos offset = ((t2 - t1) + (t3 - t4)) / 2;

    std::lock_guard<std::mutex> lock(mutex_);
    window_.push_back({offset, rtt, t4});
    if (window_.size() > kWindow) {
        window_.pop_front();
    }
    const int n = count_.fetch_add(1, std::memory_order_relaxed) + 1;
    const Nanos now = t4;

    // 1. Drift: the two clocks tick at slightly different rates (tens of ppm
    //    is normal), so the offset slowly changes. Measuring a rate that small
    //    needs a long baseline, so we keep the fastest sample from each
    //    2-second bucket over the last two minutes and fit a line through them.
    const Nanos bucketStart = now - (now % kBucketLength);
    if (buckets_.empty() || buckets_.back().start != bucketStart) {
        buckets_.push_back({bucketStart, {offset, rtt, t4}});
        if (buckets_.size() > kMaxBuckets) {
            buckets_.pop_front();
        }
    } else if (rtt < buckets_.back().best.rtt) {
        buckets_.back().best = {offset, rtt, t4};
    }

    double slope = 0.0; // change in offset per nanosecond of our clock
    if (buckets_.size() >= kMinBucketsForDrift) {
        const double tRef = static_cast<double>(buckets_.front().best.localTime);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const double count = static_cast<double>(buckets_.size());
        for (const Bucket& b : buckets_) {
            const double x = static_cast<double>(b.best.localTime) - tRef;
            const double y = static_cast<double>(b.best.offset);
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double denom = count * sxx - sx * sx;
        if (denom > 0) {
            slope = (count * sxy - sx * sy) / denom;
            if (std::fabs(slope) * 1e6 > kMaxDriftPpm) {
                slope = 0.0;
            }
        }
    }
    driftPpm_.store(slope * 1e6, std::memory_order_relaxed);

    // 2. Offset: take the fastest eighth of the window (least affected by
    //    jitter), project each to "now" using the drift, and use the median.
    std::vector<Sample> sorted(window_.begin(), window_.end());
    std::sort(sorted.begin(), sorted.end(), [](const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
    const size_t keep = std::max<size_t>(1, sorted.size() / 8);
    std::vector<Nanos> best;
    best.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        const double age = static_cast<double>(now - sorted[i].localTime);
        best.push_back(sorted[i].offset + static_cast<Nanos>(slope * age));
    }
    std::nth_element(best.begin(), best.begin() + static_cast<long>(best.size() / 2), best.end());
    const Nanos raw = best[best.size() / 2];
    bestRtt_.store(sorted.front().rtt, std::memory_order_relaxed);

    // 3. Smooth small changes so playback never jumps; snap if we are far off
    //    (initial convergence, or the network path changed).
    const Nanos current = offset_.load(std::memory_order_relaxed);
    Nanos next;
    if (!ready_.load(std::memory_order_relaxed) || std::llabs(raw - current) > 5 * kNanosPerMs) {
        next = raw;
    } else {
        next = current + (raw - current) / 8;
    }
    offset_.store(next, std::memory_order_release);

    if (n >= kMinSamples) {
        ready_.store(true, std::memory_order_release);
    }
}

} // namespace b2b
