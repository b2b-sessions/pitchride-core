// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/file_share.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace b2b {

namespace fs = std::filesystem;

namespace {

enum FrameType : uint8_t {
    kHello = 1,
    kList = 2,
    kRequest = 3,
    kHint = 4,
    kBegin = 5,
    kData = 6,
    kEnd = 7,
    kError = 8,
    kCancel = 9,
    kMeta = 10, // track details, before kBegin; older peers ignore it
};

constexpr size_t kMaxBeats = 48 * 1024;

constexpr size_t kChunk = 64 * 1024;
constexpr size_t kMaxFrame = kChunk + 1024;
constexpr size_t kMaxListFrame = 16 * 1024 * 1024;
constexpr uint64_t kProgressStep = 256 * 1024;

fs::path toPath(const std::string& utf8) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

std::string fromPath(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string hex16(uint64_t v) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<size_t>(i)] = kDigits[v & 0xf];
        v >>= 4;
    }
    return s;
}

// Keep only a short alphanumeric extension: it ends up in a file name.
bool startsWith(const uint8_t* b, size_t n, const char* sig, size_t at = 0) {
    const size_t len = std::strlen(sig);
    return n >= at + len && std::memcmp(b + at, sig, len) == 0;
}

std::string safeExtension(const std::string& ext) {
    std::string out;
    for (char c : ext) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        if (out.size() >= 5) {
            break;
        }
    }
    return out.empty() ? std::string("bin") : out;
}

struct W {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void str16(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 0xffff);
        u16(static_cast<uint16_t>(n));
        b.insert(b.end(), s.begin(), s.begin() + static_cast<long>(n));
    }
    void str8(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 0xff);
        u8(static_cast<uint8_t>(n));
        b.insert(b.end(), s.begin(), s.begin() + static_cast<long>(n));
    }
    void put(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
};

struct R {
    const uint8_t* d;
    size_t n;
    size_t p = 0;
    bool ok = true;
    uint64_t get(int k) {
        if (!ok || p + static_cast<size_t>(k) > n) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < k; ++i) {
            v |= static_cast<uint64_t>(d[p + static_cast<size_t>(i)]) << (8 * i);
        }
        p += static_cast<size_t>(k);
        return v;
    }
    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    std::string str(size_t len) {
        if (!ok || p + len > n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(d + p), len);
        p += len;
        return s;
    }
    std::string str16() { return str(u16()); }
    std::string str8() { return str(u8()); }
};

} // namespace

