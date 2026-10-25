// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/session.hpp"

#include "ikcp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <unordered_map>
#include <random>
#include <sstream>
#include <variant>

namespace b2b {
namespace {

// Resending unacknowledged events: wait about one round trip (not less than
// 20 ms), doubling each time up to 1 s. A fixed short interval floods slow
// links (a 150 ms round trip meant ~10 copies of everything), and the flood
// itself then makes the link slower still.
constexpr Nanos kEventResendMin = 20 * kNanosPerMs;
constexpr Nanos kEventResendMax = 1 * kNanosPerSec;
constexpr Nanos kEventGiveUpAfter = 8 * kNanosPerSec;
constexpr Nanos kStartResendInterval = 250 * kNanosPerMs;
constexpr Nanos kPingIntervalFast = 50 * kNanosPerMs;  // while converging
constexpr Nanos kPingIntervalSlow = 100 * kNanosPerMs; // steady state

// Join-code mode (see b2b-server's README for the B2BR1 protocol).
constexpr char kRvMagic[] = "B2BR1";
constexpr Nanos kRvRetry = 1 * kNanosPerSec;          // HOST / JOIN until answered
constexpr Nanos kRvRetryAfterFailure = 5 * kNanosPerSec;
constexpr Nanos kRvGiveUpContact = 8 * kNanosPerSec;  // then tell the user, keep trying
constexpr Nanos kRvKeepWaiting = 5 * kNanosPerSec;    // host keepalive while waiting
constexpr Nanos kRvKeepConnected = 15 * kNanosPerSec; // host keepalive once connected
constexpr Nanos kRvRejoin = 2 * kNanosPerSec;         // guest repeats JOIN while punching
constexpr Nanos kPeerSilentFor = 10 * kNanosPerSec;    // a working peer isn't replaced before this long silent
constexpr Nanos kPunchInterval = 100 * kNanosPerMs;
constexpr Nanos kPunchGiveUp = 15 * kNanosPerSec;
constexpr Nanos kRelayAfter = 3 * kNanosPerSec; // no direct route by then: try the relay too

// Bulk stream (KCP). Segments of 1100 bytes stay under 1200 on the wire even
// with our header and the relay's, so they are never fragmented.
constexpr int kKcpMtu = 1100;
constexpr int kKcpWindow = 512;                  // segments in flight (~550 KB)
constexpr size_t kStreamBufferCap = 512 * 1024;  // per direction, between app and network thread
constexpr size_t kStreamSendChunk = 32 * (kKcpMtu - 24);

uint32_t newStreamConv() {
    std::random_device rd;
    uint32_t conv = 0;
    while (conv == 0) {
        conv = static_cast<uint32_t>(rd());
    }
    return conv;
}

std::vector<std::string> splitWords(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;) {
        words.push_back(w);
    }
    return words;
}

// "1.2.3.4:5" -> endpoint. IPv6 ("[...]:5") isn't supported by our sockets yet.
bool parseEndpoint(const std::string& text, Endpoint& out) {
    const size_t colon = text.rfind(':');
    if (text.empty() || text[0] == '[' || colon == std::string::npos || colon == 0) {
        return false;
    }
    const std::string host = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if (host.find_first_not_of("0123456789.") != std::string::npos || port.empty() ||
        port.find_first_not_of("0123456789") != std::string::npos || port.size() > 5) {
        return false;
    }
    const unsigned long p = std::stoul(port);
    return p > 0 && p < 65536 && UdpSocket::resolve(host, static_cast<uint16_t>(p), out);
}

std::string randomNonce() {
    std::random_device rd;
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 12; ++i) {
        s += hex[rd() & 15u];
    }
    return s;
}

std::string serverError(const std::string& reason, const std::string& code) {
    if (reason == "unknown") {
        return "There's no session with the code " + code + ". Check it with your friend; codes stop working "
               "when the host leaves.";
    }
    if (reason == "busy") {
        return "Someone else has already joined " + code + ".";
    }
    if (reason == "slow") {
        return "Too many attempts. Wait a minute, then try again.";
    }
    if (reason == "full") {
        return "The B2B server is full right now. Trying again...";
    }
    return "The B2B server didn't understand the request (" + reason + "). Is the app up to date?";
}

} // namespace

#ifndef B2B_DEFAULT_SERVER
#define B2B_DEFAULT_SERVER "connect.pitchride.co.uk:7700"
#endif

const char* defaultRendezvousServer() { return B2B_DEFAULT_SERVER; }

std::string normaliseJoinCode(const std::string& text) {
    std::string out;
    bool dash = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            if (dash && !out.empty()) {
                out += '-';
            }
            dash = false;
            out += static_cast<char>(std::toupper(u));
        } else if (c == '-' || c == '_' || c == ' ') {
            dash = true;
        }
    }
    return out;
}

std::string makeSessionPassword() {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // 32: no 0/O, 1/I
    uint8_t random[12];
    crypto::randomBytes(random, sizeof(random));
    std::string out;
    for (size_t i = 0; i < sizeof(random); ++i) {
        if (i > 0 && i % 4 == 0) {
            out += '-';
        }
        out += alphabet[random[i] & 31];
    }
    return out;
}

std::string normalisePassword(const std::string& text) {
    std::string out;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out += static_cast<char>(std::toupper(u));
        }
    }
    return out;
}

bool looksLikeJoinCode(const std::string& text) {
    bool letter = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '.' || c == ':') {
            return false; // an address
        }
        if (std::isalpha(u)) {
            letter = true;
        } else if (!std::isdigit(u) && c != '-' && c != '_' && c != ' ') {
            return false;
        }
    }
    const std::string code = normaliseJoinCode(text);
    return letter && std::count(code.begin(), code.end(), '-') >= 1;
}

