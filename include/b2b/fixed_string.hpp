// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace b2b {

// A small string stored inline (no heap), so events containing text can be
// copied on the audio thread without allocating. Longer input is truncated.
template <size_t Capacity>
class FixedString {
public:
    FixedString() { data_[0] = '\0'; }
    FixedString(std::string_view s) { assign(s); } // NOLINT(google-explicit-constructor)
    FixedString(const char* s) { assign(s ? std::string_view(s) : std::string_view()); } // NOLINT

    void assign(std::string_view s) {
        size_ = s.size() < Capacity ? s.size() : Capacity;
        std::memcpy(data_, s.data(), size_);
        data_[size_] = '\0';
    }

    std::string_view view() const { return {data_, size_}; }
    std::string str() const { return std::string(data_, size_); }
    const char* c_str() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    static constexpr size_t capacity() { return Capacity; }

    bool operator==(std::string_view other) const { return view() == other; }
    bool operator!=(std::string_view other) const { return view() != other; }

private:
    char data_[Capacity + 1];
    size_t size_ = 0;
};

} // namespace b2b