std::string safeFileName(const std::string& name, const std::string& fallback) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7f || std::strchr("/\\:*?\"<>|", c) != nullptr) {
            out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    // Trim spaces and dots at both ends (Windows drops trailing dots; a
    // leading dot hides the file), collapse runs of spaces.
    std::string collapsed;
    for (char c : out) {
        if (c == ' ' && (collapsed.empty() || collapsed.back() == ' ')) {
            continue;
        }
        collapsed += c;
    }
    while (!collapsed.empty() && (collapsed.back() == ' ' || collapsed.back() == '.')) {
        collapsed.pop_back();
    }
    size_t start = 0;
    while (start < collapsed.size() && (collapsed[start] == ' ' || collapsed[start] == '.')) {
        ++start;
    }
    collapsed = collapsed.substr(start);
    // At most ~80 bytes, without cutting a UTF-8 character in half.
    if (collapsed.size() > 80) {
        size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(collapsed[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        collapsed.resize(cut);
        while (!collapsed.empty() && collapsed.back() == ' ') {
            collapsed.pop_back();
        }
    }
    // Names Windows reserves for devices.
    std::string upper;
    for (char c : collapsed.substr(0, collapsed.find('.'))) {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                      "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                      "LPT7", "LPT8", "LPT9"};
    for (const char* r : kReserved) {
        if (upper == r) {
            return fallback;
        }
    }
    return collapsed.empty() ? fallback : collapsed;
}

FileShare::FileShare(FileShareConfig config, Callbacks callbacks)
        : config_(std::move(config)), cb_(std::move(callbacks)) {}

FileShare::~FileShare() { stop(); }

bool FileShare::start() {
    std::error_code ec;
    fs::create_directories(toPath(config_.cacheDir), ec);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void FileShare::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FileShare::setLocalTracks(std::vector<SharedTrack> tracks) {
    std::lock_guard<std::mutex> lock(mutex_);
    localTracks_ = std::move(tracks);
    listDirty_ = true;
}

void FileShare::request(uint64_t id, uint8_t priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = wanted_.find(id);
    if (it != wanted_.end() && it->second <= priority) {
        return; // already wanted at this or a more urgent priority
    }
    wanted_[id] = priority;
    newRequests_.emplace_back(id, priority);
}

void FileShare::hint(uint64_t id, uint8_t priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    newHints_.emplace_back(id, priority);
}

void FileShare::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wanted_.erase(id) > 0) {
        newCancels_.push_back(id);
    }
}

void FileShare::queueFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    W w;
    w.u32(static_cast<uint32_t>(payload.size() + 1));
    w.u8(type);
    w.b.insert(w.b.end(), payload.begin(), payload.end());
    controlQueue_.push_back(std::move(w.b));
}

void FileShare::onConnected() {
    connected_ = true;
    rbuf_.clear();
    wbuf_.clear();
    wpos_ = 0;
    controlQueue_.clear();
    W hello;
    hello.u32(kProtocolVersion);
    queueFrame(kHello, hello.b);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listDirty_ = true; // send our list
        newRequests_.clear();
        for (const auto& [id, prio] : wanted_) {
            newRequests_.emplace_back(id, prio); // re-request anything still missing
        }
    }
    if (cb_.onConnection) {
        cb_.onConnection(true);
    }
}

void FileShare::onDisconnected() {
    conn_.reset();
    connected_ = false;
    outgoing_.clear();
    for (auto& [id, in] : incoming_) {
        in.file.reset();
        std::error_code ec;
        fs::remove(toPath(in.partPath), ec);
    }
    incoming_.clear();
    if (cb_.onConnection) {
        cb_.onConnection(false);
    }
}

namespace {

// One stream of the session (an epoch). Gone (-1) once the session starts a
// new one, which makes FileShare start over cleanly.
class SessionStream : public ByteStream {
public:
    SessionStream(Session& session, uint32_t epoch) : session_(session), epoch_(epoch) {}
    int send(const uint8_t* data, size_t length) override { return session_.streamSend(epoch_, data, length); }
    int recv(uint8_t* buffer, size_t capacity) override { return session_.streamRecv(epoch_, buffer, capacity); }
    int wait(bool wantRead, bool wantWrite, int timeoutMs) override {
        return session_.streamWait(epoch_, wantRead, wantWrite, timeoutMs);
    }

private:
    Session& session_;
    uint32_t epoch_;
};

} // namespace

bool isAcceptedAudioFile(const std::string& path) {
    const fs::path p = toPath(path);
    std::string ext = p.has_extension() ? fromPath(p.extension()).substr(1) : std::string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::ifstream f(p, std::ios::binary);
    uint8_t b[16] = {};
    f.read(reinterpret_cast<char*>(b), sizeof(b));
    const size_t n = static_cast<size_t>(f.gcount());
    // MP3: an ID3 tag, or straight into a frame (11 sync bits set).
    const bool mp3 = startsWith(b, n, "ID3") || (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0);
    const bool mp4 = startsWith(b, n, "ftyp", 4); // m4a / mp4 / alac
    if (ext == "mp3") return mp3;
    if (ext == "flac") return startsWith(b, n, "fLaC") || (startsWith(b, n, "ID3"));
    if (ext == "wav") return startsWith(b, n, "RIFF") && startsWith(b, n, "WAVE", 8);
    if (ext == "aif" || ext == "aiff") return startsWith(b, n, "FORM") && (startsWith(b, n, "AIFF", 8) || startsWith(b, n, "AIFC", 8));
    if (ext == "ogg" || ext == "opus") return startsWith(b, n, "OggS");
    if (ext == "m4a" || ext == "mp4" || ext == "alac") return mp4;
    if (ext == "aac") return startsWith(b, n, "ID3") || (n >= 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0);
    if (ext == "wv") return startsWith(b, n, "wvpk");
    return false;
}

