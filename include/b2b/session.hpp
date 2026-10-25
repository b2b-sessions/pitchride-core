// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "b2b/clock_sync.hpp"
#include "b2b/crypto.hpp"
#include "b2b/fixed_string.hpp"
#include "b2b/net.hpp"
#include "b2b/protocol.hpp"
#include "b2b/spsc_queue.hpp"
#include "b2b/time.hpp"
#include "b2b/timeline.hpp"
#include "b2b/version.hpp"

struct IKCPCB; // KCP (third_party/kcp)

namespace b2b {

// Network roles. The leader ("host") owns the shared clock and waits for the
// follower ("guest") to connect. Who is *live* (whose moves everyone hears)
// is separate and can change during a session; see the Mixxx adapter.
enum class Role { Leader, Follower };

// A control change at an exact moment on the shared clock, e.g.
// "[Channel1],volume = 0.0 at shared time T". Fixed-size so it can be copied
// on the audio thread without allocating.
struct ControlEvent {
    uint32_t seq = 0;
    Nanos time = 0;
    FixedString<64> key;
    double value = 0;
    FixedString<64> text;
};

// Where a deck is at a given shared-clock time (leader -> follower, frequent,
// may be lost). See DeckSnapshotMsg.
struct DeckSnapshot {
    Nanos time = 0;
    uint8_t deck = 0;
    bool playing = false;
    double positionSec = 0;
    double rate = 1.0;
    uint64_t trackId = 0;
};

struct SessionConfig {
    Role role = Role::Leader;
    uint16_t localPort = 0;
    std::string peerHost;     // follower only, direct mode
    uint16_t peerPort = 0;    // follower only, direct mode
    // Join-code mode: set the rendezvous server. The host then gets a code
    // (see rendezvousStatus) and the guest joins with joinCode instead of
    // peerHost/peerPort. Both connect directly once the server introduces them.
    std::string rendezvousHost;
    uint16_t rendezvousPort = 7700;
    std::string joinCode;     // follower only
    // Join-code mode also uses `password` below: the session secret. The
    // host makes one (rendezvousStatus().secret) unless given one; the guest
    // must be given the host's. It never goes to the join-code server.
    // Testing: skip the direct route and go straight through the server's relay.
    bool relayOnly = false;
    // Connecting by address: both sides must use the same password (it is
    // mixed into the encryption keys; someone without it can't join).
    // Join-code mode: the session secret (see joinCode).
    std::string password;
    std::string appVersion = kVersion; // told to the peer, for update messages
    double bpm = 124.0;       // leader only
    Nanos delayNs = 150 * kNanosPerMs; // leader only: how far behind the follower renders
    Nanos startInNs = 3 * kNanosPerSec; // leader only: beat 0 is this far in the future
    NetSimConfig sim;
};

// Where a join-code session is up to (main thread: poll rendezvousStatus()).
struct RendezvousStatus {
    enum class State {
        Off,        // direct mode (no server)
        Contacting, // talking to the server
        Waiting,    // host: have a code, waiting for the guest
        Punching,   // introduced; opening a direct route
        Connected,  // direct route works
        Failed,     // see error; retrying says whether we keep trying
    };
    State state = State::Off;
    std::string code;          // host: the code to share; guest: the code used
    std::string secret;        // host: the session secret, given to the guest with the code (through
                               // the room, never the join-code server): it keys the encryption
    std::string checkCode;     // once connected: the same six digits on both screens
    std::string publicAddress; // our address as the internet sees it
    std::string error;         // when Failed: a sentence for the user
    bool retrying = false;     // when Failed: still trying in the background
    bool relayed = false;      // Connected through the server's relay (no direct route)
};

// The join-code server built into this copy ("host:port"; CMake B2B_DEFAULT_SERVER).
const char* defaultRendezvousServer();

// Something about the peer the user should know (main thread: linkProblem()).
enum class LinkProblem {
    None,
    PeerOutdated,  // the friend's app speaks an older protocol: they need to update
    PeerNewer,     // the friend's app is newer: we need to update
    WrongPassword, // the handshake works but the keys don't match
};

// "bass tiger 42" / "Bass_Tiger-42" -> "BASS-TIGER-42".
std::string normaliseJoinCode(const std::string& text);
// A random session password for connecting by address, e.g. "KX7P-Q2MD-9TZA"
// (60 bits; letters and digits that can't be misread).
std::string makeSessionPassword();
// How passwords are compared: letters and digits only, upper case.
std::string normalisePassword(const std::string& text);

// True for "BASS-TIGER-42"-style text, false for addresses like "1.2.3.4:9000".
bool looksLikeJoinCode(const std::string& text);

struct SessionStats {
    std::atomic<uint64_t> eventsSent{0};
    std::atomic<uint64_t> eventResends{0};
    std::atomic<uint64_t> eventsAcked{0};
    std::atomic<uint64_t> eventsDropped{0}; // gave up resending (follower gone)
    std::atomic<uint64_t> eventsSuperseded{0}; // not resent: a newer value of the same control was sent
    std::atomic<uint64_t> eventsReceived{0};
    std::atomic<uint64_t> duplicateEvents{0};
    std::atomic<uint64_t> pingsSent{0};
    std::atomic<uint64_t> pongsReceived{0};
    std::atomic<uint64_t> snapshotsSent{0};
    std::atomic<uint64_t> snapshotsReceived{0};
    std::atomic<uint64_t> authFailures{0};  // packets that failed decryption (forged, stale or wrong keys)
    std::atomic<uint64_t> hellosRefused{0}; // handshakes with a key the server didn't introduce
};

// Owns the network side of one peer: clock sync, the shared timeline and
// reliable delivery of control events. Runs its own network thread.
//
// Thread safety: the "audio thread" methods are lock-free and allocation-free.
class Session {
public:
    Session(SessionConfig config, const LocalClock& clock);
    ~Session();

