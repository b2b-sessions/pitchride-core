// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Session encryption: every packet between the two apps is encrypted and
// authenticated, so nobody on the way can read it, change it, replay it or
// pose as the other side.
//
//   - Each app makes a fresh X25519 key pair per session (forward secrecy:
//     nothing is reusable after the session).
//   - The two public keys plus a shared secret (the join code, or the
//     session password when connecting by address) give two keys, one per
//     direction (BLAKE2b).
//   - Each packet: XChaCha20-Poly1305 with a counter as nonce; a sliding
//     window rejects replays.
//
// Primitives from Monocypher (third_party/monocypher), audited, public domain.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace b2b::crypto {

using Key = std::array<uint8_t, 32>;

// Cryptographically secure random bytes from the operating system.
void randomBytes(uint8_t* out, size_t size);

struct KeyPair {
    Key publicKey{};
    Key secretKey{};
    static KeyPair generate();
    ~KeyPair();
};

std::string toHex(const Key& key);
bool fromHex(const std::string& hex, Key& out);

// What the join-code server is given in place of the session secret: 32 hex
// digits it can compare (a guest must bring the same), but can't turn back
// into the secret.
std::string rendezvousTicket(const std::string& secret);

// Six digits both people see once connected ("482 913"). The same on both
// screens means nobody sits between the two apps.
std::string checkCode(const Key& hostPublic, const Key& guestPublic, const std::string& sharedSecret);

// Bytes a sealed packet adds: 4 magic + 1 type + 8 counter + 16 MAC.
constexpr size_t kSealOverhead = 29;

// The keys for talking to one peer.
class Channel {
public:
    // False if the peer's key is unusable (e.g. a low-order point).
    bool init(const KeyPair& mine, const Key& peerPublic, bool iAmHost, const std::string& sharedSecret);

    // header (magic, type, counter) + MAC + ciphertext.
    std::vector<uint8_t> seal(uint32_t magic, uint8_t type, const uint8_t* plain, size_t size);
    // False if forged, corrupted, from other keys, or a replay.
    bool open(const uint8_t* packet, size_t size, std::vector<uint8_t>& plain);

    const Key& peerPublic() const { return peer_; }
    ~Channel();

private:
    Key send_{};
    Key recv_{};
    Key peer_{};
    uint64_t sendCounter_ = 0;
    // Replay window: which of the last kReplayWindow counters were seen. As
    // wide as WireGuard's: a track streaming in has hundreds of packets in
    // flight, and a little jitter reorders them by more than a few dozen
    // (a 64-packet window threw those away, and every one stalled the
    // transfer for a round trip).
    static constexpr uint64_t kReplayWindow = 8192;
    uint64_t recvMax_ = 0; // highest counter accepted
    std::array<uint64_t, kReplayWindow / 64> recvSeen_{}; // bit (counter % kReplayWindow)
};

} // namespace b2b::crypto