void FileShare::run() {
    Endpoint peer;
    Nanos nextConnectAttempt = 0;
    while (running_) {
        if (config_.session) {
            // Follow the session's stream: a new one means a (re)connected peer.
            const uint32_t epoch = config_.session->streamEpoch();
            if (epoch != epoch_) {
                if (conn_) {
                    onDisconnected();
                }
                epoch_ = epoch;
                if (epoch != 0) {
                    conn_ = std::make_unique<SessionStream>(*config_.session, epoch);
                    onConnected();
                }
            }
        } else if (config_.role == Role::Leader) {
            // Leader: always accept; a newer connection replaces an older one
            // (the follower restarted or its network changed).
            if (!listener_) {
                listener_ = std::make_unique<TcpSocket>();
                if (!listener_->listen(config_.listenPort)) {
                    listener_.reset();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            }
            if (auto incoming = listener_->accept()) {
                if (conn_) {
                    onDisconnected();
                }
                conn_ = std::move(incoming);
                onConnected();
            }
        } else if (!conn_) {
            if (trueNow() < nextConnectAttempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            nextConnectAttempt = trueNow() + kNanosPerSec;
            if (!peer.valid && !UdpSocket::resolve(config_.peerHost, config_.peerPort, peer)) {
                continue;
            }
            auto s = std::make_unique<TcpSocket>();
            if (s->connect(peer, 2000)) {
                conn_ = std::move(s);
                onConnected();
            }
            continue;
        }

        if (!conn_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // Requests from other threads.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (listDirty_) {
                listDirty_ = false;
                W w;
                w.u32(static_cast<uint32_t>(localTracks_.size()));
                for (const auto& t : localTracks_) {
                    w.u64(t.id);
                    w.u64(t.size);
                    w.str16(t.name);
                }
                queueFrame(kList, w.b);
            }
            for (const auto& [id, prio] : newRequests_) {
                W w;
                w.u64(id);
                w.u8(prio);
                queueFrame(kRequest, w.b);
            }
            newRequests_.clear();
            for (const auto& [id, prio] : newHints_) {
                W w;
                w.u64(id);
                w.u8(prio);
                queueFrame(kHint, w.b);
            }
            newHints_.clear();
            for (uint64_t id : newCancels_) {
                W w;
                w.u64(id);
                queueFrame(kCancel, w.b);
                auto it = incoming_.find(id);
                if (it != incoming_.end()) {
                    it->second.file.reset();
                    std::error_code ec;
                    fs::remove(toPath(it->second.partPath), ec);
                    incoming_.erase(it);
                }
            }
            newCancels_.clear();
        }

        if (!pump(*conn_)) {
            onDisconnected();
        }
    }
    if (conn_) {
        onDisconnected();
    }
    listener_.reset();
}

void FileShare::queueMeta(uint64_t id) {
    TrackMeta meta;
    if (!cb_.describe || !cb_.describe(id, meta)) {
        return;
    }
    W m;
    m.u64(id);
    m.str16(meta.title);
    m.str16(meta.artist);
    m.str8(meta.key);
    uint64_t bpmBits = 0;
    std::memcpy(&bpmBits, &meta.bpm, sizeof(bpmBits));
    m.u64(bpmBits);
    m.str8(meta.beatsVersion);
    m.str8(meta.beatsSubVersion);
    const bool beatsFit = meta.beats.size() <= kMaxBeats;
    m.u32(beatsFit ? static_cast<uint32_t>(meta.beats.size()) : 0);
    if (beatsFit) {
        m.b.insert(m.b.end(), meta.beats.begin(), meta.beats.end());
    }
    queueFrame(kMeta, m.b);
}

void FileShare::startSending(uint64_t id, uint8_t priority) {
    auto it = outgoing_.find(id);
    if (it != outgoing_.end()) {
        it->second.priority = std::min(it->second.priority, priority);
        return;
    }
    std::string path;
    if (!cb_.lookup || !cb_.lookup(id, path)) {
        W w;
        w.u64(id);
        w.str16("not shared");
        queueFrame(kError, w.b);
        return;
    }
    std::error_code ec;
    const fs::path p = toPath(path);
    const uint64_t size = fs::file_size(p, ec);
    if (ec || size > config_.maxFileBytes) {
        W w;
        w.u64(id);
        w.str16(ec ? "unreadable" : "too large");
        queueFrame(kError, w.b);
        return;
    }
    auto file = std::make_unique<std::ifstream>(p, std::ios::binary);
    if (!*file) {
        W w;
        w.u64(id);
        w.str16("unreadable");
        queueFrame(kError, w.b);
        return;
    }
    std::string ext = p.extension().u8string().empty() ? std::string()
                                                         : fromPath(p.extension()).substr(1);
    queueMeta(id);
    W begin;
    begin.u64(id);
    begin.u64(size);
    begin.str8(safeExtension(ext));
    queueFrame(kBegin, begin.b);
    outgoing_[id] = Outgoing{id, priority, size, 0, sendOrder_++, std::move(file)};
}

bool FileShare::fillWriteBuffer() {
    if (wpos_ < wbuf_.size()) {
        return true;
    }
    wbuf_.clear();
    wpos_ = 0;
    if (!controlQueue_.empty()) {
        wbuf_ = std::move(controlQueue_.front());
        controlQueue_.pop_front();
        return true;
    }
    if (outgoing_.empty()) {
        return false;
    }

    // Upload cap (token bucket).
    const Nanos now = trueNow();
    if (lastRefill_ == 0) {
        lastRefill_ = now;
    }
    const uint64_t rate = config_.session && config_.session->relayed()
                                  ? std::min(config_.rateBytesPerSec, config_.relayRateBytesPerSec)
                                  : config_.rateBytesPerSec;
    tokens_ += static_cast<double>(now - lastRefill_) / 1e9 * static_cast<double>(rate);
    tokens_ = std::min(tokens_, static_cast<double>(4 * kChunk));
    lastRefill_ = now;
    if (tokens_ < static_cast<double>(kChunk)) {
        return false;
    }

    // Most urgent first, then oldest.
    auto best = std::min_element(outgoing_.begin(), outgoing_.end(), [](const auto& a, const auto& b) {
        return a.second.priority != b.second.priority ? a.second.priority < b.second.priority
                                                       : a.second.order < b.second.order;
    });
    Outgoing& out = best->second;
    std::vector<uint8_t> chunk(static_cast<size_t>(std::min<uint64_t>(kChunk, out.size - out.sent)));
    if (!chunk.empty()) {
        out.file->read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        if (out.file->gcount() != static_cast<std::streamsize>(chunk.size())) {
            W w;
            w.u64(out.id);
            w.str16("read error");
            queueFrame(kError, w.b);
            outgoing_.erase(best);
            return true;
        }
    }
    W w;
    w.u32(static_cast<uint32_t>(chunk.size() + 1 + 8));
    w.u8(kData);
    w.u64(out.id);
    w.b.insert(w.b.end(), chunk.begin(), chunk.end());
    wbuf_ = std::move(w.b);
    out.sent += chunk.size();
    tokens_ -= static_cast<double>(chunk.size());
    stats_.bytesSent += chunk.size();
    if (out.sent >= out.size) {
        // Again at the end: the track may have finished analysing (beat
        // grid, key) while it was on its way.
        queueMeta(out.id);
        W end;
        end.u64(out.id);
        queueFrame(kEnd, end.b);
        stats_.filesSent++;
        outgoing_.erase(best);
    }
    return true;
}

bool FileShare::pump(ByteStream& socket) {
    const bool haveData = fillWriteBuffer();
    const int ready = socket.wait(true, haveData, 10);
    if (ready < 0) {
        return false;
    }
    if (ready & 1) {
        uint8_t buf[64 * 1024];
        for (int i = 0; i < 8; ++i) {
            const int n = socket.recv(buf, sizeof(buf));
            if (n < 0) {
                return false;
            }
            if (n == 0) {
                break;
            }
            rbuf_.insert(rbuf_.end(), buf, buf + n);
            stats_.bytesReceived += static_cast<uint64_t>(n);
        }
        size_t pos = 0;
        while (rbuf_.size() - pos >= 5) {
            const uint32_t len = static_cast<uint32_t>(rbuf_[pos]) | (static_cast<uint32_t>(rbuf_[pos + 1]) << 8) |
                                 (static_cast<uint32_t>(rbuf_[pos + 2]) << 16) |
                                 (static_cast<uint32_t>(rbuf_[pos + 3]) << 24);
            if (len == 0 || len > kMaxListFrame) {
                return false; // corrupt or hostile stream
            }
            if (rbuf_.size() - pos - 4 < len) {
                break;
            }
            const uint8_t type = rbuf_[pos + 4];
            if (type != kList && len > kMaxFrame) {
                return false;
            }
            handleFrame(type, rbuf_.data() + pos + 5, len - 1);
            pos += 4 + len;
        }
        rbuf_.erase(rbuf_.begin(), rbuf_.begin() + static_cast<long>(pos));
    }
    while (wpos_ < wbuf_.size()) {
        const int n = socket.send(wbuf_.data() + wpos_, wbuf_.size() - wpos_);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            break;
        }
        wpos_ += static_cast<size_t>(n);
        if (wpos_ >= wbuf_.size() && !fillWriteBuffer()) {
            break;
        }
    }
    return true;
}

