// SPDX-License-Identifier: GPL-2.0-or-later
//
// b2b_demo: two-peer metronome that proves the sync idea.
//
//   Leader:    b2b_demo leader   --port 9000
//   Follower:  b2b_demo follower --connect 127.0.0.1:9000
//
// Run `b2b_demo --help` for all options.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio_output.hpp"
#include "b2b/engine.hpp"
#include "b2b/event_log.hpp"
#include "b2b/session.hpp"
#include "b2b/time.hpp"

using namespace b2b;

namespace {

std::atomic<bool> gInterrupted{false};

struct Options {
    Role role = Role::Leader;
    uint16_t port = 0;
    std::string connect;
    double bpm = 124.0;
    double delayMs = 150.0;
    double durationSec = 30.0;
    bool audio = false;
    float pan = 0.0f;
    std::string logPath;
    std::string wavPath;
    NetSimConfig sim;
    double fakeOffsetMs = 0.0;
    double fakeDriftPpm = 0.0;
    uint32_t scriptSeed = 7;
};

void printUsage() {
    std::puts(
        "Usage:\n"
        "  b2b_demo leader   [options]          the DJ in control (owns the shared clock)\n"
        "  b2b_demo follower --connect HOST:PORT [options]\n"
        "\n"
        "Options:\n"
        "  --port N             UDP port to listen on (leader default 9000, follower any)\n"
        "  --connect HOST:PORT  leader address (follower only)\n"
        "  --bpm X              tempo (leader, default 124)\n"
        "  --delay-ms X         how far behind the follower plays (leader, default 150)\n"
        "  --duration S         seconds to run (default 30)\n"
        "  --audio              play through your speakers/headphones (needs miniaudio)\n"
        "  --pan X              -1 left .. 1 right (handy: leader left, follower right)\n"
        "  --log FILE           write a CSV of everything that happened (for analyse.py)\n"
        "  --wav FILE           write the rendered audio to a WAV file (headless mode)\n"
        "\n"
        "Fake bad internet (applied to packets this peer sends):\n"
        "  --sim-delay-ms X     fixed one-way delay\n"
        "  --sim-jitter-ms X    random extra delay, mean X\n"
        "  --sim-loss X         percent of packets dropped\n"
        "\n"
        "Pretend to be a different machine (testing on one computer):\n"
        "  --fake-offset-ms X   shift this peer's clock\n"
        "  --fake-drift-ppm X   make this peer's clock run fast/slow\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    if (argc < 2) {
        return false;
    }
    const std::string mode = argv[1];
    if (mode == "leader") {
        o.role = Role::Leader;
        o.port = 9000;
    } else if (mode == "follower") {
        o.role = Role::Follower;
    } else {
        return false;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") o.port = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (a == "--connect") o.connect = next("--connect");
        else if (a == "--bpm") o.bpm = std::atof(next("--bpm"));
        else if (a == "--delay-ms") o.delayMs = std::atof(next("--delay-ms"));
        else if (a == "--duration") o.durationSec = std::atof(next("--duration"));
        else if (a == "--audio") o.audio = true;
        else if (a == "--pan") o.pan = static_cast<float>(std::atof(next("--pan")));
        else if (a == "--log") o.logPath = next("--log");
        else if (a == "--wav") o.wavPath = next("--wav");
        else if (a == "--sim-delay-ms") o.sim.delayMs = std::atof(next("--sim-delay-ms"));
        else if (a == "--sim-jitter-ms") o.sim.jitterMs = std::atof(next("--sim-jitter-ms"));
        else if (a == "--sim-loss") o.sim.lossPct = std::atof(next("--sim-loss"));
        else if (a == "--fake-offset-ms") o.fakeOffsetMs = std::atof(next("--fake-offset-ms"));
        else if (a == "--fake-drift-ppm") o.fakeDriftPpm = std::atof(next("--fake-drift-ppm"));
        else if (a == "--seed") o.scriptSeed = static_cast<uint32_t>(std::atoi(next("--seed")));
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        }
    }
    if (o.role == Role::Follower && o.connect.find(':') == std::string::npos) {
        std::fprintf(stderr, "follower needs --connect HOST:PORT\n");
        return false;
    }
    return true;
}

