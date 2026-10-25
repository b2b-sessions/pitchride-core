// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace b2b {

// Wire protocol. Every packet: 4-byte magic, 1-byte type, then fields,
// all little-endian. Strings are a 1-byte length followed by the bytes.
//
// On the wire there are only two kinds of packet (envelopes):
//   Hello  (plain)     magic, 0xF0, role, protocol version, public key, app version
//   Sealed (encrypted) magic, 0xF1, counter, MAC, then one Message (below), encrypted
// Hello is laid out so that any later version can still read the version
// numbers, and tell the user who needs to update.
constexpr uint32_t kMagic = 0x34423242;   // "B2B4" (protocol 4: encrypted)
constexpr uint32_t kMagicV3 = 0x33423242; // "B2B3": the previous, unencrypted protocol
constexpr uint16_t kProtocolVersion = 5; // 5: the session secret from the room (no longer the join code alone)
constexpr uint8_t kEnvelopeHello = 0xF0;
constexpr uint8_t kEnvelopeSealed = 0xF1;

// The first 4 bytes of a packet as a magic number (0 if too short).
uint32_t peekMagic(const uint8_t* data, size_t length);

struct HelloMsg {
    uint8_t role = 0;          // 0 host, 1 guest
    uint16_t protocol = kProtocolVersion;
    std::array<uint8_t, 32> publicKey{};
    std::string appVersion;    // e.g. "0.3.0", for messages to the user
};

std::vector<uint8_t> encodeHello(const HelloMsg& hello);
std::optional<HelloMsg> decodeHello(const uint8_t* data, size_t length);

enum class MsgType : uint8_t {
    Ping = 1,
    Pong = 2,
    Start = 3,
    StartAck = 4,
    Event = 5,
    EventAck = 6,
    DeckSnapshot = 7,
    Stream = 8,
};

// Clock sync request (either way; the guest uses it to sync its clock, the
// host only to measure the round trip).
struct PingMsg {
    uint32_t id = 0;
    int64_t t1 = 0;
};

// Clock sync reply.
struct PongMsg {
    uint32_t id = 0;
    int64_t t1 = 0;
    int64_t t2 = 0;
    int64_t t3 = 0;
};

// Leader -> follower: session settings.
struct StartMsg {
    int64_t t0 = 0;       // shared-clock time of beat 0 (used by the metronome demo)
    double bpm = 0;       // (metronome demo)
    int64_t delayNs = 0;  // how far behind the shared clock the follower renders
};

struct StartAckMsg {};

// Either way, reliable: a control changed at an exact shared-clock time.
//   key   which control, e.g. "[Channel1],volume"
//   value its new value
//   text  optional extra data (e.g. a track fingerprint for a load)
struct EventMsg {
    uint32_t seq = 0;
    int64_t time = 0;
    std::string key;
    double value = 0;
    std::string text;
};

struct EventAckMsg {
    uint32_t seq = 0;
};

// From whoever is live, unreliable and frequent: where a deck is right now.
// Lets the follower notice and correct any drift in playback position.
struct DeckSnapshotMsg {
    int64_t time = 0;      // shared-clock time this snapshot describes
    uint8_t deck = 0;      // 1-based deck number
    uint8_t playing = 0;
    double positionSec = 0;
    double rate = 1.0;     // playback speed ratio (1.0 = normal)
    uint64_t trackId = 0;  // fingerprint of the loaded track (0 = none)
};

// Either way: one segment of the reliable byte stream that carries track
// sharing (KCP segments, opaque here).
struct StreamMsg {
    std::vector<uint8_t> data;
};

using Message = std::variant<PingMsg, PongMsg, StartMsg, StartAckMsg, EventMsg, EventAckMsg, DeckSnapshotMsg,
                             StreamMsg>;

std::vector<uint8_t> encode(const Message& message);
std::optional<Message> decode(const uint8_t* data, size_t length);

} // namespace b2b
