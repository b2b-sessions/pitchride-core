// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the core. No framework, just run the binary (or `ctest`).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <variant>

#include "b2b/clock_sync.hpp"
#include "b2b/crypto.hpp"
#include "b2b/fixed_string.hpp"
#include "b2b/protocol.hpp"
#include "b2b/session.hpp"
#include "b2b/spsc_queue.hpp"
#include "b2b/time.hpp"
#include "b2b/timeline.hpp"

using namespace b2b;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

void testProtocolRoundTrip() {
    std::puts("protocol round trip");
    {
        PongMsg in{42, -123456789012345, 987654321, 987654999};
        auto bytes = encode(in);
        auto out = decode(bytes.data(), bytes.size());
        CHECK(out.has_value());
        auto* p = std::get_if<PongMsg>(&*out);
        CHECK(p && p->id == 42 && p->t1 == in.t1 && p->t2 == in.t2 && p->t3 == in.t3);
    }
    {
        EventMsg in{7, 123456789012, "[Channel1],playposition", 0.123456789012345, "a1b2c3"};
        auto bytes = encode(in);
        auto out = decode(bytes.data(), bytes.size());
        auto* e = out ? std::get_if<EventMsg>(&*out) : nullptr;
        CHECK(e && e->seq == 7 && e->time == in.time && e->key == in.key && e->value == in.value &&
              e->text == in.text);
    }
    {
        DeckSnapshotMsg in{987654321, 2, 1, 93.5000123, 1.0625, 0xfeedfacecafebeefULL};
        auto bytes = encode(in);
        auto out = decode(bytes.data(), bytes.size());
        auto* d = out ? std::get_if<DeckSnapshotMsg>(&*out) : nullptr;
        CHECK(d && d->time == in.time && d->deck == 2 && d->playing == 1 && d->positionSec == in.positionSec &&
              d->rate == in.rate && d->trackId == in.trackId);
    }
    {
        StartMsg in{1'000'000'000'000, 127.5, 150'000'000};
        auto bytes = encode(in);
        auto out = decode(bytes.data(), bytes.size());
        auto* s = out ? std::get_if<StartMsg>(&*out) : nullptr;
        CHECK(s && s->t0 == in.t0 && s->bpm == 127.5 && s->delayNs == in.delayNs);
    }
}

void testProtocolRejectsGarbage() {
    std::puts("protocol rejects garbage");
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7};
    CHECK(!decode(junk, sizeof(junk)).has_value());
    auto bytes = encode(EventMsg{1, 2, "[Master],crossfader", 1.0, ""});
    CHECK(!decode(bytes.data(), bytes.size() - 1).has_value()); // truncated
    bytes.push_back(0);
    CHECK(!decode(bytes.data(), bytes.size()).has_value()); // trailing byte
}

// Simulate ping/pong exchanges over a network with jitter and check that the
// offset estimate converges on the true offset. `driftPpm` makes the leader's
// clock run fast, so the true offset grows over time.
double simulateClockSync(Nanos initialOffset, double forwardMs, double backMs, double jitterMs, int pings,
                         double driftPpm = 0.0, double* estimatedDrift = nullptr) {
    std::mt19937 rng(1234);
    std::exponential_distribution<double> jitter(1.0 / jitterMs);
    ClockSync sync;
    const Nanos start = 5 * kNanosPerSec;
    auto offsetAt = [&](Nanos local) {
        return initialOffset + static_cast<Nanos>(static_cast<double>(local - start) * driftPpm * 1e-6);
    };
    Nanos local = start;
    for (int i = 0; i < pings; ++i) {
        const Nanos t1 = local;
        const Nanos arriveLocal = t1 + fromMs(forwardMs + jitter(rng));
        const Nanos t2 = arriveLocal + offsetAt(arriveLocal);
        const Nanos t3 = t2 + 50'000; // 50 us processing
        const Nanos sendLocal = arriveLocal + 50'000;
        const Nanos t4 = sendLocal + fromMs(backMs + jitter(rng));
        sync.addSample(t1, t2, t3, t4);
        local = t4 + 100 * kNanosPerMs;
    }
    CHECK(sync.hasEstimate());
    if (estimatedDrift) {
        *estimatedDrift = sync.driftPpm();
    }
    return toMs(sync.offset() - offsetAt(local - 100 * kNanosPerMs));
}