// The leader's "DJ": quantised mutes plus off-grid moves at random moments,
// to show that any timing survives the trip, not just neat beat numbers.
std::vector<ScriptAction> makeScript(double totalBeats, uint32_t seed) {
    std::vector<ScriptAction> script;
    bool aOn = true;
    bool bOn = true;
    for (double beat = 16; beat < totalBeats; beat += 16) {
        aOn = !aOn;
        script.push_back({beat, kChannelAGain, aOn ? 1.0f : 0.0f});
        bOn = !bOn;
        script.push_back({beat + 8, kChannelBGain, bOn ? 1.0f : 0.0f});
    }
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> gap(2.3, 5.1);
    std::uniform_real_distribution<float> value(0.2f, 1.0f);
    std::uniform_int_distribution<int> control(0, 1);
    for (double beat = 12.37; beat < totalBeats; beat += gap(rng)) {
        script.push_back({beat, static_cast<uint16_t>(control(rng)), value(rng)});
    }
    return script;
}

bool writeWav(const std::string& path, const std::vector<float>& samples, int sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);                       // PCM
    u16(2);                       // stereo
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate * 4));
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    for (float s : samples) {
        const auto v = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, s)) * 32767.0f));
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        printUsage();
        return 0;
    }
    if (!parseArgs(argc, argv, opt)) {
        printUsage();
        return 2;
    }
    std::signal(SIGINT, [](int) { gInterrupted = true; });

    const bool leader = opt.role == Role::Leader;
    const char* name = leader ? "leader" : "follower";
    constexpr int kSampleRate = 48000;
    constexpr int kBlock = 256;

    LocalClock clock(fromMs(opt.fakeOffsetMs), opt.fakeDriftPpm);

    SessionConfig sc;
    sc.role = opt.role;
    sc.localPort = opt.port;
    sc.bpm = opt.bpm;
    sc.delayNs = fromMs(opt.delayMs);
    sc.sim = opt.sim;
    if (!leader) {
        const auto colon = opt.connect.rfind(':');
        sc.peerHost = opt.connect.substr(0, colon);
        sc.peerPort = static_cast<uint16_t>(std::atoi(opt.connect.c_str() + colon + 1));
    }

    EventLog log;
    log.setHeader("role", name);
    log.setHeader("fake_offset_ns", std::to_string(clock.fakeOffset()));
    log.setHeader("fake_drift_ppm", std::to_string(clock.fakeDriftPpm()));
    log.setHeader("origin_ns", std::to_string(clock.origin()));
    log.setHeader("sample_rate", std::to_string(kSampleRate));
    log.setHeader("sim_delay_ms", std::to_string(opt.sim.delayMs));
    log.setHeader("sim_jitter_ms", std::to_string(opt.sim.jitterMs));
    log.setHeader("sim_loss_pct", std::to_string(opt.sim.lossPct));

    Session session(sc, clock);
    if (!session.start()) {
        std::fprintf(stderr, "[%s] could not start (port in use or bad address?)\n", name);
        return 1;
    }

    Engine engine(session, log, EngineConfig{kSampleRate, opt.pan});
    if (leader) {
        engine.setScript(makeScript(opt.bpm * opt.durationSec / 60.0, opt.scriptSeed));
        std::printf("[%s] listening on UDP port %u, %.1f BPM, follower delay %.0f ms\n", name, opt.port,
                    opt.bpm, opt.delayMs);
    } else {
        std::printf("[%s] connecting to %s\n", name, opt.connect.c_str());
    }
    if (opt.sim.enabled()) {
        std::printf("[%s] fake network: %.0f ms delay, %.0f ms jitter, %.1f%% loss\n", name, opt.sim.delayMs,
                    opt.sim.jitterMs, opt.sim.lossPct);
    }

    std::atomic<bool> rendering{true};
    std::vector<float> wav;
    std::thread renderThread;
    AudioOutput audio;

    if (opt.audio) {
        if (!AudioOutput::available()) {
            std::fprintf(stderr, "[%s] built without audio support; run tools/fetch_miniaudio.sh and rebuild\n",
                         name);
            return 1;
        }
        // Drive the engine from the sound card. Sample time comes from the
        // sound card's own clock (smooth); we only nudge it towards the system
        // clock slowly, so callback timing jitter never reaches the music.
        struct AudioTiming {
            bool started = false;
            Nanos anchorLocal = 0;
            int64_t frames = 0;
        };
        auto timing = std::make_shared<AudioTiming>();
        bool ok = audio.start(kSampleRate, kBlock, [&, timing](float* out, int frames) {
            const Nanos latency = static_cast<Nanos>(audio.latencySeconds() * 1e9);
            const Nanos measured = clock.now() + latency;
            if (!timing->started) {
                timing->anchorLocal = measured;
                timing->started = true;
            }
            Nanos local = timing->anchorLocal + static_cast<Nanos>(timing->frames * 1e9 / kSampleRate);
            timing->anchorLocal += (measured - local) / 64;
            local = timing->anchorLocal + static_cast<Nanos>(timing->frames * 1e9 / kSampleRate);
            engine.render(out, frames, local, clock.toTrue(local));
            timing->frames += frames;
        });
        if (!ok) {
            std::fprintf(stderr, "[%s] could not open audio device\n", name);
            return 1;
        }
        std::printf("[%s] audio output latency ~%.1f ms\n", name, audio.latencySeconds() * 1000.0);
    } else {
        // Headless: a pretend sound card that plays a block exactly every 256 samples.
        renderThread = std::thread([&] {
            const double blockNs = kBlock * 1e9 / kSampleRate;
            const Nanos outputLatency = 20 * kNanosPerMs;
            const Nanos startTrue = trueNow();
            std::vector<float> buffer(kBlock * 2);
            for (int64_t k = 0; rendering; ++k) {
                const Nanos deadline = startTrue + static_cast<Nanos>(k * blockNs);
                std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
                const Nanos playTrue = deadline + outputLatency;
                engine.render(buffer.data(), kBlock, clock.fromTrue(playTrue), playTrue);
                if (!opt.wavPath.empty() && session.ready()) {
                    wav.insert(wav.end(), buffer.begin(), buffer.end());
                }
            }
        });
    }

    // Main thread: status and clock-sync logging.
    const Nanos endAt = trueNow() + static_cast<Nanos>(opt.durationSec * 1e9);
    Nanos nextStatus = trueNow() + 2 * kNanosPerSec;
    bool announcedReady = false;
    while (!gInterrupted && trueNow() < endAt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const ClockSync& cs = session.clockSync();
        if (!leader && cs.hasEstimate()) {
            log.add({'S', cs.sampleCount(), 0, 0, toMs(cs.bestRtt()), trueNow(), static_cast<double>(cs.offset())});
        }
        if (!announcedReady && session.ready()) {
            announcedReady = true;
            std::printf("[%s] ready: timeline shared%s\n", name, leader ? "" : ", clock synced");
        }
        if (trueNow() >= nextStatus) {
            nextStatus += 2 * kNanosPerSec;
            const auto& st = session.stats();
            if (leader) {
                std::printf("[%s] events sent %llu, acked %llu, resent %llu\n", name,
                            static_cast<unsigned long long>(st.eventsSent.load()),
                            static_cast<unsigned long long>(st.eventsAcked.load()),
                            static_cast<unsigned long long>(st.eventResends.load()));
            } else if (cs.hasEstimate()) {
                std::printf("[%s] clock offset %+.3f ms, best round trip %.1f ms, events received %llu, late %llu\n",
                            name, toMs(cs.offset()), toMs(cs.bestRtt()),
                            static_cast<unsigned long long>(st.eventsReceived.load()),
                            static_cast<unsigned long long>(engine.lateEvents()));
            } else {
                std::printf("[%s] waiting for leader...\n", name);
            }
        }
    }

    rendering = false;
    if (renderThread.joinable()) {
        renderThread.join();
    }
    audio.stop();
    session.stop();

    const Timeline tl = session.timeline();
    log.setHeader("t0_shared_ns", std::to_string(tl.t0));
    log.setHeader("bpm", std::to_string(tl.bpm));
    log.setHeader("delay_ns", std::to_string(session.renderDelay()));
    log.setHeader("follower_delay_ns", std::to_string(leader ? sc.delayNs : session.renderDelay()));

    std::printf("[%s] done: %llu events applied, %llu late\n", name,
                static_cast<unsigned long long>(engine.appliedEvents()),
                static_cast<unsigned long long>(engine.lateEvents()));
    if (!opt.logPath.empty()) {
        if (log.writeCsv(opt.logPath)) {
            std::printf("[%s] log written to %s\n", name, opt.logPath.c_str());
        } else {
            std::fprintf(stderr, "[%s] could not write %s\n", name, opt.logPath.c_str());
        }
    }
    if (!opt.wavPath.empty()) {
        if (writeWav(opt.wavPath, wav, kSampleRate)) {
            std::printf("[%s] audio written to %s\n", name, opt.wavPath.c_str());
        }
    }
    return 0;
}
