// SPDX-License-Identifier: GPL-2.0-or-later
//
// Join-code sessions against a real rendezvous server (b2b-server).
//
//   b2b_rendezvous_tests 127.0.0.1 7799
//
// tests/rendezvous_test.sh starts a local server and runs this.

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <variant>

#include "b2b/file_share.hpp"
#include "b2b/session.hpp"

using namespace b2b;
using State = RendezvousStatus::State;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

bool waitFor(const std::function<bool()>& done, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 10) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

std::string server;
uint16_t serverPort = 0;

SessionConfig hostConfig() {
    SessionConfig c;
    c.role = Role::Leader;
    c.rendezvousHost = server;
    c.rendezvousPort = serverPort;
    c.startInNs = 500 * kNanosPerMs;
    return c;
}

// The guest gets the code and the host's session secret (through the room).
SessionConfig guestConfig(const std::string& code, const std::string& secret) {
    SessionConfig c;
    c.role = Role::Follower;
    c.rendezvousHost = server;
    c.rendezvousPort = serverPort;
    c.joinCode = code;
    c.password = secret;
    return c;
}

void testJoinByCode() {
    std::puts("join by code, events both ways, strangers ignored");
    LocalClock hostClock;
    LocalClock guestClock(7 * kNanosPerSec, 30.0); // a different "machine"
    Session host(hostConfig(), hostClock);
    CHECK(host.start());
    CHECK(waitFor([&] { return host.rendezvousStatus().state == State::Waiting; }, 3000));
    const RendezvousStatus hs = host.rendezvousStatus();
    std::printf("  code %s, public %s\n", hs.code.c_str(), hs.publicAddress.c_str());
    CHECK(looksLikeJoinCode(hs.code));
    CHECK(!hs.publicAddress.empty());
    CHECK(normalisePassword(hs.secret).size() == 12); // the session secret, for the guest

    // Typed sloppily, as people will.
    std::string typed;
    for (char ch : hs.code) {
        typed += ch == '-' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    Session guest(guestConfig(typed, hs.secret), guestClock);
    CHECK(guest.start());
    CHECK(waitFor([&] {
        return host.rendezvousStatus().state == State::Connected &&
               guest.rendezvousStatus().state == State::Connected && guest.ready();
    }, 5000));
    std::printf("  host sees guest at %s, guest sees host at %s\n", host.peerAddress().c_str(),
                guest.peerAddress().c_str());
    // The same check code on both screens.
    std::printf("  check code %s\n", host.rendezvousStatus().checkCode.c_str());
    CHECK(host.rendezvousStatus().checkCode.size() == 7);
    CHECK(host.rendezvousStatus().checkCode == guest.rendezvousStatus().checkCode);

    ControlEvent ev;
    ev.seq = 1;
    ev.time = host.sharedNow();
    ev.key.assign("[Channel1],volume");
    ev.value = 0.5;
    CHECK(host.sendEvent(ev));
    ev.key.assign("[Channel2],volume");
    ev.value = 0.25;
    CHECK(guest.sendEvent(ev));
    ControlEvent got;
    CHECK(waitFor([&] { return guest.popReceivedEvent(got); }, 2000));
    CHECK(got.key == "[Channel1],volume" && got.value == 0.5);
    CHECK(waitFor([&] { return host.popReceivedEvent(got); }, 2000));
    CHECK(got.key == "[Channel2],volume" && got.value == 0.25);

    // Someone else who learned the host's port can't barge in.
    UdpSocket stranger;
    CHECK(stranger.bind(0));
    Endpoint hostEp;
    CHECK(UdpSocket::resolve("127.0.0.1", host.localPort(), hostEp));
    const auto ping = encode(Message{PingMsg{99, 1}});
    stranger.sendTo(hostEp, ping.data(), ping.size());
    uint8_t buf[256];
    Endpoint from;
    CHECK(stranger.recvFrom(buf, sizeof(buf), from, 300) <= 0);
    CHECK(host.rendezvousStatus().state == State::Connected);

    // ...nor by saying hello with its own key: the server introduced someone else.
    HelloMsg fake;
    fake.role = 1;
    fake.publicKey.fill(7);
    const auto hello = encodeHello(fake);
    const uint64_t refusedBefore = host.stats().hellosRefused.load();
    stranger.sendTo(hostEp, hello.data(), hello.size());
    CHECK(waitFor([&] { return host.stats().hellosRefused.load() > refusedBefore; }, 2000));
    CHECK(stranger.recvFrom(buf, sizeof(buf), from, 300) <= 0); // no answer either
    CHECK(host.encrypted() && guest.encrypted());

    guest.stop();
    host.stop();
}

void testRelay() {
    std::puts("through the server's relay (as behind strict NAT)");
    LocalClock hostClock;
    LocalClock guestClock(-3 * kNanosPerSec, -40.0);
    SessionConfig hc = hostConfig();
    hc.relayOnly = true;
    Session host(hc, hostClock);
    CHECK(host.start());
    CHECK(waitFor([&] { return host.rendezvousStatus().state == State::Waiting; }, 3000));
    SessionConfig gc = guestConfig(host.rendezvousStatus().code, host.rendezvousStatus().secret);
    gc.relayOnly = true;
    Session guest(gc, guestClock);
    CHECK(guest.start());
    CHECK(waitFor([&] {
        return host.rendezvousStatus().state == State::Connected &&
               guest.rendezvousStatus().state == State::Connected && guest.ready();
    }, 5000));
    CHECK(host.rendezvousStatus().relayed && guest.rendezvousStatus().relayed);
    CHECK(host.relayed() && guest.relayed()); // (what FileShare's relay cap goes by)

    for (uint32_t i = 1; i <= 50; ++i) {
        ControlEvent ev;
        ev.seq = i;
        ev.time = host.sharedNow();
        ev.key.assign("[Channel1],rate");
        ev.value = i;
        CHECK(host.sendEvent(ev));
        ev.key.assign("[Channel2],rate");
        CHECK(guest.sendEvent(ev));
    }
    int toGuest = 0;
    int toHost = 0;
    ControlEvent got;
    waitFor([&] {
        while (guest.popReceivedEvent(got)) {
            toGuest++;
        }
        while (host.popReceivedEvent(got)) {
            toHost++;
        }
        return toGuest == 50 && toHost == 50;
    }, 3000);
    CHECK(toGuest == 50 && toHost == 50);
    // Clock sync works through the relay too.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const double errMs = toMs(guest.sharedFromLocal(guestClock.now()) - hostClock.now());
    std::printf("  50 events each way; guest clock error %.3f ms, rtt p90 %.2f ms\n", errMs,
                toMs(guest.rttP90()));
    CHECK(std::abs(errMs) < 2.0);

    // The bulk stream (track sharing) through the relay: 1 MB host -> guest.
    CHECK(waitFor([&] { return host.streamEpoch() != 0; }, 2000));
    std::vector<uint8_t> blob(1024 * 1024);
    for (size_t i = 0; i < blob.size(); ++i) {
        blob[i] = static_cast<uint8_t>(i * 2654435761u >> 13);
    }
    const uint32_t hostEpoch = host.streamEpoch();
    size_t sent = 0;
    std::vector<uint8_t> got2;
    uint8_t buf[65536];
    const auto start = std::chrono::steady_clock::now();
    waitFor([&] {
        if (sent < blob.size()) {
            const int n = host.streamSend(hostEpoch, blob.data() + sent, blob.size() - sent);
            if (n > 0) {
                sent += static_cast<size_t>(n);
            }
        }
        const uint32_t ge = guest.streamEpoch();
        for (int n; ge != 0 && (n = guest.streamRecv(ge, buf, sizeof(buf))) > 0;) {
            got2.insert(got2.end(), buf, buf + n);
        }
        return got2.size() >= blob.size();
    }, 20000);
    CHECK(got2 == blob);
    std::printf("  1 MB through the relay in %.2f s\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());

    // Tracks both ways at once through the relay: FileShare keeps each side
    // to its relay cap, so the two together stay under the relay's limit
    // (the server would drop the excess, sync packets included).
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "b2b_rendezvous_relay_share";
    fs::remove_all(root);
    fs::create_directories(root);
    std::vector<char> track(6 * 1024 * 1024);
    for (size_t i = 0; i < track.size(); ++i) {
        track[i] = static_cast<char>(i * 2654435761u >> 11);
    }
    std::memcpy(track.data(), "ID3", 3);
    const std::string trackPath = (root / "t.mp3").string();
    std::ofstream(trackPath, std::ios::binary).write(track.data(), static_cast<std::streamsize>(track.size()));
    std::mutex m;
    int arrived = 0;
    auto share = [&](Session* s, Role role, const char* who) {
        FileShareConfig c;
        c.session = s;
        c.role = role;
        c.cacheDir = (root / who).string();
        FileShare::Callbacks cb;
        cb.lookup = [&](uint64_t id, std::string& path) {
            path = trackPath;
            return id == 7;
        };
        cb.onReceived = [&](uint64_t, std::string path) {
            std::lock_guard<std::mutex> l(m);
            arrived += fs::file_size(path) == track.size() ? 1 : 0;
        };
        auto f = std::make_unique<FileShare>(c, cb);
        f->start();
        return f;
    };
    auto hostShare = share(&host, Role::Leader, "host");
    auto guestShare = share(&guest, Role::Follower, "guest");
    CHECK(waitFor([&] { return hostShare->connected() && guestShare->connected(); }, 5000));
    const auto shareStart = std::chrono::steady_clock::now();
    hostShare->request(7, 0);
    guestShare->request(7, 0);
    CHECK(waitFor([&] { std::lock_guard<std::mutex> l(m); return arrived == 2; }, 30000));
    const double shareSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - shareStart).count();
    std::printf("  6 MB each way at once through the relay in %.1f s\n", shareSecs);
    CHECK(shareSecs > 3.0); // ~1.5 MB/s each: not faster than the cap allows
    guestShare.reset();
    hostShare.reset();
    fs::remove_all(root);
    guest.stop();
    host.stop();
}

void testWrongCode() {
    std::puts("wrong code is reported, not retried");
    LocalClock clock;
    Session guest(guestConfig("NOPE-NOPE-11", "ABCD-EFGH-JKLM"), clock);
    CHECK(guest.start());
    CHECK(waitFor([&] { return guest.rendezvousStatus().state == State::Failed; }, 3000));
    const auto s = guest.rendezvousStatus();
    std::printf("  \"%s\"\n", s.error.c_str());
    CHECK(!s.retrying);
    CHECK(s.error.find("NOPE-NOPE-11") != std::string::npos);
    guest.stop();
}

void testNoServer() {
    std::puts("no server answering: tells the user, keeps trying");
    SessionConfig c = hostConfig();
    c.rendezvousPort = 9; // nothing there
    LocalClock clock;
    Session host(c, clock);
    CHECK(host.start());
    CHECK(host.rendezvousStatus().state == State::Contacting);
    CHECK(waitFor([&] { return host.rendezvousStatus().state == State::Failed; }, 10000));
    CHECK(host.rendezvousStatus().retrying);
    host.stop();
}

void testCodeFreedOnLeave() {
    std::puts("leaving frees the code");
    LocalClock clock;
    auto host = std::make_unique<Session>(hostConfig(), clock);
    CHECK(host->start());
    CHECK(waitFor([&] { return host->rendezvousStatus().state == State::Waiting; }, 3000));
    const std::string code = host->rendezvousStatus().code;
    const std::string secret = host->rendezvousStatus().secret;
    host.reset();
    Session guest(guestConfig(code, secret), clock);
    CHECK(guest.start());
    CHECK(waitFor([&] { return guest.rendezvousStatus().state == State::Failed; }, 3000));
    guest.stop();
}

void testCodeWithoutSecret() {
    std::puts("the code without the session secret gets nobody in");
    LocalClock clock;
    Session host(hostConfig(), clock);
    CHECK(host.start());
    CHECK(waitFor([&] { return host.rendezvousStatus().state == State::Waiting; }, 3000));
    const RendezvousStatus hs = host.rendezvousStatus();
    // No secret at all: can't even start.
    Session bare(guestConfig(hs.code, ""), clock);
    CHECK(!bare.start());
    // A wrong one: the server says the code is unknown, and the host hears of nobody.
    Session guest(guestConfig(hs.code, "WRNG-WRNG-WRNG"), clock);
    CHECK(guest.start());
    CHECK(waitFor([&] { return guest.rendezvousStatus().state == State::Failed; }, 3000));
    CHECK(guest.rendezvousStatus().error.find(hs.code) != std::string::npos);
    CHECK(host.rendezvousStatus().state == State::Waiting);
    guest.stop();
    host.stop();
}

void testConnectedGuestKeepsItsPlace() {
    std::puts("a connected guest keeps its place: a second one with the code and secret doesn't take it");
    LocalClock hostClock;
    LocalClock guestClock(2 * kNanosPerSec, 10.0);
    Session host(hostConfig(), hostClock);
    CHECK(host.start());
    CHECK(waitFor([&] { return host.rendezvousStatus().state == State::Waiting; }, 3000));
    const RendezvousStatus hs = host.rendezvousStatus();
    Session guest(guestConfig(hs.code, hs.secret), guestClock);
    CHECK(guest.start());
    CHECK(waitFor([&] {
        return host.rendezvousStatus().state == State::Connected &&
               guest.rendezvousStatus().state == State::Connected && guest.ready();
    }, 5000));
    const std::string first = host.peerAddress();
    // Someone who had the room's code and secret (a DJ taken off since).
    Session other(guestConfig(hs.code, hs.secret), guestClock);
    CHECK(other.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(host.peerAddress() == first);
    CHECK(other.rendezvousStatus().state != State::Connected || !other.ready());
    // The first guest still gets the host's events.
    ControlEvent ev;
    ev.seq = 7;
    ev.time = host.sharedNow();
    ev.key.assign("[Channel1],rate");
    ev.value = 0.1;
    CHECK(host.sendEvent(ev));
    ControlEvent got;
    CHECK(waitFor([&] { return guest.popReceivedEvent(got); }, 2000));
    CHECK(got.key == "[Channel1],rate");
    other.stop();
    guest.stop();
    host.stop();
}

void testMalformedPeerDoesntCrash() {
    std::puts("a malformed PEER from the server is ignored, not a crash");
    // A stand-in server: answers HOST with a code, then sends PEER whose
    // first address doesn't parse but a later one does.
    UdpSocket fake;
    CHECK(fake.bind(0));
    SessionConfig c = hostConfig();
    c.rendezvousHost = "127.0.0.1";
    c.rendezvousPort = fake.localPort();
    LocalClock clock;
    Session host(c, clock);
    CHECK(host.start());
    uint8_t buf[600];
    Endpoint from;
    std::string nonce;
    for (int i = 0; i < 20 && nonce.empty(); ++i) {
        const int n = fake.recvFrom(buf, sizeof(buf), from, 200);
        const std::string text(reinterpret_cast<char*>(buf), n > 0 ? static_cast<size_t>(n) : 0);
        if (text.rfind("B2BR1 HOST ", 0) == 0) {
            nonce = text.substr(11, text.find(' ', 11) - 11);
        }
    }
    CHECK(!nonce.empty());
    auto say = [&](const std::string& text) {
        fake.sendTo(from, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    };
    say("B2BR1 CODE " + nonce + " TEST-CODE-11 127.0.0.1:1 0011223344556677");
    CHECK(waitFor([&] { return host.rendezvousStatus().state == State::Waiting; }, 3000));
    say("B2BR1 PEER TEST-CODE-11 junk 10.0.0.1:5,10.0.0.2:6 0011223344556677 " + std::string(64, 'a'));
    // ...and one for another code, which must change nothing.
    say("B2BR1 PEER OTHER-CODE-12 10.9.9.9:5 - 0011223344556677 " + std::string(64, 'b'));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(host.rendezvousStatus().state == State::Punching || host.rendezvousStatus().state == State::Waiting);
    host.stop();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: b2b_rendezvous_tests <server> <port>");
        return 2;
    }
    server = argv[1];
    serverPort = static_cast<uint16_t>(std::atoi(argv[2]));
    testJoinByCode();
    testRelay();
    testWrongCode();
    testCodeFreedOnLeave();
    testCodeWithoutSecret();
    testConnectedGuestKeepsItsPlace();
    testMalformedPeerDoesntCrash();
    testNoServer();
    if (gFailures == 0) {
        std::puts("all rendezvous tests passed");
        return 0;
    }
    std::printf("%d failure(s)\n", gFailures);
    return 1;
}
