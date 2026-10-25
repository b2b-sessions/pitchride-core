// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Bug reports and logs, sent to the B2B server.
//
// A report is sealed on the user's computer to the team's report key (an
// X25519 public key built into the app): a fresh key pair per report,
// BLAKE2b to derive the key, XChaCha20-Poly1305 to encrypt. The server only
// stores what it receives; only whoever holds the private key (kept off the
// server) can read a report. So a log's file paths and IP addresses stay
// private even if the server is broken into.
//
// Wire format (TCP, same port number as the rendezvous server):
//   client: "B2BBUG1" + u32 length (little endian) + sealed report
//   server: "OK <reference>\n" or "ERR <reason>\n"
// Sealed report: ephemeral public key (32) + nonce (24) + MAC (16) + ciphertext.

#include <cstdint>
#include <string>
#include <vector>

#include "b2b/crypto.hpp"

namespace b2b {

// The team's report key (public half). Override for testing with B2B_REPORT_KEY.
extern const char* const kReportPublicKeyHex;

constexpr size_t kMaxReportBytes = 4 * 1024 * 1024; // sealed, as sent

std::vector<uint8_t> sealReport(const crypto::Key& recipientPublic, const uint8_t* data, size_t size);
// For tests and the team's own tools.
bool openReport(const crypto::Key& recipientSecret, const uint8_t* sealed, size_t size, std::vector<uint8_t>& out);

struct ReportResult {
    bool ok = false;
    std::string reference; // e.g. "20261013-191502-3fa2c1": quote it when talking to the user
    std::string error;     // for the user, in plain words
};

// Blocking (seconds at most `timeoutMs`): run it off the UI thread.
ReportResult sendReport(const std::string& host, uint16_t port, const std::vector<uint8_t>& sealed,
                        int timeoutMs = 20000);

} // namespace b2b
