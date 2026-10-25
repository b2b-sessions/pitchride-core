// SPDX-License-Identifier: GPL-2.0-or-later
//
// Two FileShare peers on this machine: transfer, priorities, refusals.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "b2b/file_share.hpp"

using namespace b2b;
namespace fs = std::filesystem;

namespace {

int gFailures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            gFailures++;                                                  \
        }                                                                 \
    } while (0)

std::vector<char> randomBytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<char> v(n);
    for (auto& c : v) {
        c = static_cast<char>(rng());
    }
    return v;
}

// Random content that starts like the named format (the receiver checks).
std::vector<char> withHeader(std::vector<char> v, const std::string& header) {
    std::copy(header.begin(), header.end(), v.begin());
    return v;
}

std::vector<char> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

template <typename F>
bool waitFor(F cond, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

} // namespace

void testTcp() {
    std::puts("=== over TCP ===");
    const fs::path root = fs::temp_directory_path() / "b2b_file_share_test";
    fs::remove_all(root);
    fs::create_directories(root / "leader-music");

    // Leader's shared tracks: a 6 MB "song", a 1 MB one, and a 3 MB one over the size cap.
    const auto big = withHeader(randomBytes(6 * 1024 * 1024 + 123, 1), "ID3");
    const auto small = withHeader(randomBytes(1024 * 1024, 2), "fLaC");
    const auto huge = randomBytes(3 * 1024 * 1024, 3);
    std::map<uint64_t, std::string> shared = {
        {0xA, (root / "leader-music" / "big track.mp3").string()},
        {0xB, (root / "leader-music" / "small.flac").string()},
        {0xC, (root / "leader-music" / "huge.wav").string()},
        {0xE, (root / "leader-music" / "fake.mp3").string()},
    };
    std::ofstream(shared[0xA], std::ios::binary).write(big.data(), static_cast<std::streamsize>(big.size()));
    std::ofstream(shared[0xB], std::ios::binary).write(small.data(), static_cast<std::streamsize>(small.size()));
    std::ofstream(shared[0xC], std::ios::binary).write(huge.data(), static_cast<std::streamsize>(huge.size()));
    const auto fake = withHeader(randomBytes(200 * 1024, 4), "MZ"); // a program, named .mp3
    std::ofstream(shared[0xE], std::ios::binary).write(fake.data(), static_cast<std::streamsize>(fake.size()));

    std::mutex m;
    std::map<uint64_t, std::string> received;
    std::map<uint64_t, std::string> failed;
    std::vector<uint64_t> order;
    std::vector<SharedTrack> peerList;
    std::atomic<int> hints{0};

    FileShareConfig lc;
    lc.role = Role::Leader;
    lc.listenPort = 9551;
    lc.cacheDir = (root / "leader-cache").string();
    lc.maxFileBytes = 8 * 1024 * 1024;
    lc.rateBytesPerSec = 20 * 1024 * 1024;
    FileShare::Callbacks lcb;
    lcb.lookup = [&](uint64_t id, std::string& path) {
        auto it = shared.find(id);
        if (it == shared.end()) {
            return false;
        }
        path = it->second;
        return true;
    };
    lcb.onPeerHint = [&](uint64_t, uint8_t) { hints++; };
    lcb.describe = [](uint64_t id, TrackMeta& meta) {
        if (id != 0xB) {
            return false;
        }
        meta.title = "Small";
        meta.artist = "Artist";
        meta.key = "8A";
        meta.bpm = 128.5;
        meta.beatsVersion = "BeatGrid-2.0";
        meta.beats = {1, 2, 3, 250};
        return true;
    };
    FileShare leader(lc, lcb);
    leader.setLocalTracks({{0xA, big.size(), "Artist - Big"}, {0xB, small.size(), "Artist - Small"}});

    FileShareConfig fc;
    fc.role = Role::Follower;
    fc.peerHost = "127.0.0.1";
    fc.peerPort = 9551;
    fc.cacheDir = (root / "follower-cache").string();
    fc.maxFileBytes = 2 * 1024 * 1024 + 512 * 1024; // follower refuses > 2.5 MB
    FileShare::Callbacks fcb;
    fcb.lookup = [](uint64_t, std::string&) { return false; };
    fcb.onPeerList = [&](std::vector<SharedTrack> l) {
        std::lock_guard<std::mutex> lock(m);
        peerList = std::move(l);
    };
    fcb.onReceived = [&](uint64_t id, std::string path) {
        std::lock_guard<std::mutex> lock(m);
        received[id] = path;
        order.push_back(id);
    };
    std::map<uint64_t, TrackMeta> metas;
    fcb.onPeerMeta = [&](uint64_t id, TrackMeta meta) {
        std::lock_guard<std::mutex> lock(m);
        metas[id] = std::move(meta);
    };
    fcb.onFailed = [&](uint64_t id, std::string reason) {
        std::lock_guard<std::mutex> lock(m);
        failed[id] = reason;
    };
    FileShare follower(fc, fcb);

    leader.start();
    follower.start();

    std::puts("connect and exchange track lists");
    CHECK(waitFor([&] { return leader.connected() && follower.connected(); }, 5000));
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return peerList.size() == 2; }, 3000));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(peerList.size() == 2 && peerList[0].name == "Artist - Big" && peerList[0].size == big.size());
    }

    std::puts("hints reach the other side");
    follower.hint(0xA, 1);
    CHECK(waitFor([&] { return hints.load() == 1; }, 2000));

    std::puts("transfer, and the receiver refuses files over its size limit");
    // A (6 MB) is over the follower's 2.5 MB limit; B arrives intact.
    follower.request(0xA, 5);
    follower.request(0xB, 0);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return received.count(0xB) && failed.count(0xA); }, 10000));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(received.count(0xB) == 1);
        if (received.count(0xB)) {
            CHECK(readAll(received[0xB]) == small);
            CHECK(fs::path(received[0xB]).filename() == "Artist - Small.flac");
        }
        std::puts("its details (name, key, BPM, beat grid) arrive with it");
        CHECK(metas.count(0xB) == 1);
        if (metas.count(0xB)) {
            const TrackMeta& t = metas[0xB];
            CHECK(t.title == "Small" && t.artist == "Artist" && t.key == "8A" && t.bpm == 128.5);
            CHECK(t.beatsVersion == "BeatGrid-2.0" && t.beats == std::vector<uint8_t>({1, 2, 3, 250}));
        }
        CHECK(failed.count(0xA) == 1 && failed[0xA] == "too large");
    }

    std::puts("sender refuses files over its own limit and tracks it does not share");
    follower.request(0xC, 0); // 3 MB: leader allows 8 MB, follower allows 2.5 MB -> refused on receive
    follower.request(0xD, 0); // not shared at all
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return failed.count(0xC) && failed.count(0xD); }, 10000));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(failed[0xD] == "not shared");
        CHECK(!received.count(0xC));
    }

    std::puts("a file that isn't the audio it claims to be is refused and removed");
    follower.request(0xE, 0);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return failed.count(0xE) == 1; }, 10000));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(failed[0xE] == "not an audio file" && !received.count(0xE));
    }

    std::puts("no partial files left behind");
    bool partLeft = false;
    bool fakeLeft = false;
    for (const auto& e : fs::recursive_directory_iterator(fc.cacheDir)) {
        if (e.path().extension() == ".part") {
            partLeft = true;
        }
        if (e.path().extension() == ".mp3" && e.file_size() == fake.size()) {
            fakeLeft = true;
        }
    }
    CHECK(!partLeft && !fakeLeft);

    std::puts("reconnect: a follower restart re-requests what it still wants");
    follower.stop();
    FileShareConfig fc2 = fc;
    fc2.maxFileBytes = 64 * 1024 * 1024;
    FileShare follower2(fc2, fcb);
    follower2.start();
    follower2.request(0xA, 0);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return received.count(0xA) == 1; }, 15000));
    {
        std::lock_guard<std::mutex> l(m);
        if (received.count(0xA)) {
            CHECK(readAll(received[0xA]) == big);
        }
    }
    std::printf("  leader sent %llu bytes in %u files\n",
                static_cast<unsigned long long>(leader.stats().bytesSent.load()), leader.stats().filesSent.load());

    follower2.stop();
    leader.stop();
    fs::remove_all(root);
}