Session::Session(SessionConfig config, const LocalClock& clock) : config_(std::move(config)), clock_(clock) {}

Session::~Session() { stop(); }

bool Session::start() {
    if (!socket_.bind(config_.localPort)) {
        return false;
    }
    keys_ = std::make_unique<crypto::KeyPair>(crypto::KeyPair::generate());
    const bool rendezvous = !config_.rendezvousHost.empty();
    if (rendezvous && config_.role == Role::Leader && normalisePassword(config_.password).empty()) {
        config_.password = makeSessionPassword(); // this session's secret, for the guest
    }
    if (rendezvous && normalisePassword(config_.password).empty()) {
        return false; // a guest without the host's session secret can't get in
    }
    // Join-code mode: the code and the session secret together key the
    // encryption (the host learns its own code from the server; the secret
    // never goes there). By address: the password.
    sharedSecret_ = !rendezvous ? normalisePassword(config_.password)
                    : config_.role == Role::Follower
                            ? normaliseJoinCode(config_.joinCode) + "/" + normalisePassword(config_.password)
                            : std::string();
    if (rendezvous) {
        if (!UdpSocket::resolve(config_.rendezvousHost, config_.rendezvousPort, server_)) {
            return false;
        }
        if (config_.role == Role::Follower && normaliseJoinCode(config_.joinCode).empty()) {
            return false;
        }
        RendezvousStatus status;
        status.state = RendezvousStatus::State::Contacting;
        status.code = config_.role == Role::Follower ? normaliseJoinCode(config_.joinCode) : std::string();
        status.secret = config_.role == Role::Leader ? config_.password : std::string();
        setStatus(status);
    }
    if (config_.role == Role::Follower && !rendezvous) {
        if (!UdpSocket::resolve(config_.peerHost, config_.peerPort, peer_)) {
            return false;
        }
        peerKnown_ = true;
    } else if (config_.role == Role::Leader) {
        // The leader's clock *is* the shared clock, so it can fix the timeline now.
        t0_ = clock_.now() + config_.startInNs;
        bpm_ = config_.bpm;
        delayNs_ = config_.delayNs;
        timelineReady_ = true;
    }
    link_ = std::make_unique<Link>(socket_, config_.sim);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Session::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    link_.reset();
}

bool Session::ready() const {
    if (!timelineReady_.load(std::memory_order_acquire)) {
        return false;
    }
    return config_.role == Role::Leader || clockSync_.hasEstimate();
}

Nanos Session::sharedFromLocal(Nanos local) const {
    return config_.role == Role::Leader ? local : local + clockSync_.offset();
}

Timeline Session::timeline() const {
    Timeline t;
    t.t0 = t0_.load(std::memory_order_acquire);
    t.bpm = bpm_.load(std::memory_order_acquire);
    return t;
}

Nanos Session::renderDelay() const {
    return config_.role == Role::Leader ? 0 : delayNs_.load(std::memory_order_acquire);
}

std::string Session::peerAddress() const { return peer_.toString(); }

void Session::sendHello(const Endpoint& to) {
    HelloMsg hello;
    hello.role = config_.role == Role::Leader ? 0 : 1;
    std::copy(keys_->publicKey.begin(), keys_->publicKey.end(), hello.publicKey.begin());
    hello.appVersion = config_.appVersion;
    std::vector<uint8_t> packet = encodeHello(hello);
    if (!relayHeader_.empty() && to == server_) {
        packet.insert(packet.begin(), relayHeader_.begin(), relayHeader_.end());
    }
    link_->send(to, std::move(packet));
}

std::string Session::peerAppVersion() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return peerAppVersion_;
}

RendezvousStatus Session::rendezvousStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void Session::setStatus(const RendezvousStatus& status) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = status;
}

void Session::send(const Message& message) {
    if (!peerKnown_ || !channel_) {
        return; // nothing goes out unencrypted (only the hello handshake)
    }
    const std::vector<uint8_t> plain = encode(message);
    std::vector<uint8_t> packet = channel_->seal(kMagic, kEnvelopeSealed, plain.data(), plain.size());
    if (!relayHeader_.empty() && peer_ == server_) {
        // Through the server's relay: it needs our token and side first.
        packet.insert(packet.begin(), relayHeader_.begin(), relayHeader_.end());
    }
    link_->send(peer_, std::move(packet));
}

void Session::addRtt(Nanos rtt) {
    // For the stream throttle: the last few round trips, and the quiet level
    // (lowest over the last ~2 minutes, in 30 s buckets so a lasting route
    // change ages out, but a long transfer can't make "slow" the new normal).
    const Nanos now = clock_.now();
    recentRtt_[recentRttNext_] = rtt;
    lastRttDiag_.store(rtt, std::memory_order_relaxed);
    recentRttNext_ = (recentRttNext_ + 1) % 3;
    if (now - minRttSince_ > 30 * kNanosPerSec) {
        minRttBucket_ = (minRttBucket_ + 1) % 4;
        minRttBuckets_[minRttBucket_] = 0;
        minRttSince_ = now;
    }
    Nanos& bucket = minRttBuckets_[minRttBucket_];
    if (bucket == 0 || rtt < bucket) {
        bucket = rtt;
    }
    minRtt_ = 0;
    for (Nanos b : minRttBuckets_) {
        if (b > 0 && (minRtt_ == 0 || b < minRtt_)) {
            minRtt_ = b;
        }
    }
    constexpr size_t kRttWindow = 64;
    if (rtts_.size() < kRttWindow) {
        rtts_.push_back(rtt);
    } else {
        rtts_[rttNext_] = rtt;
        rttNext_ = (rttNext_ + 1) % kRttWindow;
    }
    std::vector<Nanos> sorted = rtts_;
    std::sort(sorted.begin(), sorted.end());
    rttP90_.store(sorted[(sorted.size() * 9) / 10 < sorted.size() ? (sorted.size() * 9) / 10 : sorted.size() - 1],
                  std::memory_order_relaxed);
}

