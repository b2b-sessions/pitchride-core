// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/crypto.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "monocypher.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace b2b::crypto {

void randomBytes(uint8_t* out, size_t size) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("no secure random numbers");
    }
#elif defined(__APPLE__)
    arc4random_buf(out, size);
#else
    while (size > 0) {
        const ssize_t n = getrandom(out, size, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("no secure random numbers");
        }
        out += n;
        size -= static_cast<size_t>(n);
    }
#endif
}

KeyPair KeyPair::generate() {
    KeyPair k;
    randomBytes(k.secretKey.data(), k.secretKey.size());
    crypto_x25519_public_key(k.publicKey.data(), k.secretKey.data());
    return k;
}

KeyPair::~KeyPair() { crypto_wipe(secretKey.data(), secretKey.size()); }

std::string toHex(const Key& key) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : key) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

bool fromHex(const std::string& hex, Key& out) {
    if (hex.size() != 64) {
        return false;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::string rendezvousTicket(const std::string& secret) {
    static const char label[] = "B2B rendezvous ticket";
    uint8_t hash[16];
    crypto_blake2b_keyed(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1,
                         reinterpret_cast<const uint8_t*>(secret.data()), secret.size());
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : hash) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::string checkCode(const Key& hostPublic, const Key& guestPublic, const std::string& sharedSecret) {
    static const char label[] = "B2B check code";
    std::vector<uint8_t> input(hostPublic.begin(), hostPublic.end());
    input.insert(input.end(), guestPublic.begin(), guestPublic.end());
    input.insert(input.end(), sharedSecret.begin(), sharedSecret.end());
    uint8_t hash[8];
    crypto_blake2b_keyed(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1,
                         input.data(), input.size());
    uint64_t v = 0;
    for (uint8_t b : hash) {
        v = v << 8 | b;
    }
    char text[8];
    const unsigned n = static_cast<unsigned>(v % 1000000);
    std::snprintf(text, sizeof(text), "%03u %03u", n / 1000, n % 1000);
    return text;
}

namespace {

void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

constexpr size_t kHeader = 13; // magic + type + counter

} // namespace

bool Channel::init(const KeyPair& mine, const Key& peerPublic, bool iAmHost, const std::string& sharedSecret) {
    Key shared{};
    crypto_x25519(shared.data(), mine.secretKey.data(), peerPublic.data());
    Key zero{};
    if (crypto_verify32(shared.data(), zero.data()) == 0) {
        return false; // a low-order point: no secret at all
    }
    // The shared secret (code or password) keys the derivation: without it,
    // someone who swapped in their own public key still can't get the keys.
    Key secretKey{};
    crypto_blake2b(secretKey.data(), secretKey.size(), reinterpret_cast<const uint8_t*>(sharedSecret.data()),
                   sharedSecret.size());
    const Key& hostPub = iAmHost ? mine.publicKey : peerPublic;
    const Key& guestPub = iAmHost ? peerPublic : mine.publicKey;
    std::vector<uint8_t> input;
    const char label[] = "B2B4 session keys";
    input.insert(input.end(), label, label + sizeof(label) - 1);
    input.insert(input.end(), shared.begin(), shared.end());
    input.insert(input.end(), hostPub.begin(), hostPub.end());
    input.insert(input.end(), guestPub.begin(), guestPub.end());
    Key prk{};
    crypto_blake2b_keyed(prk.data(), prk.size(), secretKey.data(), secretKey.size(), input.data(), input.size());
    Key hostToGuest{};
    Key guestToHost{};
    crypto_blake2b_keyed(hostToGuest.data(), 32, prk.data(), 32, reinterpret_cast<const uint8_t*>("h2g"), 3);
    crypto_blake2b_keyed(guestToHost.data(), 32, prk.data(), 32, reinterpret_cast<const uint8_t*>("g2h"), 3);
    send_ = iAmHost ? hostToGuest : guestToHost;
    recv_ = iAmHost ? guestToHost : hostToGuest;
    peer_ = peerPublic;
    sendCounter_ = 0;
    recvMax_ = 0;
    recvSeen_.fill(0);
    crypto_wipe(shared.data(), 32);
    crypto_wipe(secretKey.data(), 32);
    crypto_wipe(prk.data(), 32);
    crypto_wipe(hostToGuest.data(), 32);
    crypto_wipe(guestToHost.data(), 32);
    crypto_wipe(input.data(), input.size());
    return true;
}

std::vector<uint8_t> Channel::seal(uint32_t magic, uint8_t type, const uint8_t* plain, size_t size) {
    std::vector<uint8_t> out(kHeader + 16 + size);
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(magic >> (8 * i));
    }
    out[4] = type;
    put64(&out[5], ++sendCounter_);
    uint8_t nonce[24] = {};
    std::memcpy(nonce, &out[5], 8);
    crypto_aead_lock(out.data() + kHeader + 16, out.data() + kHeader, send_.data(), nonce, out.data(), kHeader, plain,
                     size);
    return out;
}

bool Channel::open(const uint8_t* packet, size_t size, std::vector<uint8_t>& plain) {
    if (size < kHeader + 16) {
        return false;
    }
    const uint64_t counter = get64(packet + 5);
    if (counter == 0) {
        return false;
    }
    auto seen = [this](uint64_t n) -> uint64_t& { return recvSeen_[(n % kReplayWindow) / 64]; };
    auto bit = [](uint64_t n) { return uint64_t{1} << (n % 64); };
    if (counter <= recvMax_) {
        if (recvMax_ - counter >= kReplayWindow || (seen(counter) & bit(counter))) {
            return false; // too old, or a replay
        }
    }
    uint8_t nonce[24] = {};
    std::memcpy(nonce, packet + 5, 8);
    plain.resize(size - kHeader - 16);
    if (crypto_aead_unlock(plain.data(), packet + kHeader, recv_.data(), nonce, packet, kHeader,
                           packet + kHeader + 16, plain.size()) != 0) {
        plain.clear();
        return false;
    }
    if (counter > recvMax_) {
        // The counters skipped over haven't been seen (their slots last held
        // counters a whole window older).
        if (counter - recvMax_ >= kReplayWindow) {
            recvSeen_.fill(0);
        } else {
            for (uint64_t n = recvMax_ + 1; n < counter; ++n) {
                seen(n) &= ~bit(n);
            }
        }
        recvMax_ = counter;
    }
    seen(counter) |= bit(counter);
    return true;
}

Channel::~Channel() {
    crypto_wipe(send_.data(), 32);
    crypto_wipe(recv_.data(), 32);
}

} // namespace b2b::crypto
