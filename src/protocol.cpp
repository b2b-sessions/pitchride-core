// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace b2b {
namespace {

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { putLE(v, 2); }
    void u32(uint32_t v) { putLE(v, 4); }
    void i64(int64_t v) { putLE(static_cast<uint64_t>(v), 8); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        putLE(bits, 8);
    }
    void u64(uint64_t v) { putLE(v, 8); }
    void str(const std::string& s) {
        const size_t n = s.size() < 255 ? s.size() : 255;
        u8(static_cast<uint8_t>(n));
        buf_.insert(buf_.end(), s.begin(), s.begin() + static_cast<long>(n));
    }
    void bytes(const std::vector<uint8_t>& b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    void putLE(uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool ok() const { return ok_; }
    bool atEnd() const { return pos_ == len_; }

    uint8_t u8() { return static_cast<uint8_t>(getLE(1)); }
    uint16_t u16() { return static_cast<uint16_t>(getLE(2)); }
    uint32_t u32() { return static_cast<uint32_t>(getLE(4)); }
    int64_t i64() { return static_cast<int64_t>(getLE(8)); }
    float f32() {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    double f64() {
        const uint64_t bits = getLE(8);
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    uint64_t u64() { return getLE(8); }
    std::vector<uint8_t> bytes(size_t n) {
        if (!ok_ || pos_ + n > len_) {
            ok_ = false;
            return {};
        }
        std::vector<uint8_t> b(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return b;
    }
    std::string str() {
        const size_t n = u8();
        if (!ok_ || pos_ + n > len_) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

private:
    uint64_t getLE(int bytes) {
        if (!ok_ || pos_ + bytes > len_) {
            ok_ = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
            v |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += bytes;
        return v;
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace

uint32_t peekMagic(const uint8_t* data, size_t length) {
    if (length < 4) {
        return 0;
    }
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

std::vector<uint8_t> encodeHello(const HelloMsg& hello) {
    Writer w;
    w.u32(kMagic);
    w.u8(kEnvelopeHello);
    w.u8(hello.role);
    w.u16(hello.protocol);
    w.bytes(std::vector<uint8_t>(hello.publicKey.begin(), hello.publicKey.end()));
    w.str(hello.appVersion.substr(0, 40));
    return w.take();
}

std::optional<HelloMsg> decodeHello(const uint8_t* data, size_t length) {
    Reader r(data, length);
    if (r.u32() != kMagic || r.u8() != kEnvelopeHello) {
        return std::nullopt;
    }
    HelloMsg h;
    h.role = r.u8();
    h.protocol = r.u16();
    const auto key = r.bytes(32);
    h.appVersion = r.str();
    if (!r.ok() || h.role > 1) {
        return std::nullopt;
    }
    std::copy(key.begin(), key.end(), h.publicKey.begin());
    return h; // later versions may append fields: ignored
}

std::vector<uint8_t> encode(const Message& message) {
    Writer w;
    w.u32(kMagic);
    std::visit(
        [&w](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, PingMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::Ping));
                w.u32(m.id);
                w.i64(m.t1);
            } else if constexpr (std::is_same_v<T, PongMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::Pong));
                w.u32(m.id);
                w.i64(m.t1);
                w.i64(m.t2);
                w.i64(m.t3);
            } else if constexpr (std::is_same_v<T, StartMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::Start));
                w.i64(m.t0);
                w.f64(m.bpm);
                w.i64(m.delayNs);
            } else if constexpr (std::is_same_v<T, StartAckMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::StartAck));
            } else if constexpr (std::is_same_v<T, EventMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::Event));
                w.u32(m.seq);
                w.i64(m.time);
                w.str(m.key);
                w.f64(m.value);
                w.str(m.text);
            } else if constexpr (std::is_same_v<T, EventAckMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::EventAck));
                w.u32(m.seq);
            } else if constexpr (std::is_same_v<T, DeckSnapshotMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::DeckSnapshot));
                w.i64(m.time);
                w.u8(m.deck);
                w.u8(m.playing);
                w.f64(m.positionSec);
                w.f64(m.rate);
                w.u64(m.trackId);
            } else if constexpr (std::is_same_v<T, StreamMsg>) {
                w.u8(static_cast<uint8_t>(MsgType::Stream));
                w.u16(static_cast<uint16_t>(m.data.size()));
                w.bytes(m.data);
            }
        },
        message);
    return w.take();
}

std::optional<Message> decode(const uint8_t* data, size_t length) {
    Reader r(data, length);
    if (r.u32() != kMagic || !r.ok()) {
        return std::nullopt;
    }
    const auto type = static_cast<MsgType>(r.u8());
    Message out;
    switch (type) {
    case MsgType::Ping: {
        PingMsg m;
        m.id = r.u32();
        m.t1 = r.i64();
        out = m;
        break;
    }
    case MsgType::Pong: {
        PongMsg m;
        m.id = r.u32();
        m.t1 = r.i64();
        m.t2 = r.i64();
        m.t3 = r.i64();
        out = m;
        break;
    }
    case MsgType::Start: {
        StartMsg m;
        m.t0 = r.i64();
        m.bpm = r.f64();
        m.delayNs = r.i64();
        out = m;
        break;
    }
    case MsgType::StartAck:
        out = StartAckMsg{};
        break;
    case MsgType::Event: {
        EventMsg m;
        m.seq = r.u32();
        m.time = r.i64();
        m.key = r.str();
        m.value = r.f64();
        m.text = r.str();
        out = m;
        break;
    }
    case MsgType::DeckSnapshot: {
        DeckSnapshotMsg m;
        m.time = r.i64();
        m.deck = r.u8();
        m.playing = r.u8();
        m.positionSec = r.f64();
        m.rate = r.f64();
        m.trackId = r.u64();
        out = m;
        break;
    }
    case MsgType::EventAck: {
        EventAckMsg m;
        m.seq = r.u32();
        out = m;
        break;
    }
    case MsgType::Stream: {
        StreamMsg m;
        const uint16_t n = r.u16();
        m.data = r.bytes(n);
        out = std::move(m);
        break;
    }
    default:
        return std::nullopt;
    }
    if (!r.ok() || !r.atEnd()) {
        return std::nullopt;
    }
    return out;
}

} // namespace b2b