    bool start();
    void stop();

    Role role() const { return config_.role; }

    // ---- Audio-thread safe ----

    // True once we have the timeline and (follower) a usable clock estimate.
    bool ready() const;

    // Our local clock converted to the shared (leader) clock.
    Nanos sharedFromLocal(Nanos local) const;

    Timeline timeline() const;

    // How far behind the shared clock this peer renders: 0 for the leader,
    // the agreed delay for the follower.
    Nanos renderDelay() const;

    // Audio thread: queue an event to send to the peer (reliable).
    // The caller numbers its events 1, 2, 3... (seq must stay below 0x80000000).
    bool sendEvent(const ControlEvent& event) { return outgoing_.push(event); }

    // Audio thread: queue a deck snapshot (sent once, never resent).
    bool sendSnapshot(const DeckSnapshot& snapshot) { return outgoingSnapshots_.push(snapshot); }

    // Audio thread: take the next received event (in arrival order).
    // Session messages (keys starting "[B2B]," except "[B2B],handover") go
    // to popMainEvent instead.
    bool popReceivedEvent(ControlEvent& event) { return incoming_.pop(event); }

    // Audio thread: take the next received deck snapshot.
    bool popReceivedSnapshot(DeckSnapshot& snapshot) { return incomingSnapshots_.pop(snapshot); }

    // ---- Main (UI) thread ----

    // Queue an event from the main thread (e.g. a track load, or a session
    // message such as "[B2B],request"). Its seq is assigned by the session.
    bool sendEventFromMainThread(const ControlEvent& event) { return outgoingMain_.push(event); }

    // Main thread: next received session message ("[B2B],..." keys).
    bool popMainEvent(ControlEvent& event) { return incomingMain_.pop(event); }

    // Our local clock when we last heard anything from the peer (0 = never).
    Nanos lastHeard() const { return lastHeard_.load(std::memory_order_relaxed); }

