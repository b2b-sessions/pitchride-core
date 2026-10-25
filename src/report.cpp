// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/report.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "b2b/net.hpp"
#include "monocypher.h"

namespace b2b {

// Generated for the team with tools/b2breport.py keygen (b2b-server repo).
const char* const kReportPublicKeyHex = "44f43227f02488fd149de71a33c1b10682a4e351bd7603f74f6402a1437ae56d";

namespace {

constexpr char kMagic[] = "B2BBUG1";
constexpr size_t kMagicLen = 7;
constexpr size_t kHeader = 32 + 24 + 16;

crypto::Key deriveKey(const crypto::Key& shared, const crypto::Key& ephemeralPublic,
                      const crypto::Key& recipientPublic) {
    std::vector<uint8_t> input;
    const char label[] = "B2B report v1";
    input.insert(input.end(), label, label + sizeof(label) - 1);
    input.insert(input.end(), shared.begin(), shared.end());
    input.insert(input.end(), ephemeralPublic.begin(), ephemeralPublic.end());
    input.insert(input.end(), recipientPublic.begin(), recipientPublic.end());
    crypto::Key key{};
    crypto_blake2b(key.data(), key.size(), input.data(), input.size());
    crypto_wipe(input.data(), input.size());
    return key;
}

} // namespace

std::vector<uint8_t> sealReport(const crypto::Key& recipientPublic, const uint8_t* data, size_t size) {
    const crypto::KeyPair eph = crypto::KeyPair::generate();
    crypto::Key shared{};
    crypto_x25519(shared.data(), eph.secretKey.data(), recipientPublic.data());
    crypto::Key key = deriveKey(shared, eph.publicKey, recipientPublic);
    std::vector<uint8_t> out(kHeader + size);
    std::memcpy(out.data(), eph.publicKey.data(), 32);
    crypto::randomBytes(out.data() + 32, 24);
    crypto_aead_lock(out.data() + kHeader, out.data() + 56, key.data(), out.data() + 32, nullptr, 0, data, size);
    crypto_wipe(shared.data(), 32);
    crypto_wipe(key.data(), 32);
    return out;
}

bool openReport(const crypto::Key& recipientSecret, const uint8_t* sealed, size_t size, std::vector<uint8_t>& out) {
    if (size < kHeader) {
        return false;
    }
    crypto::Key eph{};
    std::memcpy(eph.data(), sealed, 32);
    crypto::Key recipientPublic{};
    crypto_x25519_public_key(recipientPublic.data(), recipientSecret.data());
    crypto::Key shared{};
    crypto_x25519(shared.data(), recipientSecret.data(), eph.data());
    crypto::Key key = deriveKey(shared, eph, recipientPublic);
    out.resize(size - kHeader);
    const bool ok = crypto_aead_unlock(out.data(), sealed + 56, key.data(), sealed + 32, nullptr, 0,
                                       sealed + kHeader, out.size()) == 0;
    crypto_wipe(shared.data(), 32);
    crypto_wipe(key.data(), 32);
    if (!ok) {
        out.clear();
    }
    return ok;
}

ReportResult sendReport(const std::string& host, uint16_t port, const std::vector<uint8_t>& sealed, int timeoutMs) {
    ReportResult result;
    if (sealed.size() > kMaxReportBytes) {
        result.error = "The report is too large to send.";
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto remainingMs = [&] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        return static_cast<int>(std::max<long long>(0, left.count()));
    };
    Endpoint server;
    if (!UdpSocket::resolve(host, port, server)) {
        result.error = "Couldn't find the B2B server. Check your internet connection.";
        return result;
    }
    TcpSocket socket;
    if (!socket.connect(server, std::min(remainingMs(), 8000))) {
        result.error = "Couldn't reach the B2B server. Check your internet connection and try again.";
        return result;
    }
    std::vector<uint8_t> frame(kMagic, kMagic + kMagicLen);
    const auto n = static_cast<uint32_t>(sealed.size());
    for (int i = 0; i < 4; ++i) {
        frame.push_back(static_cast<uint8_t>(n >> (8 * i)));
    }
    frame.insert(frame.end(), sealed.begin(), sealed.end());

    size_t sent = 0;
    while (sent < frame.size()) {
        const int left = remainingMs();
        if (left == 0) {
            result.error = "Sending took too long. Try again on a better connection.";
            return result;
        }
        const int ready = socket.wait(false, true, std::min(left, 500));
        if (ready < 0) {
            result.error = "The connection to the server dropped while sending.";
            return result;
        }
        if (ready & 2) {
            const int k = socket.send(frame.data() + sent, frame.size() - sent);
            if (k < 0) {
                result.error = "The connection to the server dropped while sending.";
                return result;
            }
            sent += static_cast<size_t>(k);
        }
    }
    std::string reply;
    while (reply.find('\n') == std::string::npos && reply.size() < 200) {
        const int left = remainingMs();
        if (left == 0) {
            break;
        }
        const int ready = socket.wait(true, false, std::min(left, 500));
        if (ready < 0) {
            break;
        }
        if (ready & 1) {
            uint8_t buf[256];
            const int k = socket.recv(buf, sizeof(buf));
            if (k < 0) {
                break;
            }
            reply.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(k));
        }
    }
    reply = reply.substr(0, reply.find('\n'));
    if (reply.rfind("OK ", 0) == 0) {
        result.ok = true;
        result.reference = reply.substr(3);
    } else if (reply.rfind("ERR busy", 0) == 0 || reply.rfind("ERR slow", 0) == 0) {
        result.error = "The server is getting a lot of reports right now. Please try again later.";
    } else if (reply.rfind("ERR ", 0) == 0) {
        result.error = "The server didn't accept the report (" + reply.substr(4) + ").";
    } else {
        result.error = "The server didn't confirm the report. Please try again.";
    }
    return result;
}

} // namespace b2b
