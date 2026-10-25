// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace b2b {

// Lock-free single-producer / single-consumer ring buffer.
// Used to pass events between the network thread and the audio thread
// without the audio thread ever blocking on a lock.
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[tail];
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace b2b