void Session::peerChanged() {
    // A new peer instance numbers its events from 1 again.
    seen_.clear();
    peerGeneration_.fetch_add(1, std::memory_order_acq_rel);
    // And a fresh bulk stream: the host starts one; the guest joins it when
    // the host's first segment arrives.
    streamReset(config_.role == Role::Leader ? newStreamConv() : 0);
}

// ---- Bulk stream ----------------------------------------------------------

int Session::streamOutput(const char* data, int length, IKCPCB* /*kcp*/, void* user) {
    auto* self = static_cast<Session*>(user);
    StreamMsg m;
    m.data.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + length);
    self->send(m);
    return 0;
}

void Session::streamReset(uint32_t conv) {
    if (kcp_) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(streamMutex_);
        streamOut_.clear();
        streamIn_.clear();
        streamInPos_ = 0;
        if (conv != 0) {
            kcp_ = ikcp_create(conv, this);
            ikcp_setoutput(kcp_, &Session::streamOutput);
            ikcp_setmtu(kcp_, kKcpMtu);
            // Quick resends and no loss-based congestion control (it collapses
            // on lossy links): the upload cap and the delay throttle below
            // keep us from flooding the connection instead.
            ikcp_nodelay(kcp_, 1, 10, 2, 1);
            // Don't call a segment lost just because a jittery link delayed
            // it: spurious resends only add to the queue. Real losses are
            // caught sooner by fast resend (two later segments acked).
            kcp_->rx_minrto = 200;
            streamWindow_ = 8; // start small; the controller opens it up
            streamFastStart_ = true;
            streamPeakInFlight_ = 0;
            streamRateSince_ = 0;
            streamRateLast_ = 0;
            streamFlatSteps_ = 0;
            ikcp_wndsize(kcp_, streamWindow_, kKcpWindow);
            kcp_->stream = 1;                // a byte stream, not messages
            kcpNextUpdate_ = 0;
            if (++epochCounter_ == 0) {
                ++epochCounter_;
            }
            streamEpoch_.store(epochCounter_, std::memory_order_release);
        } else {
            streamEpoch_.store(0, std::memory_order_release);
        }
    }
    streamCv_.notify_all();
}

void Session::streamPump(Nanos now) {
    if (!kcp_) {
        return;
    }
    bool notify = false;
    // App -> network, as far as the send window allows.
    while (ikcp_waitsnd(kcp_) < 2 * kKcpWindow) {
        std::vector<uint8_t> chunk;
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            if (streamOut_.empty()) {
                break;
            }
            const size_t n = std::min(streamOut_.size(), kStreamSendChunk);
            chunk.assign(streamOut_.begin(), streamOut_.begin() + static_cast<long>(n));
            streamOut_.erase(streamOut_.begin(), streamOut_.begin() + static_cast<long>(n));
        }
        ikcp_send(kcp_, reinterpret_cast<const char*>(chunk.data()), static_cast<int>(chunk.size()));
        kcpNextUpdate_ = 0;
        notify = true;
    }
    // Every 100 ms, a delay-based controller (in the spirit of LEDBAT): aim
    // for at most ~30 ms of queueing on top of the quiet round trip. A new
    // transfer grows fast, by a third each step, until the round trips show
    // the first queueing (15 ms over quiet); from then on slowly,
    // two segments a step (the signal arrives a round trip late, so growing
    // fast for long overshoots by seconds on links with big buffers). Above
    // 30 ms it cuts back by a quarter each time; far above it, all but stops.
    if (now - lastThrottle_ >= 100 * kNanosPerMs && minRtt_ > 0 && recentRtt_[2] > 0) {
        lastThrottle_ = now;
        const Nanos recent = std::min({recentRtt_[0], recentRtt_[1], recentRtt_[2]});
        // The middle of the last three: one round trip delayed by jitter
        // doesn't end the fast start, two do.
        const Nanos middle = std::max(std::min(recentRtt_[0], recentRtt_[1]),
                                      std::min(std::max(recentRtt_[0], recentRtt_[1]), recentRtt_[2]));
        const Nanos excess = recent - minRtt_;
        if (middle - minRtt_ > 15 * kNanosPerMs) {
            streamFastStart_ = false;
        }
        // Or when a bigger window stopped delivering more (the link is full
        // and a queue is about to build; this shows before the delay does).
        const uint32_t ackedTotal = kcp_->snd_nxt - kcp_->nsnd_buf; // selective acks too
        const bool full = streamPeakInFlight_ >= streamWindow_ - 1;
        if (!streamFastStart_ || streamRateSince_ == 0) {
            streamRateSince_ = now;
            streamAckedAtSince_ = ackedTotal;
            streamRateLast_ = 0;
            streamWindowLimited_ = full;
        } else {
            streamWindowLimited_ = streamWindowLimited_ && full;
            if (now - streamRateSince_ >= std::max(minRtt_, 100 * kNanosPerMs)) {
                const double rate = static_cast<double>(ackedTotal - streamAckedAtSince_) /
                                    (static_cast<double>(now - streamRateSince_) / 1e9);
                if (streamWindowLimited_ && streamRateLast_ > 0) {
                    streamFlatSteps_ = rate < streamRateLast_ * 1.125 ? streamFlatSteps_ + 1 : 0;
                    if (streamFlatSteps_ >= 2) {
                        streamFastStart_ = false;
                    }
                }
                streamRateLast_ = streamWindowLimited_ ? rate : 0;
                streamRateSince_ = now;
                streamAckedAtSince_ = ackedTotal;
                streamWindowLimited_ = full;
            }
        }
        int next = streamWindow_;
        if (excess > 200 * kNanosPerMs) {
            next = 2;
        } else if (excess > 30 * kNanosPerMs) {
            next = std::max(2, streamWindow_ * 3 / 4);
        } else if (excess < 15 * kNanosPerMs) {
            // Only grow a window that was actually full (at its busiest in
            // this step): an idle stream must not build up a big window and
            // then burst when a track starts.
            if (streamPeakInFlight_ >= streamWindow_ - 1) {
                const int step = streamFastStart_ ? std::max(2, streamWindow_ / 3) : 2;
                next = std::min(kKcpWindow, streamWindow_ + step);
            } else if (ikcp_waitsnd(kcp_) == 0) {
                next = std::max(8, streamWindow_ - 1); // idle: drift back to a small start
                streamFastStart_ = true;
                streamFlatSteps_ = 0;
            }
        }
        streamPeakInFlight_ = 0;
        if (next != streamWindow_) {
            streamWindow_ = next;
            ikcp_wndsize(kcp_, streamWindow_, kKcpWindow);
        }
        streamWindowDiag_.store(streamWindow_, std::memory_order_relaxed);
    }
    const auto ms = static_cast<IUINT32>(now / kNanosPerMs);
    if (now >= kcpNextUpdate_) {
        ikcp_update(kcp_, ms);
        streamPeakInFlight_ = std::max(streamPeakInFlight_, static_cast<int>(kcp_->snd_nxt - kcp_->snd_una));
        const IUINT32 next = ikcp_check(kcp_, ms);
        kcpNextUpdate_ = now + static_cast<Nanos>(static_cast<int32_t>(next - ms)) * kNanosPerMs;
    }
    // Network -> app, unless the app is behind (then KCP's window holds the peer back).
    char tmp[64 * 1024];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            if (streamIn_.size() - streamInPos_ >= kStreamBufferCap) {
                break;
            }
        }
        const int n = ikcp_recv(kcp_, tmp, sizeof(tmp));
        if (n <= 0) {
            break;
        }
        std::lock_guard<std::mutex> lock(streamMutex_);
        if (streamInPos_ > 0 && streamInPos_ * 2 > streamIn_.size()) {
            streamIn_.erase(streamIn_.begin(), streamIn_.begin() + static_cast<long>(streamInPos_));
            streamInPos_ = 0;
        }
        streamIn_.insert(streamIn_.end(), tmp, tmp + n);
        notify = true;
    }
    if (kcp_->state == static_cast<IUINT32>(-1)) {
        // The peer stopped acknowledging (gone, or a stream we don't share):
        // the host starts over, the guest waits for the host's new stream.
        streamReset(config_.role == Role::Leader ? newStreamConv() : 0);
        return;
    }
    if (notify) {
        streamCv_.notify_all();
    }
}