// Over the session's own stream (KCP inside the session's UDP traffic), on a
// bad simulated network, both directions at once, while control events flow.
void testSession() {
    std::puts("\n=== over the session stream (20 ms delay, 5 ms jitter, 3% loss each way) ===");
    const fs::path root = fs::temp_directory_path() / "b2b_file_share_session_test";
    fs::remove_all(root);
    fs::create_directories(root / "host-music");
    fs::create_directories(root / "guest-music");
    const auto hostTrack = withHeader(randomBytes(5 * 1024 * 1024 + 77, 11), "ID3");
    const auto guestTrack = withHeader(randomBytes(3 * 1024 * 1024 + 5, 12), "fLaC");
    const std::string hostPath = (root / "host-music" / "host.mp3").string();
    const std::string guestPath = (root / "guest-music" / "guest.flac").string();
    std::ofstream(hostPath, std::ios::binary).write(hostTrack.data(), static_cast<std::streamsize>(hostTrack.size()));
    std::ofstream(guestPath, std::ios::binary).write(guestTrack.data(), static_cast<std::streamsize>(guestTrack.size()));

    NetSimConfig sim;
    sim.delayMs = 20;
    sim.jitterMs = 5;
    sim.lossPct = 3;
    LocalClock hostClock;
    LocalClock guestClock(5 * kNanosPerSec, 20.0);
    SessionConfig hc;
    hc.role = Role::Leader;
    hc.localPort = 9561;
    hc.sim = sim;
    hc.sim.seed = 1;
    SessionConfig gc;
    gc.role = Role::Follower;
    gc.peerHost = "127.0.0.1";
    gc.peerPort = 9561;
    gc.sim = sim;
    gc.sim.seed = 2;
    auto host = std::make_unique<Session>(hc, hostClock);
    auto guest = std::make_unique<Session>(gc, guestClock);
    CHECK(host->start() && guest->start());

    std::mutex m;
    std::map<std::string, std::string> received; // "host"/"guest" -> path
    auto makeShare = [&](Session* s, Role role, const std::string& who, uint64_t ownId, const std::string& ownPath) {
        FileShareConfig c;
        c.session = s;
        c.role = role;
        c.cacheDir = (root / (who + "-cache")).string();
        c.rateBytesPerSec = 1024 * 1024;
        FileShare::Callbacks cb;
        cb.lookup = [ownId, ownPath](uint64_t id, std::string& path) {
            if (id != ownId) {
                return false;
            }
            path = ownPath;
            return true;
        };
        cb.onReceived = [&, who](uint64_t, std::string path) {
            std::lock_guard<std::mutex> lock(m);
            received[who] = path;
        };
        auto share = std::make_unique<FileShare>(c, cb);
        share->start();
        return share;
    };
    auto hostShare = makeShare(host.get(), Role::Leader, "host", 0x1, hostPath);
    auto guestShare = makeShare(guest.get(), Role::Follower, "guest", 0x2, guestPath);

    std::puts("connects once the session does");
    CHECK(waitFor([&] { return hostShare->connected() && guestShare->connected(); }, 5000));

    std::puts("both directions at once, intact, while events keep flowing");
    const auto t0 = std::chrono::steady_clock::now();
    guestShare->request(0x1, 0);
    hostShare->request(0x2, 0);
    uint32_t seq = 0;
    ControlEvent ev;
    int eventsIn = 0;
    const bool done = waitFor([&] {
        ev.seq = ++seq;
        ev.time = host->sharedNow();
        ev.key.assign("[Channel1],volume");
        ev.value = seq;
        host->sendEvent(ev);
        ControlEvent got;
        while (guest->popReceivedEvent(got)) {
            eventsIn++;
        }
        std::lock_guard<std::mutex> lock(m);
        return received.size() == 2;
    }, 30000);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(done);
    {
        std::lock_guard<std::mutex> lock(m);
        if (received.count("guest")) {
            CHECK(readAll(received["guest"]) == hostTrack);
        }
        if (received.count("host")) {
            CHECK(readAll(received["host"]) == guestTrack);
        }
    }
    std::printf("  %.1f MB each way in %.1f s; %d of %u events delivered meanwhile, rtt p90 %.0f ms\n",
                hostTrack.size() / 1048576.0, secs, eventsIn, seq, toMs(guest->rttP90()));
    // Lost in-between values of the same control aren't resent (a newer one
    // supersedes them), but the latest value always arrives.
    double lastValue = 0;
    CHECK(waitFor([&] {
        ControlEvent got;
        while (guest->popReceivedEvent(got)) {
            eventsIn++;
            lastValue = got.value;
        }
        return lastValue == static_cast<double>(seq);
    }, 5000));

    std::puts("guest restarts: a new stream, and it asks again");
    guestShare.reset();
    guest.reset();
    {
        std::lock_guard<std::mutex> lock(m);
        received.erase("guest");
    }
    fs::remove_all(root / "guest-cache");
    guest = std::make_unique<Session>(gc, guestClock);
    CHECK(guest->start());
    guestShare = makeShare(guest.get(), Role::Follower, "guest", 0x2, guestPath);
    guestShare->request(0x1, 0);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> lock(m); return received.count("guest") == 1; }, 30000));
    {
        std::lock_guard<std::mutex> lock(m);
        if (received.count("guest")) {
            CHECK(readAll(received["guest"]) == hostTrack);
        }
    }
    guestShare.reset();
    hostShare.reset();
    guest.reset();
    host.reset();
    fs::remove_all(root);
}