    // Round trip to the peer that 90% of recent pings beat (0 = not known yet).
    Nanos rttP90() const { return rttP90_.load(std::memory_order_relaxed); }

    // Our local clock converted to the shared clock, right now.
    Nanos sharedNow() const { return sharedFromLocal(clock_.now()); }

    // Increases each time the peer (re)connects: a new guest acknowledged the
    // session (host), or a new host session started (guest). Watch it to
    // resend full state (loaded tracks, control values).
    uint32_t peerGeneration() const { return peerGeneration_.load(std::memory_order_acquire); }

    // ---- Bulk stream (track sharing), any thread ----
    //
    // A reliable, ordered byte stream to the peer inside the session's own
    // UDP traffic (KCP), so it works wherever the session works: direct,
    // through the relay, through any NAT. A new stream (new epoch) starts
    // whenever the peer (re)connects; data never crosses from one to the next.

    // Current stream, 0 = none (not connected yet).
    uint32_t streamEpoch() const { return streamEpoch_.load(std::memory_order_acquire); }
    // Bytes accepted (0 = buffer full, try later), -1 = that stream is gone.
    int streamSend(uint32_t epoch, const uint8_t* data, size_t length);
    // Bytes read (0 = nothing yet), -1 = that stream is gone.
    int streamRecv(uint32_t epoch, uint8_t* buffer, size_t capacity);
    // Waits until readable (1) and/or writable (2); 0 on timeout, -1 = gone.
    int streamWait(uint32_t epoch, bool wantRead, bool wantWrite, int timeoutMs);

    // ---- Diagnostics ----
    int streamWindow() const { return streamWindowDiag_.load(std::memory_order_relaxed); }
    Nanos lastRtt() const { return lastRttDiag_.load(std::memory_order_relaxed); }
    const ClockSync& clockSync() const { return clockSync_; }
    const SessionStats& stats() const { return stats_; }
    std::string peerAddress() const;
    RendezvousStatus rendezvousStatus() const;
    LinkProblem linkProblem() const { return static_cast<LinkProblem>(linkProblem_.load(std::memory_order_relaxed)); }
    std::string peerAppVersion() const;
    // True while the peer is reached through the server's relay (it limits
    // how much a session may send, both ways together).
    bool relayed() const { return relayed_.load(std::memory_order_relaxed); }
    // True once packets from the peer decrypt with our session keys.
    bool encrypted() const { return authenticated_.load(std::memory_order_relaxed); }
    uint16_t localPort() const { return socket_.localPort(); }

private:
    void run();
    void send(const Message& message);
    void addRtt(Nanos rtt);
    void peerChanged();
    void setStatus(const RendezvousStatus& status);
    void sendHello(const Endpoint& to);
    void setProblem(LinkProblem problem) { linkProblem_.store(static_cast<int>(problem), std::memory_order_relaxed); }
    void streamReset(uint32_t conv); // network thread; conv 0 = no stream
    void streamPump(Nanos now);       // network thread
    static int streamOutput(const char* data, int length, IKCPCB* kcp, void* user);

    SessionConfig config_;
    const LocalClock& clock_;
    UdpSocket socket_;
    std::unique_ptr<Link> link_;

    ClockSync clockSync_;
    SessionStats stats_;

    std::atomic<bool> timelineReady_{false};
    std::atomic<Nanos> t0_{0};
    std::atomic<double> bpm_{0};
    std::atomic<Nanos> delayNs_{0};

