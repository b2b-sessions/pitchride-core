// SPDX-License-Identifier: GPL-2.0-or-later
//
// How long a track takes to reach the friend over the session stream, on
// simulated networks, and what it does to the sync traffic meanwhile.
//
//   b2b_bench_share [upload cap KB/s, default 1024] [MB, default 8]
//
// For each network: time from request to the whole file received, and the
// sync round trip (p90) before and at its worst during the transfer.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "b2b/file_share.hpp"

using namespace b2b;
namespace fs = std::filesystem;

namespace {

struct Net {
    const char* name;
    NetSimConfig sim;
};

double ms(Nanos n) { return static_cast<double>(n) / 1e6; }

void run(const Net& net, uint64_t capKBps, size_t bytes, uint16_t port) {
    const fs::path root = fs::temp_directory_path() / ("b2b_bench_share_" + std::to_string(port));
    fs::remove_all(root);
    fs::create_directories(root);
    std::vector<char> data(bytes);
    std::mt19937 rng(7);
    for (auto& c : data) {
        c = static_cast<char>(rng());
    }
    std::memcpy(data.data(), "ID3", 3);
    const std::string path = (root / "track.mp3").string();
    std::ofstream(path, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));

    LocalClock hostClock;
    LocalClock guestClock;
    SessionConfig hc;
    hc.role = Role::Leader;
    hc.localPort = port;
    hc.sim = net.sim;
    hc.sim.seed = 1;
    SessionConfig gc;
    gc.role = Role::Follower;
    gc.peerHost = "127.0.0.1";
    gc.peerPort = port;
    gc.sim = net.sim;
    gc.sim.seed = 2;
    Session host(hc, hostClock);
    Session guest(gc, guestClock);
    host.start();
    guest.start();

    std::mutex m;
    bool got = false;
    FileShareConfig c;
    c.rateBytesPerSec = capKBps * 1024;
    FileShare::Callbacks hcb;
    hcb.lookup = [&](uint64_t id, std::string& p) {
        p = path;
        return id == 1;
    };
    FileShare::Callbacks gcb;
    gcb.onReceived = [&](uint64_t, std::string) {
        std::lock_guard<std::mutex> l(m);
        got = true;
    };
    c.session = &host;
    c.role = Role::Leader;
    c.cacheDir = (root / "host-cache").string();
    FileShare hs(c, hcb);
    c.session = &guest;
    c.role = Role::Follower;
    c.cacheDir = (root / "guest-cache").string();
    FileShare gs(c, gcb);
    hs.start();
    gs.start();

    // Linked, and a few seconds of quiet sync traffic for a baseline.
    for (int i = 0; i < 200 && !(hs.connected() && gs.connected()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const double quiet = ms(guest.rttP90());

    const auto t0 = std::chrono::steady_clock::now();
    gs.request(1, 0);
    double worst = 0;
    double secs = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        worst = std::max({worst, ms(guest.rttP90()), ms(host.rttP90())});
        static int tick = 0;
        if (std::getenv("BENCH_TRACE") && ++tick % 10 == 0) {
            std::printf("    %.1fs window %d  sent %.2f MB  rtt %.0f\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                        host.streamWindow(), hs.stats().bytesSent.load() / 1048576.0, ms(host.rttP90()));
        }
        secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> l(m);
        if (got || secs > 120) {
            break;
        }
    }
    std::printf("%-44s %6.1f s   sync rtt p90: quiet %4.0f ms, during %4.0f ms%s\n", net.name, secs, quiet, worst,
                got ? "" : "   (NOT RECEIVED)");
    std::fflush(stdout);
    gs.stop();
    hs.stop();
    guest.stop();
    host.stop();
    fs::remove_all(root);
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t cap = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1024;
    const double mb = argc > 2 ? std::atof(argv[2]) : 8;
    const char* only = argc > 3 ? argv[3] : nullptr;
    std::printf("upload cap %llu KB/s, %.1f MB track\n", static_cast<unsigned long long>(cap), mb);
    auto sim = [](double delay, double jitter, double loss, double bw) {
        NetSimConfig s;
        s.delayMs = delay;
        s.jitterMs = jitter;
        s.lossPct = loss;
        s.bandwidthKBps = bw;
        return s;
    };
    const Net nets[] = {
        {"same city, fast broadband (20 ms rtt)", sim(10, 1, 0, 0)},
        {"across Europe, Wi-Fi (50 ms rtt, 1% loss)", sim(25, 3, 1, 0)},
        {"far away (150 ms rtt)", sim(75, 3, 0, 0)},
        {"far away, no jitter (150 ms rtt)", sim(75, 0, 0, 0)},
        {"upload 2.5 MB/s, buffered (20 Mbit)", sim(10, 1, 0, 2500)},
        {"upload 1 MB/s, buffered (8 Mbit)", sim(10, 1, 0, 1000)},
        {"mobile hotspot (80 ms, 2% loss, 600 KB/s)", sim(40, 8, 2, 600)},
    };
    uint16_t port = 9700;
    for (const Net& n : nets) {
        if (!only || std::strstr(n.name, only)) {
            run(n, cap, static_cast<size_t>(mb * 1024 * 1024), port);
        }
        port += 2;
    }
    return 0;
}