void FileShare::handleFrame(uint8_t type, const uint8_t* data, size_t length) {
    R r{data, length};
    switch (type) {
    case kHello:
        r.u32();
        break;
    case kList: {
        const uint32_t n = r.u32();
        std::vector<SharedTrack> list;
        list.reserve(std::min<uint32_t>(n, 100000));
        for (uint32_t i = 0; i < n && r.ok; ++i) {
            SharedTrack t;
            t.id = r.u64();
            t.size = r.u64();
            t.name = r.str16();
            if (r.ok) {
                list.push_back(std::move(t));
            }
        }
        peerNames_.clear();
        for (const auto& t : list) {
            peerNames_[t.id] = t.name;
        }
        if (cb_.onPeerList) {
            cb_.onPeerList(std::move(list));
        }
        break;
    }
    case kMeta: {
        const uint64_t id = r.u64();
        TrackMeta meta;
        meta.title = r.str16();
        meta.artist = r.str16();
        meta.key = r.str8();
        const uint64_t bpmBits = r.u64();
        std::memcpy(&meta.bpm, &bpmBits, sizeof(bpmBits));
        meta.beatsVersion = r.str8();
        meta.beatsSubVersion = r.str8();
        const uint32_t beatsLen = r.u32();
        if (!r.ok || beatsLen > kMaxBeats) {
            break;
        }
        const std::string beats = r.str(beatsLen);
        if (!r.ok) {
            break;
        }
        meta.beats.assign(beats.begin(), beats.end());
        if (!(meta.bpm >= 0 && meta.bpm < 1000)) {
            meta.bpm = 0; // NaN or nonsense
        }
        bool isWanted;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            isWanted = wanted_.count(id) > 0;
        }
        if (isWanted && cb_.onPeerMeta) {
            cb_.onPeerMeta(id, std::move(meta));
        }
        break;
    }
    case kRequest: {
        const uint64_t id = r.u64();
        const uint8_t prio = r.u8();
        if (r.ok) {
            startSending(id, prio);
        }
        break;
    }
    case kHint: {
        const uint64_t id = r.u64();
        const uint8_t prio = r.u8();
        if (r.ok && cb_.onPeerHint) {
            cb_.onPeerHint(id, prio);
        }
        break;
    }
    case kCancel: {
        const uint64_t id = r.u64();
        if (r.ok) {
            outgoing_.erase(id);
        }
        break;
    }
    case kBegin: {
        const uint64_t id = r.u64();
        const uint64_t size = r.u64();
        const std::string ext = safeExtension(r.str8());
        if (!r.ok) {
            break;
        }
        bool isWanted;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            isWanted = wanted_.count(id) > 0;
        }
        auto refuse = [&](const std::string& reason) {
            W w;
            w.u64(id);
            queueFrame(kCancel, w.b);
            if (cb_.onFailed) {
                cb_.onFailed(id, reason);
            }
        };
        if (!isWanted) {
            refuse("not requested"); // never accept files we did not ask for
            break;
        }
        if (size > config_.maxFileBytes) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                wanted_.erase(id);
            }
            refuse("too large");
            break;
        }
        // Each borrowed track in its own folder, named like the original, so
        // even a file without tags shows a real name.
        const auto known = peerNames_.find(id);
        const std::string base = safeFileName(known != peerNames_.end() ? known->second : std::string(), hex16(id));
        const fs::path finalPath = toPath(config_.cacheDir) / hex16(id) / toPath(base + "." + ext);
        const fs::path partPath = toPath(config_.cacheDir) / (hex16(id) + "." + ext + ".part");
        auto file = std::make_unique<std::ofstream>(partPath, std::ios::binary | std::ios::trunc);
        if (!*file) {
            refuse("cannot write cache");
            break;
        }
        incoming_[id] = Incoming{size, 0, 0, fromPath(partPath), fromPath(finalPath), std::move(file)};
        if (cb_.onProgress) {
            cb_.onProgress(id, 0, size);
        }
        break;
    }
    case kData: {
        const uint64_t id = r.u64();
        auto it = incoming_.find(id);
        if (!r.ok || it == incoming_.end()) {
            break;
        }
        Incoming& in = it->second;
        const size_t n = length - 8;
        if (in.received + n > in.size) {
            in.file.reset();
            std::error_code ec;
            fs::remove(toPath(in.partPath), ec);
            incoming_.erase(it);
            if (cb_.onFailed) {
                cb_.onFailed(id, "more data than announced");
            }
            break;
        }
        in.file->write(reinterpret_cast<const char*>(data + 8), static_cast<std::streamsize>(n));
        in.received += n;
        if (cb_.onProgress && in.received - in.lastReported >= kProgressStep) {
            in.lastReported = in.received;
            cb_.onProgress(id, in.received, in.size);
        }
        break;
    }
    case kEnd: {
        const uint64_t id = r.u64();
        auto it = incoming_.find(id);
        if (!r.ok || it == incoming_.end()) {
            break;
        }
        Incoming in = std::move(it->second);
        incoming_.erase(it);
        in.file->close();
        std::error_code ec;
        if (in.received != in.size || in.file->fail()) {
            fs::remove(toPath(in.partPath), ec);
            if (cb_.onFailed) {
                cb_.onFailed(id, "incomplete");
            }
            break;
        }
        fs::create_directories(toPath(in.finalPath).parent_path(), ec);
        fs::rename(toPath(in.partPath), toPath(in.finalPath), ec);
        if (!ec && !isAcceptedAudioFile(in.finalPath)) {
            // Not what it claims to be: never let it near a decoder.
            fs::remove(toPath(in.finalPath), ec);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                wanted_.erase(id); // and don't ask for it again
            }
            if (cb_.onFailed) {
                cb_.onFailed(id, "not an audio file");
            }
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wanted_.erase(id);
        }
        stats_.filesReceived++;
        if (ec) {
            if (cb_.onFailed) {
                cb_.onFailed(id, "cannot finish cache file");
            }
        } else if (cb_.onReceived) {
            cb_.onReceived(id, in.finalPath);
        }
        break;
    }
    case kError: {
        const uint64_t id = r.u64();
        const std::string reason = r.str16();
        if (!r.ok) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wanted_.erase(id);
        }
        if (cb_.onFailed) {
            cb_.onFailed(id, reason);
        }
        break;
    }
    default:
        break; // unknown frame types are ignored (newer peer)
    }
}

} // namespace b2b