    SpscQueue<ControlEvent, 1024> outgoing_;          // audio -> network
    SpscQueue<ControlEvent, 256> outgoingMain_;       // main thread -> network
    SpscQueue<DeckSnapshot, 1024> outgoingSnapshots_; // audio -> network
    SpscQueue<ControlEvent, 1024> incoming_;          // network -> audio
    SpscQueue<ControlEvent, 256> incomingMain_;       // network -> main thread
    SpscQueue<DeckSnapshot, 1024> incomingSnapshots_; // network -> audio
    std::atomic<Nanos> rttP90_{0};
    std::atomic<Nanos> lastHeard_{0};
    std::vector<Nanos> rtts_; // network thread: recent round trips (ring)
    size_t rttNext_ = 0;
    std::unordered_set<uint32_t> seen_; // network thread: peer event seqs already delivered
    // Audio-thread events carry the caller's sequence numbers (1, 2, 3...).
    // Main-thread events get numbers from a separate range so they never clash.
    uint32_t nextMainSeq_ = 0x80000000u; // network thread only

    // Encryption (network thread only, except the atomics).
    std::unique_ptr<crypto::KeyPair> keys_;     // ours, fresh per session
    std::unique_ptr<crypto::Channel> channel_;  // keys for the current peer
    struct PendingPeer {                        // host: a guest that said hello but hasn't proven its keys yet
        Endpoint from;
        std::unique_ptr<crypto::Channel> channel;
        Nanos since = 0;
        int failures = 0;
    };
    std::vector<PendingPeer> pending_;
    crypto::Key expectedPeerKey_{};             // join-code mode: the key the server introduced
    bool haveExpectedKey_ = false;
    std::string sharedSecret_;                  // the join code, or the password
    Nanos channelSince_ = 0;
    Nanos lastAuthentic_ = 0;
    std::atomic<int> linkProblem_{0};
    std::atomic<bool> authenticated_{false};
    std::string peerAppVersion_;                // guarded by statusMutex_

    Endpoint server_; // rendezvous server (join-code mode)
    std::vector<uint8_t> relayHeader_; // network thread: "B2BX" + token + role, once known
    mutable std::mutex statusMutex_;
    RendezvousStatus status_;

    Endpoint peer_; // only touched by the network thread after start()
    std::atomic<bool> peerKnown_{false};

    // Bulk stream. kcp_ is network-thread only; the buffers are shared.
    IKCPCB* kcp_ = nullptr;
    Nanos kcpNextUpdate_ = 0;
    // Delay-based throttle: shrink the send window when the round trip rises
    // above its quiet level (a queue is building that would delay sync traffic).
    Nanos recentRtt_[3] = {0, 0, 0}; // network thread: last three round trips
    int recentRttNext_ = 0;
    Nanos minRtt_ = 0;               // quiet level: lowest over ~2 minutes
    Nanos minRttBuckets_[4] = {0, 0, 0, 0}; // 30 s each
    int minRttBucket_ = 0;
    Nanos minRttSince_ = 0;
    Nanos lastThrottle_ = 0;
    int streamWindow_ = 0;
    // Grow fast (by a third every 100 ms) until the first sign of queueing,
    // then gently; fast again after the stream has been idle.
    bool streamFastStart_ = true;
    int streamPeakInFlight_ = 0; // the most in flight since the last throttle step
    // Fast start: delivery measured over whole round trips (acks come in
    // clumps once a round trip); two in a row no faster than the one before
    // mean the link is full.
    Nanos streamRateSince_ = 0;
    uint32_t streamAckedAtSince_ = 0;
    double streamRateLast_ = 0;     // segments per second, the round trip before
    bool streamWindowLimited_ = false; // the window was full throughout
    int streamFlatSteps_ = 0;
    std::atomic<int> streamWindowDiag_{0};
    std::atomic<Nanos> lastRttDiag_{0};
    std::atomic<bool> relayed_{false};
    uint32_t epochCounter_ = 0;
    std::atomic<uint32_t> streamEpoch_{0};
    std::mutex streamMutex_;
    std::condition_variable streamCv_;
    std::vector<uint8_t> streamOut_; // app -> network
    std::vector<uint8_t> streamIn_;  // network -> app
    size_t streamInPos_ = 0;

    std::atomic<uint32_t> peerGeneration_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace b2b