int Session::streamSend(uint32_t epoch, const uint8_t* data, size_t length) {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(streamMutex_);
        if (epoch == 0 || streamEpoch_.load(std::memory_order_relaxed) != epoch) {
            return -1;
        }
        n = std::min(length, kStreamBufferCap > streamOut_.size() ? kStreamBufferCap - streamOut_.size() : 0);
        streamOut_.insert(streamOut_.end(), data, data + n);
    }
    return static_cast<int>(n);
}

int Session::streamRecv(uint32_t epoch, uint8_t* buffer, size_t capacity) {
    std::lock_guard<std::mutex> lock(streamMutex_);
    if (epoch == 0 || streamEpoch_.load(std::memory_order_relaxed) != epoch) {
        return -1;
    }
    const size_t n = std::min(capacity, streamIn_.size() - streamInPos_);
    std::memcpy(buffer, streamIn_.data() + streamInPos_, n);
    streamInPos_ += n;
    if (streamInPos_ == streamIn_.size()) {
        streamIn_.clear();
        streamInPos_ = 0;
    }
    return static_cast<int>(n);
}

int Session::streamWait(uint32_t epoch, bool wantRead, bool wantWrite, int timeoutMs) {
    std::unique_lock<std::mutex> lock(streamMutex_);
    auto state = [&]() -> int {
        if (epoch == 0 || streamEpoch_.load(std::memory_order_relaxed) != epoch) {
            return -1;
        }
        int mask = 0;
        if (wantRead && streamIn_.size() > streamInPos_) {
            mask |= 1;
        }
        if (wantWrite && streamOut_.size() < kStreamBufferCap) {
            mask |= 2;
        }
        return mask;
    };
    streamCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return state() != 0; });
    return state();
}