void testClockSyncDrift() {
    std::puts("clock sync: clocks drifting apart at 60 ppm");
    double drift = 0;
    const double err = simulateClockSync(fromMs(10.0), 15.0, 15.0, 4.0, 400, 60.0, &drift);
    std::printf("  estimate error %.3f ms, estimated drift %.1f ppm\n", err, drift);
    CHECK(std::fabs(err) < 0.2);
    CHECK(std::fabs(drift - 60.0) < 20.0);
}

void testClockSyncSymmetric() {
    std::puts("clock sync: symmetric path with heavy jitter");
    const double err = simulateClockSync(fromMs(123.456), 20.0, 20.0, 15.0, 200);
    std::printf("  estimate error %.3f ms\n", err);
    CHECK(std::fabs(err) < 1.0);
}

void testClockSyncAsymmetric() {
    std::puts("clock sync: asymmetric path (documents the known limitation)");
    // 30 ms there, 10 ms back: expected error is about half the difference = 10 ms.
    const double err = simulateClockSync(fromMs(-50.0), 30.0, 10.0, 2.0, 200);
    std::printf("  estimate error %.3f ms (expected ~10 ms; harmless for musical accuracy)\n", err);
    CHECK(std::fabs(std::fabs(err) - 10.0) < 1.5);
}

void testTimeline() {
    std::puts("timeline maths");
    Timeline tl;
    tl.t0 = 10 * kNanosPerSec;
    tl.bpm = 120.0; // 0.5 s per beat
    CHECK(std::fabs(tl.beatAt(tl.t0) - 0.0) < 1e-9);
    CHECK(std::fabs(tl.beatAt(tl.t0 + kNanosPerSec) - 2.0) < 1e-9);
    CHECK(std::llabs(tl.timeAtBeat(64.0) - (tl.t0 + 32 * kNanosPerSec)) <= 1);
    CHECK(std::fabs(tl.beatAt(tl.timeAtBeat(33.75)) - 33.75) < 1e-6);
}

void testCrypto() {
    std::puts("encryption: round trip, wrong secret, tampering, replays, reordering");
    using namespace b2b::crypto;
    const KeyPair host = KeyPair::generate();
    const KeyPair guest = KeyPair::generate();
    CHECK(host.publicKey != guest.publicKey);
    Key back{};
    CHECK(fromHex(toHex(host.publicKey), back) && back == host.publicKey);
    CHECK(!fromHex("zz", back));

    Channel h, g, wrong;
    CHECK(h.init(host, guest.publicKey, true, "BASS-TIGER-42"));
    CHECK(g.init(guest, host.publicKey, false, "BASS-TIGER-42"));
    CHECK(wrong.init(guest, host.publicKey, false, "BASS-TIGER-43"));
    const std::string text = "[Channel1],volume = 0.5";
    const auto* p = reinterpret_cast<const uint8_t*>(text.data());

    auto a = h.seal(0x34423242, 0xF1, p, text.size());
    CHECK(a.size() == text.size() + kSealOverhead);
    CHECK(std::search(a.begin(), a.end(), text.begin(), text.end()) == a.end()); // not readable
    std::vector<uint8_t> out;
    CHECK(g.open(a.data(), a.size(), out) && std::string(out.begin(), out.end()) == text);
    CHECK(!g.open(a.data(), a.size(), out)); // replay
    auto b = h.seal(0x34423242, 0xF1, p, text.size());
    CHECK(!wrong.open(b.data(), b.size(), out)); // wrong code/password
    auto c = h.seal(0x34423242, 0xF1, p, text.size());
    c.back() ^= 1;
    CHECK(!g.open(c.data(), c.size(), out)); // tampered
    auto d = h.seal(0x34423242, 0xF1, p, text.size());
    d[6] ^= 1;                               // header (counter) is authenticated too
    CHECK(!g.open(d.data(), d.size(), out));

    // Reordering within the window is fine; each packet still only once.
    std::vector<std::vector<uint8_t>> burst;
    for (int i = 0; i < 10; ++i) {
        burst.push_back(h.seal(0x34423242, 0xF1, p, text.size()));
    }
    CHECK(g.open(burst[9].data(), burst[9].size(), out));
    CHECK(g.open(burst[3].data(), burst[3].size(), out));
    CHECK(!g.open(burst[3].data(), burst[3].size(), out));
    CHECK(b.size() > 0 && g.open(b.data(), b.size(), out)); // older, but within the window: accepted once
    CHECK(!g.open(b.data(), b.size(), out));
    // Hundreds behind (a track's burst, reordered): still accepted, once.
    std::vector<std::vector<uint8_t>> many;
    for (int i = 0; i < 1000; ++i) {
        many.push_back(h.seal(0x34423242, 0xF1, p, text.size()));
    }
    CHECK(g.open(many[999].data(), many[999].size(), out));
    CHECK(g.open(many[0].data(), many[0].size(), out));
    CHECK(!g.open(many[0].data(), many[0].size(), out));
    CHECK(g.open(burst[5].data(), burst[5].size(), out)); // ~1000 behind, never seen: fine
    CHECK(!g.open(burst[9].data(), burst[9].size(), out)); // seen before the jump: still refused
    // Far behind the window (8192): refused, even if never seen.
    auto old1 = h.seal(0x34423242, 0xF1, p, text.size());
    for (int i = 0; i < 8200; ++i) {
        auto x = h.seal(0x34423242, 0xF1, p, text.size());
        g.open(x.data(), x.size(), out);
    }
    CHECK(!g.open(old1.data(), old1.size(), out));
    CHECK(!g.open(many[500].data(), many[500].size(), out));
    // A jump of more than a whole window forgets everything older.
    for (int i = 0; i < 9000; ++i) {
        h.seal(0x34423242, 0xF1, p, text.size());
    }
    auto far1 = h.seal(0x34423242, 0xF1, p, text.size());
    CHECK(g.open(far1.data(), far1.size(), out));
    CHECK(!g.open(far1.data(), far1.size(), out));

    // The other direction uses a different key.
    auto e = g.seal(0x34423242, 0xF1, p, text.size());
    CHECK(h.open(e.data(), e.size(), out));
    auto f = g.seal(0x34423242, 0xF1, p, text.size());
    CHECK(!g.open(f.data(), f.size(), out)); // can't open our own

    // A low-order public key gives no secret: refused.
    Channel bad;
    Key zero{};
    CHECK(!bad.init(host, zero, true, ""));
}