void testFileNames() {
    std::puts("=== safe file names ===");
    CHECK(safeFileName("Artist - Title", "x") == "Artist - Title");
    CHECK(safeFileName("../../etc/passwd", "x") == "etc passwd");
    CHECK(safeFileName("a\\b:c*d?e\"f<g>h|i", "x") == "a b c d e f g h i");
    CHECK(safeFileName("  ...  ", "fallback") == "fallback");
    CHECK(safeFileName("", "fallback") == "fallback");
    CHECK(safeFileName("CON", "fallback") == "fallback");
    CHECK(safeFileName("Track.", "x") == "Track");
    CHECK(safeFileName(std::string("tab\there\nnow"), "x") == "tab here now");
    const std::string longName(200, 'a');
    CHECK(safeFileName(longName, "x").size() == 80);
    // 79 ASCII + a 2-byte character must not be cut in half
    const std::string utf = std::string(79, 'a') + "\xC3\xA9" + "bbb";
    CHECK(safeFileName(utf, "x") == std::string(79, 'a'));
    CHECK(safeFileName("Beyonc\xC3\xA9 - Halo", "x") == "Beyonc\xC3\xA9 - Halo");
}

int main() {
    testFileNames();
    testTcp();
    testSession();
    if (gFailures == 0) {
        std::puts("\nAll file share tests passed.");
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", gFailures);
    return 1;
}
