// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "b2b/net.hpp"
#include "b2b/session.hpp"

namespace b2b {

// A track one peer can offer the other.
struct SharedTrack {
    uint64_t id = 0;      // content fingerprint
    uint64_t size = 0;    // bytes
    std::string name;     // for display, e.g. "Artist - Title"
};

// What the sender's library knows about a track, sent ahead of the file so
// the borrowed copy shows the right name and uses the same beat grid (the
// analysis on two computers can put the grid in slightly different places).
struct TrackMeta {
    std::string title;
    std::string artist;
    std::string key;         // as the sender's app writes it, e.g. "8A" or "Am"
    double bpm = 0;
    std::string beatsVersion; // the app's own beat grid format, opaque here
    std::string beatsSubVersion;
    std::vector<uint8_t> beats;
};

// A file name made from `name` that is safe on every OS: no path separators,
// reserved characters or control characters, at most ~80 bytes, never empty.
std::string safeFileName(const std::string& name, const std::string& fallback);

// True if `path` is an audio file we accept from a peer: an allowed extension
// (mp3, flac, wav, aiff, ogg, opus, m4a, mp4, aac, wv, alac) AND contents that
// start the way that format does. The audio decoders are the code a hostile
// file would attack, so nothing else ever reaches them.
bool isAcceptedAudioFile(const std::string& path);

struct FileShareConfig {
    // Usual: share over the session's own connection (works through NAT and
    // the relay). Without a session, the old TCP link below is used.
    Session* session = nullptr;
    Role role = Role::Leader;
    uint16_t listenPort = 9001;  // leader
    std::string peerHost;        // follower
    uint16_t peerPort = 9001;    // follower
    std::string cacheDir;        // where received files are written (UTF-8 path)
    uint64_t maxFileBytes = 300ull * 1024 * 1024;  // refuse anything bigger
    // Upload cap. The session's stream also backs off by itself as soon as
    // the connection starts queueing (which would delay the sync traffic),
    // so this only needs to keep a fast link from being flooded.
    uint64_t rateBytesPerSec = 12ull * 1024 * 1024;
    // Through the server's relay: it lets a session send 4 MB/s, both ways
    // together, so each side stays well under half of that.
    uint64_t relayRateBytesPerSec = 1536ull * 1024;
};

// Two-way track sharing between the peers, over the session's reliable
// stream (or, without a session, a TCP connection: the follower connects to
// the leader). An upload cap keeps a big transfer from crowding out the
// timing-critical sync messages. Once connected the link is symmetric:
// either side can offer a track list, hint that it is using a track, and
// request files from the other.
//
// Files are only ever served by fingerprint through `lookup`, which the app
// limits to tracks the user chose to share. A peer can never ask for an
// arbitrary path.
//
// All callbacks run on the network thread.
class FileShare {
public:
    struct Callbacks {
        // Our track by fingerprint: fill `path` (UTF-8) and return true if we may send it.
        std::function<bool(uint64_t id, std::string& path)> lookup;
        // Optional: details of our track, sent along with it.
        std::function<bool(uint64_t id, TrackMeta& meta)> describe;
        std::function<void(std::vector<SharedTrack>)> onPeerList;
        // Details of a track the peer is about to send.
        std::function<void(uint64_t id, TrackMeta meta)> onPeerMeta;
        // The peer is using (loading/previewing) a track; priority 0 = most urgent.
        std::function<void(uint64_t id, uint8_t priority)> onPeerHint;
        std::function<void(uint64_t id, uint64_t received, uint64_t total)> onProgress;
        std::function<void(uint64_t id, std::string path)> onReceived;
        std::function<void(uint64_t id, std::string reason)> onFailed;
        std::function<void(bool connected)> onConnection;
    };

    FileShare(FileShareConfig config, Callbacks callbacks);
    ~FileShare();

    bool start();
    void stop();

    // ---- Any thread ----
    void setLocalTracks(std::vector<SharedTrack> tracks); // (re)sent to the peer
    void request(uint64_t id, uint8_t priority);          // ask the peer for a file
    void hint(uint64_t id, uint8_t priority);             // tell the peer we use a track
    void cancel(uint64_t id);                              // no longer needed
    bool connected() const { return connected_.load(); }

    struct Stats {
        std::atomic<uint64_t> bytesSent{0};
        std::atomic<uint64_t> bytesReceived{0};
        std::atomic<uint32_t> filesSent{0};
        std::atomic<uint32_t> filesReceived{0};
    };
    const Stats& stats() const { return stats_; }

    static constexpr uint32_t kProtocolVersion = 1;

private:
    struct Outgoing {
        uint64_t id;
        uint8_t priority;
        uint64_t size;
        uint64_t sent;
        uint64_t order;
        std::unique_ptr<std::ifstream> file;
    };
    struct Incoming {
        uint64_t size;
        uint64_t received;
        uint64_t lastReported;
        std::string partPath;
        std::string finalPath;
        std::unique_ptr<std::ofstream> file;
    };

    void run();
    void onConnected();
    void onDisconnected();
    bool pump(ByteStream& stream);
    void handleFrame(uint8_t type, const uint8_t* data, size_t length);
    void startSending(uint64_t id, uint8_t priority);
    void queueMeta(uint64_t id);
    void queueFrame(uint8_t type, const std::vector<uint8_t>& payload);
    bool fillWriteBuffer();

    FileShareConfig config_;
    Callbacks cb_;

    std::mutex mutex_; // guards the "wanted" state shared with other threads
    std::vector<SharedTrack> localTracks_;
    bool listDirty_ = false;
    std::map<uint64_t, uint8_t> wanted_;      // requested from peer: id -> priority
    std::vector<std::pair<uint64_t, uint8_t>> newRequests_, newHints_;
    std::vector<uint64_t> newCancels_;

    // Network thread only
    std::unique_ptr<TcpSocket> listener_;
    std::unique_ptr<ByteStream> conn_;
    uint32_t epoch_ = 0; // session mode: the stream we're on
    std::deque<std::vector<uint8_t>> controlQueue_;
    std::vector<uint8_t> wbuf_;
    size_t wpos_ = 0;
    std::vector<uint8_t> rbuf_;
    std::map<uint64_t, Outgoing> outgoing_;
    std::map<uint64_t, Incoming> incoming_;
    std::map<uint64_t, std::string> peerNames_; // from the peer's list: names the cached file
    uint64_t sendOrder_ = 0;
    double tokens_ = 0;
    Nanos lastRefill_ = 0;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    Stats stats_;
};

} // namespace b2b