// One loop for both roles. The host answers clock pings and announces the
// session; the guest syncs its clock to the host. Everything else (reliable
// events, snapshots, round-trip measurement) is the same in both directions,
// because either side can be the live DJ.
void Session::run() {
    const bool host = config_.role == Role::Leader;
    struct Unacked {
        EventMsg msg;
        Nanos firstSent;
        Nanos lastSent;
        Nanos wait; // until the next resend
    };
    std::map<uint32_t, Unacked> unacked;
    // Latest unacknowledged event per control: a newer value of the same
    // control makes resending the older one pointless.
    std::unordered_map<std::string, uint32_t> latestByKey;
    auto resendWait = [this]() {
        const Nanos rtt = rttP90_.load(std::memory_order_relaxed);
        const Nanos base = rtt > 0 ? rtt + rtt / 4 + 10 * kNanosPerMs : 100 * kNanosPerMs;
        return std::clamp(base, kEventResendMin, kEventResendMax);
    };
    bool startAcked = false;     // host: the current guest acknowledged the session
    bool haveStart = false;      // guest: we have seen a session start
    Nanos lastStartSent = 0;
    uint32_t nextPingId = 1;
    Nanos lastPing = 0;
    Nanos lastHello = 0;
    uint8_t buffer[1500];

    // ---- Join-code mode (network thread only) ----
    const bool rv = !config_.rendezvousHost.empty();
    RendezvousStatus rs = rendezvousStatus();
    std::string nonce = randomNonce();
    std::string secret;                 // host: proves the code is ours (the server's, not the session secret)
    // What the server gets instead of the session secret: a guest must bring
    // the same ticket, so a code on its own (guessed, overheard) gets nobody in.
    const std::string ticket = rv ? crypto::rendezvousTicket(normalisePassword(config_.password)) : std::string();
    std::vector<Endpoint> candidates;   // the peer's addresses, from the server
    bool terminal = false;              // guest: stop (wrong code etc.)
    const Nanos contactStart = clock_.now();
    bool heardServer = false;
    Nanos lastServerSend = 0;
    Nanos punchStart = 0;
    Nanos lastPunch = 0;
    std::string localsText = "-";
    std::string checkCode;              // for the peer whose key is checkFor
    crypto::Key checkFor{};
    static const std::string kNoCheck;
    {
        uint32_t ip = 0;
        if (UdpSocket::primaryLocalIpv4(ip)) {
            Endpoint e{ip, socket_.localPort(), true};
            localsText = e.toString();
        }
    }
    auto sendServer = [&](const std::string& text) {
        const std::string line = std::string(kRvMagic) + " " + text;
        socket_.sendTo(server_, reinterpret_cast<const uint8_t*>(line.data()), line.size());
        lastServerSend = clock_.now();
    };
    auto publish = [&](RendezvousStatus::State state, std::string error = {}, bool retrying = false) {
        if (rs.state == state && rs.error == error && rs.retrying == retrying) {
            return;
        }
        rs.state = state;
        rs.error = std::move(error);
        rs.retrying = retrying;
        setStatus(rs);
    };
    // Same IP as one of the peer's addresses (the port can differ: some
    // routers give each destination its own port).
    auto isCandidate = [&](const Endpoint& from) {
        if (from == server_ && !relayHeader_.empty()) {
            return true; // the relay
        }
        for (const Endpoint& c : candidates) {
            if (c.ipv4 == from.ipv4) {
                return true;
            }
        }
        return false;
    };
    // A packet from where the peer might be (any address when hosting by address).
    auto fromPeerSide = [&](const Endpoint& from) {
        if (rv) {
            return isCandidate(from);
        }
        return host || (peerKnown_ && from == peer_);
    };
    auto handleHello = [&](const HelloMsg& hello, const Endpoint& from, Nanos now) {
        if (hello.role == (host ? 0 : 1) || !fromPeerSide(from)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            peerAppVersion_ = hello.appVersion;
        }
        if (hello.protocol != kProtocolVersion) {
            setProblem(hello.protocol < kProtocolVersion ? LinkProblem::PeerOutdated : LinkProblem::PeerNewer);
            return;
        }
        crypto::Key key{};
        std::copy(hello.publicKey.begin(), hello.publicKey.end(), key.begin());
        if (rv && haveExpectedKey_ && key != expectedPeerKey_) {
            stats_.hellosRefused++; // not who the server introduced
            return;
        }
        if (host) {
            const bool known = channel_ && channel_->peerPublic() == key;
            if (!known) {
                auto it = std::find_if(pending_.begin(), pending_.end(),
                                       [&](const PendingPeer& p) { return p.from == from; });
                if (it == pending_.end() || it->channel->peerPublic() != key) {
                    auto channel = std::make_unique<crypto::Channel>();
                    if (!channel->init(*keys_, key, true, sharedSecret_)) {
                        return;
                    }
                    if (it != pending_.end()) {
                        pending_.erase(it);
                    }
                    if (pending_.size() >= 8) {
                        pending_.erase(pending_.begin()); // the oldest
                    }
                    pending_.push_back({from, std::move(channel), now, 0});
                }
            }
            sendHello(from); // so it can work out the same keys
        } else {
            if (!channel_ || channel_->peerPublic() != key) {
                auto channel = std::make_unique<crypto::Channel>();
                if (!channel->init(*keys_, key, false, sharedSecret_)) {
                    return;
                }
                channel_ = std::move(channel);
                channelSince_ = now;
                lastAuthentic_ = 0;
            }
            if (!peerKnown_) {
                peer_ = from; // join-code mode: the first route that answers
                peerKnown_ = true;
            }
        }
    };
    auto openSealed = [&](const uint8_t* data, size_t len, const Endpoint& from, Nanos now,
                          bool& promoted) -> std::optional<Message> {
        std::vector<uint8_t> plain;
        if (channel_ && channel_->open(data, len, plain)) {
            lastAuthentic_ = now;
            authenticated_ = true;
            return decode(plain.data(), plain.size());
        }
        if (host) {
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if (!(it->from == from)) {
                    continue;
                }
                if (it->channel->open(data, len, plain)) {
                    if (channel_ && lastAuthentic_ != 0 && now - lastAuthentic_ < kPeerSilentFor) {
                        break; // our peer is here and talking: nobody takes its place
                    }
                    channel_ = std::move(it->channel);
                    pending_.erase(it);
                    promoted = true;
                    lastAuthentic_ = now;
                    authenticated_ = true;
                    return decode(plain.data(), plain.size());
                }
                if (++it->failures >= 10) {
                    setProblem(LinkProblem::WrongPassword); // they said hello, but their keys differ
                }
                break;
            }
        }
        stats_.authFailures++;
        return std::nullopt;
    };
    auto handleServer = [&](const std::string& text, Nanos now) {
        const auto w = splitWords(text);
        if (w.size() < 2 || w[0] != kRvMagic) {
            return;
        }
        heardServer = true;
        const std::string& kind = w[1];
        if (kind == "CODE" && host && w.size() >= 6 && w[2] == nonce && secret.empty()) {
            rs.code = w[3];
            rs.publicAddress = w[4];
            secret = w[5];
            // The code and the session secret key the encryption (the guest gets both through the room).
            sharedSecret_ = rs.code + "/" + normalisePassword(config_.password);
            setStatus(rs);
            publish(startAcked ? RendezvousStatus::State::Connected : RendezvousStatus::State::Waiting);
        } else if (kind == "PEER" && w.size() >= 5) {
            if (w[2] != rs.code) {
                return; // not about our session
            }
            // While our peer is here and talking, nobody new is introduced:
            // only when it has been silent a while may the server's word change.
            const bool linked = host ? (peerKnown_ && startAcked) : (peerKnown_ && haveStart);
            if (linked && lastAuthentic_ != 0 && now - lastAuthentic_ < kPeerSilentFor) {
                return;
            }
            std::vector<Endpoint> fresh;
            Endpoint e;
            if (parseEndpoint(w[3], e)) {
                fresh.push_back(e);
            }
            std::istringstream locals(w[4]);
            for (std::string part; std::getline(locals, part, ',');) {
                if (parseEndpoint(part, e) && (fresh.empty() || !(e == fresh.front()))) {
                    fresh.push_back(e);
                }
            }
            if (fresh.empty()) {
                return;
            }
            const bool changed = candidates.empty() || !(fresh.front() == candidates.front());
            candidates = fresh;
            if (config_.relayOnly) {
                candidates.clear();
            }
            // Relay token (servers older than the relay don't send one).
            if (w.size() >= 6 && w[5].size() == 16 &&
                w[5].find_first_not_of("0123456789abcdef") == std::string::npos) {
                std::vector<uint8_t> header = {'B', '2', 'B', 'X'};
                for (size_t i = 0; i < 16; i += 2) {
                    header.push_back(static_cast<uint8_t>(std::stoul(w[5].substr(i, 2), nullptr, 16)));
                }
                header.push_back(host ? 'H' : 'G');
                relayHeader_ = header;
            }
            // The peer's public key, as the server saw it: only that key may
            // complete the handshake (older servers don't send one).
            crypto::Key key{};
            if (w.size() >= 7 && crypto::fromHex(w[6], key)) {
                expectedPeerKey_ = key;
                haveExpectedKey_ = true;
            }
            const bool linkedNow = host ? (peerKnown_ && startAcked) : (peerKnown_ && haveStart);
            if (!linkedNow || changed) {
                if (rs.state != RendezvousStatus::State::Punching) {
                    punchStart = now;
                }
                publish(RendezvousStatus::State::Punching);
            }
        } else if (kind == "GONE" && host) {
            // The server forgot us (it restarted, or we were away too long): get a new code.
            secret.clear();
            nonce = randomNonce();
            rs.code.clear();
            setStatus(rs);
            if (!(peerKnown_ && startAcked)) {
                publish(RendezvousStatus::State::Contacting);
            }
        } else if (kind == "ERR" && w.size() >= 4) {
            const std::string& reason = w[3];
            if (host) {
                if (secret.empty() && (reason == "full" || reason == "slow")) {
                    publish(RendezvousStatus::State::Failed, serverError(reason, ""), true);
                } else if (reason == "secret") {
                    secret.clear(); // start over with a new code
                    nonce = randomNonce();
                }
            } else if (!(peerKnown_ && haveStart)) {
                const bool stop = reason == "unknown" || reason == "busy" || reason == "bad";
                terminal = stop;
                publish(RendezvousStatus::State::Failed, serverError(reason, rs.code), !stop);
            }
        }
    };

    while (running_) {
        Nanos now = clock_.now();
        relayed_.store(rv && !relayHeader_.empty() && peer_ == server_, std::memory_order_relaxed);

        if (rv) {
            const bool linked = host ? (peerKnown_ && startAcked) : (peerKnown_ && haveStart);
            if (linked && channel_ && (checkCode.empty() || checkFor != channel_->peerPublic())) {
                checkFor = channel_->peerPublic();
                checkCode = crypto::checkCode(host ? keys_->publicKey : checkFor, host ? checkFor : keys_->publicKey,
                                              sharedSecret_);
            }
            const std::string& check = linked && channel_ ? checkCode : kNoCheck;
            if (linked && (rs.state != RendezvousStatus::State::Connected || rs.relayed != (peer_ == server_) ||
                           rs.checkCode != check)) {
                rs.relayed = peer_ == server_;
                rs.checkCode = check;
                rs.state = RendezvousStatus::State::Connected;
                rs.error.clear();
                rs.retrying = false;
                setStatus(rs);
            }
            // Talk to the server.
            const bool failed = rs.state == RendezvousStatus::State::Failed;
            if (host) {
                if (secret.empty()) {
                    if (now - lastServerSend > (failed ? kRvRetryAfterFailure : kRvRetry)) {
                        sendServer("HOST " + nonce + " " + localsText + " " + crypto::toHex(keys_->publicKey) + " " +
                                   ticket);
                    }
                } else if (now - lastServerSend > (linked ? kRvKeepConnected : kRvKeepWaiting)) {
                    // "linked": the server keeps the guest's place for them.
                    sendServer("KEEP " + rs.code + " " + secret + (linked ? " linked" : ""));
                }
            } else if (!terminal && !linked) {
                const Nanos every = failed ? kRvRetryAfterFailure
                                  : rs.state == RendezvousStatus::State::Punching ? kRvRejoin : kRvRetry;
                if (now - lastServerSend > every) {
                    sendServer("JOIN " + rs.code + " " + nonce + " " + localsText + " " +
                               crypto::toHex(keys_->publicKey) + " " + ticket);
                }
            }
            if (!heardServer && rs.state == RendezvousStatus::State::Contacting &&
                now - contactStart > kRvGiveUpContact) {
                publish(RendezvousStatus::State::Failed,
                        "Can't reach the B2B server. Check your internet connection. Still trying...", true);
            }
            // Open a route: send to every address the peer might have until one answers.
            if (rs.state == RendezvousStatus::State::Punching && !linked) {
                if (now - punchStart > kPunchGiveUp) {
                    publish(RendezvousStatus::State::Failed,
                            relayHeader_.empty()
                                    ? "Couldn't connect directly: one of your routers is blocking it, and the "
                                      "B2B server's relay isn't available. Try again later."
                                    : "Couldn't connect to your friend, even through the B2B server. "
                                      "Check that neither of you blocks the app in a firewall, then try again.",
                            host); // the host keeps waiting for another try
                    terminal = !host;
                } else if (now - lastPunch > kPunchInterval) {
                    for (const Endpoint& c : candidates) {
                        sendHello(c);
                    }
                    // No direct route yet: try through the server as well.
                    // Whichever answers first is kept.
                    if (!relayHeader_.empty() && (config_.relayOnly || now - punchStart > kRelayAfter)) {
                        sendHello(server_);
                    }
                    lastPunch = now;
                }
            }
        }

        // Handshake. The guest says hello to the host it knows until packets
        // from it decrypt (again); the host answers each hello. A key pair
        // that never decrypts anything means the secrets differ.
        if (!host && peerKnown_ && (lastAuthentic_ == 0 || now - lastAuthentic_ > 3 * kNanosPerSec) &&
            now - lastHello > 250 * kNanosPerMs) {
            sendHello(peer_);
            lastHello = now;
        }
        if (!host && channel_ && lastAuthentic_ == 0 && now - channelSince_ > 3 * kNanosPerSec) {
            setProblem(LinkProblem::WrongPassword);
        }
        if (host ? (peerKnown_ && startAcked) : haveStart) {
            if (linkProblem() != LinkProblem::None) {
                setProblem(LinkProblem::None);
            }
        }

        // Pings: the guest needs them for its clock; the host only measures
        // the round trip (to choose how far behind it listens).
        if (peerKnown_ && (host ? startAcked : true)) {
            const Nanos interval = (!host && !clockSync_.hasEstimate()) ? kPingIntervalFast : kPingIntervalSlow;
            if (now - lastPing > interval) {
                send(PingMsg{nextPingId++, clock_.now()});
                stats_.pingsSent++;
                lastPing = now;
            }
        }

        // Take every packet that is waiting, not just one per pass: a burst
        // (a track streaming in) would otherwise overflow the socket's buffer
        // and drop control events along with it.
        for (int burst = 0; burst < 256; ++burst) {
            Endpoint from;
            const int n = socket_.recvFrom(buffer, sizeof(buffer), from, burst == 0 ? 2 : 0);
            if (n <= 0) {
                break;
            }
            const bool serverText = n >= 6 && std::memcmp(buffer, "B2BR1 ", 6) == 0;
            if (n > 0 && rv && from == server_ && serverText) {
                handleServer(std::string(reinterpret_cast<const char*>(buffer), static_cast<size_t>(n)), clock_.now());
            } else if (n > 0) {
                const Nanos receivedAt = clock_.now();
                const auto len = static_cast<size_t>(n);
                std::optional<Message> msg;
                bool promoted = false;
                const uint32_t magic = peekMagic(buffer, len);
                if (magic == kMagicV3) {
                    if (fromPeerSide(from)) {
                        setProblem(LinkProblem::PeerOutdated); // an app from before encryption
                    }
                } else if (magic == kMagic && len > 4 && buffer[4] == kEnvelopeHello) {
                    if (auto hello = decodeHello(buffer, len)) {
                        handleHello(*hello, from, receivedAt);
                    }
                } else if (magic == kMagic && len > 4 && buffer[4] == kEnvelopeSealed) {
                    msg = openSealed(buffer, len, from, receivedAt, promoted);
                }
                if (msg && promoted) {
                    // A guest proved it holds the keys for its hello: from now on, it's the one.
                    peer_ = from;
                    peerKnown_ = true;
                    startAcked = false;
                }
                if (msg && rv && !(peerKnown_ && from == peer_)) {
                    // Join-code mode: only the peer the server introduced may
                    // connect. Keep the first route that works; switch only if it
                    // has gone quiet (a late packet down another route is ignored).
                    const bool settled = peerKnown_ && (host ? startAcked : haveStart) &&
                                         receivedAt - lastHeard_.load(std::memory_order_relaxed) < 2 * kNanosPerSec;
                    if (!isCandidate(from) || settled) {
                        msg.reset();
                    } else if (!host) {
                        peer_ = from;
                        peerKnown_ = true;
                    }
                }
                if (msg) {
                    lastHeard_.store(receivedAt, std::memory_order_relaxed);
                    if (host && (!peerKnown_ || !(from == peer_))) {
                        peer_ = from; // learn (or re-learn) the guest's address
                        peerKnown_ = true;
                        startAcked = false;
                    }
                    if (auto* ping = std::get_if<PingMsg>(&*msg)) {
                        PongMsg pong{ping->id, ping->t1, receivedAt, 0};
                        pong.t3 = clock_.now();
                        send(pong);
                    } else if (auto* pong = std::get_if<PongMsg>(&*msg)) {
                        if (!host) {
                            clockSync_.addSample(pong->t1, pong->t2, pong->t3, receivedAt);
                        }
                        addRtt((receivedAt - pong->t1) - (pong->t3 - pong->t2));
                        stats_.pongsReceived++;
                    } else if (std::holds_alternative<StartAckMsg>(*msg)) {
                        if (host && !startAcked) {
                            startAcked = true;
                            peerChanged();
                        }
                    } else if (auto* start = std::get_if<StartMsg>(&*msg)) {
                        if (!host) {
                            if (!haveStart || start->t0 != t0_.load()) {
                                haveStart = true;
                                t0_ = start->t0;
                                bpm_ = start->bpm;
                                delayNs_ = start->delayNs;
                                timelineReady_.store(true, std::memory_order_release);
                                peerChanged();
                            }
                            send(StartAckMsg{});
                        }
                    } else if (auto* event = std::get_if<EventMsg>(&*msg)) {
                        send(EventAckMsg{event->seq}); // always ack, even duplicates (the ack may have been lost)
                        if (seen_.insert(event->seq).second) {
                            ControlEvent ev;
                            ev.seq = event->seq;
                            ev.time = event->time;
                            ev.key.assign(event->key);
                            ev.value = event->value;
                            ev.text.assign(event->text);
                            const bool sessionMsg = event->key.rfind("[B2B],", 0) == 0 && event->key != "[B2B],handover";
                            if (sessionMsg) {
                                incomingMain_.push(ev);
                            } else {
                                incoming_.push(ev);
                            }
                            stats_.eventsReceived++;
                        } else {
                            stats_.duplicateEvents++;
                        }
                    } else if (auto* ack = std::get_if<EventAckMsg>(&*msg)) {
                        if (unacked.erase(ack->seq) > 0) {
                            stats_.eventsAcked++;
                        }
                    } else if (auto* sm = std::get_if<StreamMsg>(&*msg)) {
                        if (sm->data.size() >= 24) { // a KCP segment header
                            const IUINT32 conv = ikcp_getconv(sm->data.data());
                            if (!host && haveStart && (!kcp_ || kcp_->conv != conv)) {
                                streamReset(conv); // the host started a new stream: join it
                            }
                            if (kcp_ && kcp_->conv == conv) {
                                ikcp_input(kcp_, reinterpret_cast<const char*>(sm->data.data()),
                                           static_cast<long>(sm->data.size()));
                                kcpNextUpdate_ = 0;
                            }
                        }
                    } else if (auto* s = std::get_if<DeckSnapshotMsg>(&*msg)) {
                        incomingSnapshots_.push({s->time, s->deck, s->playing != 0, s->positionSec, s->rate, s->trackId});
                        stats_.snapshotsReceived++;
                    }
                }
            }

        }

        now = clock_.now();

        if (host && peerKnown_ && !startAcked && now - lastStartSent > kStartResendInterval) {
            send(StartMsg{t0_.load(), bpm_.load(), delayNs_.load()});
            lastStartSent = now;
        }

        // New events from the audio thread and the main thread.
        const bool connected = host ? (peerKnown_ && startAcked) : haveStart;
        auto queueEvent = [&](const ControlEvent& ev) {
            if (!connected) {
                return; // nobody to send to; the peer gets full state when it connects
            }
            EventMsg msg{ev.seq, ev.time, ev.key.str(), ev.value, ev.text.str()};
            if (msg.key.rfind("[B2B]", 0) != 0) { // session messages must all arrive
                auto [slot, inserted] = latestByKey.try_emplace(msg.key, msg.seq);
                if (!inserted) {
                    if (unacked.erase(slot->second) > 0) {
                        stats_.eventsSuperseded++;
                    }
                    slot->second = msg.seq;
                }
            }
            unacked[msg.seq] = {msg, now, now, resendWait()};
            send(msg);
            stats_.eventsSent++;
        };
        ControlEvent ev;
        while (outgoing_.pop(ev)) {
            queueEvent(ev);
        }
        while (outgoingMain_.pop(ev)) {
            ev.seq = nextMainSeq_++;
            queueEvent(ev);
        }

        // Deck snapshots: fire and forget, a newer one follows shortly.
        DeckSnapshot snap;
        while (outgoingSnapshots_.pop(snap)) {
            if (!connected) {
                continue;
            }
            send(DeckSnapshotMsg{snap.time, snap.deck, static_cast<uint8_t>(snap.playing ? 1 : 0),
                                 snap.positionSec, snap.rate, snap.trackId});
            stats_.snapshotsSent++;
        }

        streamPump(now);

        // Resend anything not yet acknowledged (lost packets). Give up after a
        // few seconds: the peer has disconnected, and will get full state
        // when it reconnects.
        for (auto it = unacked.begin(); it != unacked.end();) {
            Unacked& item = it->second;
            if (now - item.firstSent > kEventGiveUpAfter) {
                it = unacked.erase(it);
                stats_.eventsDropped++;
                continue;
            }
            if (now - item.lastSent > item.wait) {
                send(item.msg);
                item.lastSent = now;
                item.wait = std::min(item.wait * 2, kEventResendMax);
                stats_.eventResends++;
            }
            ++it;
        }
    }

    if (rv && host && !secret.empty()) {
        sendServer("LEAVE " + rs.code + " " + secret); // free the code straight away
    }
    streamReset(0);
}

} // namespace b2b
