// SPDX-License-Identifier: GPL-2.0-or-later
//
// Sealed bug reports: only the right key opens them, tampering is caught.
// With arguments `seal <recipient public hex> <in> <out>` it seals a file, so
// the team's Python tool can be checked against this code.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "b2b/report.hpp"

using namespace b2b;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            gFailures++;                                                  \
        }                                                                 \
    } while (0)

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::strcmp(argv[1], "seal") == 0) {
        crypto::Key pub{};
        if (!crypto::fromHex(argv[2], pub)) {
            return 2;
        }
        std::ifstream in(argv[3], std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto sealed = sealReport(pub, data.data(), data.size());
        std::ofstream(argv[4], std::ios::binary).write(reinterpret_cast<const char*>(sealed.data()),
                                                        static_cast<std::streamsize>(sealed.size()));
        return 0;
    }

    std::puts("the built-in report key is a real key");
    crypto::Key builtIn{};
    CHECK(crypto::fromHex(kReportPublicKeyHex, builtIn));

    std::puts("sealed reports open with the right key only");
    const crypto::KeyPair team = crypto::KeyPair::generate();
    const crypto::KeyPair other = crypto::KeyPair::generate();
    const std::string text = "B2BR\x01 the deck jumped when I pressed cue";
    const auto sealed = sealReport(team.publicKey, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    CHECK(sealed.size() == text.size() + 72);
    std::vector<uint8_t> out;
    CHECK(openReport(team.secretKey, sealed.data(), sealed.size(), out));
    CHECK(std::string(out.begin(), out.end()) == text);
    CHECK(!openReport(other.secretKey, sealed.data(), sealed.size(), out));

    std::puts("a changed byte anywhere is caught");
    for (size_t at : {size_t{0}, size_t{40}, size_t{60}, sealed.size() - 1}) {
        auto bad = sealed;
        bad[at] ^= 1;
        CHECK(!openReport(team.secretKey, bad.data(), bad.size(), out));
    }

    std::puts("the same report sealed twice looks different");
    const auto again = sealReport(team.publicKey, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    CHECK(again != sealed);

    if (gFailures == 0) {
        std::puts("all report tests passed");
        return 0;
    }
    std::printf("%d failure(s)\n", gFailures);
    return 1;
}