void testJoinCodes() {
    std::puts("join codes");
    CHECK(normaliseJoinCode("bass tiger 42") == "BASS-TIGER-42");
    CHECK(normaliseJoinCode("  Bass_Tiger--42 ") == "BASS-TIGER-42");
    CHECK(looksLikeJoinCode("BASS-TIGER-42"));
    CHECK(looksLikeJoinCode("bass tiger 42"));
    CHECK(!looksLikeJoinCode("192.168.1.20"));
    CHECK(!looksLikeJoinCode("192.168.1.20:9000"));
    CHECK(!looksLikeJoinCode("my-pc.local"));
    CHECK(!looksLikeJoinCode("12-34"));
    CHECK(!looksLikeJoinCode(""));
    const std::string pw = makeSessionPassword();
    CHECK(pw.size() == 14 && pw[4] == '-' && pw[9] == '-');
    CHECK(pw != makeSessionPassword());
    CHECK(normalisePassword("kx7p q2md-9tza") == "KX7PQ2MD9TZA");
}

void testFixedString() {
    std::puts("fixed string");
    FixedString<8> a("hello");
    CHECK(a == "hello" && a.size() == 5);
    FixedString<4> b("truncated");
    CHECK(b == "trun" && b.size() == 4);
    FixedString<8> c;
    CHECK(c.empty() && c == "");
}

void testSpscQueue() {
    std::puts("spsc queue");
    SpscQueue<int, 4> q; // holds 3
    CHECK(q.push(1) && q.push(2) && q.push(3));
    CHECK(!q.push(4));
    int v = 0;
    CHECK(q.pop(v) && v == 1);
    CHECK(q.push(4));
    CHECK(q.pop(v) && v == 2);
    CHECK(q.pop(v) && v == 3);
    CHECK(q.pop(v) && v == 4);
    CHECK(!q.pop(v));
}

void testLocalClockInverse() {
    std::puts("local clock fake offset/drift inverse");
    LocalClock c(fromMs(-321.0), 80.0);
    const Nanos t = trueNow() + 37 * kNanosPerSec;
    CHECK(std::llabs(c.toTrue(c.fromTrue(t)) - t) < 1000);
}

} // namespace

int main() {
    testProtocolRoundTrip();
    testProtocolRejectsGarbage();
    testClockSyncSymmetric();
    testClockSyncAsymmetric();
    testClockSyncDrift();
    testTimeline();
    testFixedString();
    testJoinCodes();
    testCrypto();
    testSpscQueue();
    testLocalClockInverse();
    if (gFailures == 0) {
        std::puts("\nAll tests passed.");
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", gFailures);
    return 1;
}
