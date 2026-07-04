#include "Utils.hpp"
#include "Config.hpp"
#include "Sha256.hpp"
#include "Progress.hpp"
#include "Protocol.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <string>
#include <algorithm>
#include <random>
#include <vector>
#include <set>

#if !defined(_WIN32) && !defined(_WIN64)
#include <fcntl.h>   // open, O_EXCL, O_NOFOLLOW
#include <unistd.h>  // read, write, close
#endif

using namespace std::chrono_literals;


namespace Receiver {

// Set by SIGINT/SIGTERM so the receive loop can snapshot progress and exit
// cleanly (writing a file from a signal handler is not async-signal-safe).
volatile sig_atomic_t g_interrupted = 0;

extern "C" void onSignal(int) { g_interrupted = 1; }

void installSignalHandlers() {
    std::signal(SIGINT, onSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, onSignal);
#endif
}

// Hard upper bound on the file size announced by a sender. We allocate the file
// in RAM, so anything larger than this would either fail or cause OOM. Adjust
// only if you know the receiver has enough memory.
constexpr size_t MAX_FILE_LENGTH = 4ULL * 1024 * 1024 * 1024; // 4 GiB

// Session the receiver is currently bound to. The sender stamps a random id into
// every packet; we latch it from the first ANNOUNCE and then reject packets
// from any other session, so a second sender (or a restarted one) cannot mix a
// different file into our buffer and have us report success on garbage.
uint32_t session_id   = 0;
bool     have_session = false;

// Chunk size (the sender's MTU) from the ANNOUNCE. The receiver slices the
// file by this value rather than its own --mtu, so a sender/receiver MTU
// mismatch no longer live-locks the transfer.
size_t chunk_size = 0;

// A packet with our magic but a foreign protocol version is dropped; report it
// once per run so a version-mixed LAN is diagnosable without a warning storm.
void warnVersionOnce(uint8_t seen) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    std::cerr << "Warning: ignoring packets with protocol version "
              << static_cast<int>(seen) << " (this build speaks "
              << static_cast<int>(Protocol::VERSION) << ")" << std::endl;
}

// SHA-256 of the whole file, announced by the sender and checked after reassembly.
uint8_t expected_hash[32] = {0};

// File name announced by the sender, used when the user did not name the output.
std::string announced_name;

// Progress bar for the current transfer and the running total of stored payload
// bytes that drives it.
Progress::Reporter reporter;
size_t received_bytes = 0;

/**
 * List of received parts
 */
std::set<size_t> parts;

size_t totalParts() {
    return Protocol::totalParts(file_length, chunk_size);
}

/**
 * Get empty parts
 */
std::vector<size_t> getEmptyParts() {
    std::vector<size_t> result;
    size_t total_parts = totalParts();
    for (size_t i = 0; i < total_parts; i++) {
        if (parts.find(i) == parts.end()) result.push_back(i);
    }
    return result;
}

// Validate and store one TRANSFER packet, parsing it from scratch — the packet
// may come from the main loop or the recovery loop, and full self-validation
// keeps the two call sites from ever drifting apart. Returns true when a new
// part was stored.
bool handleTransfer(const char* buf, int64_t length) {
    Protocol::Header h;
    if (Protocol::parseHeader(buf, static_cast<size_t>(length), h) != Protocol::Parse::Ok) return false;
    if (h.type != Protocol::Type::Transfer) return false;
    if (length < static_cast<int64_t>(Protocol::TRANSFER_HEADER)) return false;
    if (h.session != session_id) return false;

    size_t part  = Protocol::getU32(buf + Protocol::HEADER_SIZE);
    size_t size  = Protocol::getU32(buf + Protocol::HEADER_SIZE + 4);
    size_t total = totalParts();
    if (part >= total) return false;
    if (parts.find(part) != parts.end()) return false;

    // For non-final parts size must equal the chunk size; for the final part it
    // must equal the remaining bytes. Anything else is malformed and would
    // either silently zero-pad data or write past the file buffer.
    if (size != Protocol::expectedPartSize(part, file_length, chunk_size)) return false;
    if (static_cast<size_t>(length) < size + Protocol::TRANSFER_HEADER) return false;

    parts.insert(part);
    memcpy(file + part * chunk_size, buf + Protocol::TRANSFER_HEADER, size);
    received_bytes += size;
    reporter.update(received_bytes);
    if (verbose) std::cout << "Receive " << part << " part with size " << size << std::endl;
    return true;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

#if !defined(_WIN32) && !defined(_WIN64)
// Flush `fd` to stable storage, retrying on EINTR. On macOS a plain fsync only
// pushes data to the drive — it does NOT force the drive to write its cache to
// permanent media (Apple's fsync(2)); F_FULLFSYNC does, so we prefer it there and
// fall back to fsync when the filesystem doesn't support it (e.g. some network
// mounts). Returns 0 on success, -1 on error.
int durableSync(int fd) {
#if defined(__APPLE__)
    for (;;) {
        if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == ENOTSUP || errno == EINVAL || errno == ENOSYS) break;  // -> fsync
        return -1;
    }
#endif
    for (;;) {
        if (fsync(fd) == 0) return 0;
        if (errno == EINTR) continue;  // interrupted by a signal — retry
        return -1;
    }
}

// Best-effort: flush the directory holding `path` so a completed rename survives
// a crash/power loss — on POSIX a rename is durable only once the parent
// directory entry itself is flushed.
void syncParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? std::string(".")
                    : (slash == 0 ? std::string("/") : path.substr(0, slash));
    int dfd = open(dir.c_str(), O_RDONLY);
    if (dfd < 0) return;
    durableSync(dfd);
    close(dfd);
}
#endif

// Write len bytes to a fresh temp file `tmp`, flushing the data to stable storage
// before returning so the reassembled output survives an OS crash or power loss.
// On POSIX O_EXCL|O_NOFOLLOW means a symlink or pre-existing file planted at that
// path is rejected rather than followed/overwritten. Returns false on I/O error.
bool writeTempFile(const std::string& tmp, const char* data, size_t len) {
    #if defined(_WIN32) || defined(_WIN64)
    std::ofstream out(tmp, std::ofstream::binary | std::ofstream::trunc);
    if (!out.is_open()) return false;
    out.write(data, static_cast<std::streamsize>(len));
    out.close();
    if (!out) return false;
    // std::ofstream::close only reaches the OS cache; force the payload to disk
    // before the move so a power loss can't leave a torn/empty output (MoveFileEx
    // WRITE_THROUGH flushes only cross-volume copies, not a same-volume rename).
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL flushed = FlushFileBuffers(h);
    CloseHandle(h);
    return flushed != 0;
    #else
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t off = 0; off < len; ) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal — retry
            ok = false;
            break;
        }
        if (w == 0) {  // not expected for a regular file
            ok = false;
            break;
        }
        off += static_cast<size_t>(w);
    }
    if (ok && durableSync(fd) != 0) ok = false;  // flush data to disk before rename
    if (close(fd) != 0) ok = false;
    return ok;
    #endif
}

// Atomically move `tmp` onto `target`, replacing any existing file. The payload
// was already flushed by writeTempFile; here we make the rename itself durable.
// On Windows MoveFileEx replaces atomically (a plain rename can't clobber) and
// WRITE_THROUGH flushes the operation; on POSIX rename is atomic and we flush the
// directory so the rename survives a crash. Returns false on failure, leaving the
// verified temp in place.
bool finalizeReplace(const std::string& tmp, const std::string& target) {
    #if defined(_WIN32) || defined(_WIN64)
    return MoveFileExA(tmp.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    #else
    if (std::rename(tmp.c_str(), target.c_str()) != 0) return false;
    syncParentDir(target);
    return true;
    #endif
}

// Write len bytes to a randomly-named temp beside `target`, then atomically move
// it onto `target`. The random suffix keeps a hostile sender from predicting the
// temp path. Returns a process exit code; on finalize failure the verified temp
// is kept so no data is lost.
int writeVerified(const std::string& target, const char* data, size_t len) {
    std::random_device rd;
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ".part.%08x%08x",
             static_cast<unsigned>(rd()), static_cast<unsigned>(rd()));
    const std::string tmp = target + suffix;

    if (!writeTempFile(tmp, data, len)) {
        std::cerr << "Error: Failed to write output file " << tmp << std::endl;
        std::remove(tmp.c_str());
        return 2;
    }
    if (!finalizeReplace(tmp, target)) {
        std::cerr << "Error: Failed to finalize " << target
                  << "; verified data kept at " << tmp << std::endl;
        return 2;
    }
    return 0;
}

// Write len bytes to `path` (stable name, truncating). On POSIX O_NOFOLLOW
// keeps a planted symlink from being followed. Returns false on I/O error.
bool writeRawFile(const std::string& path, const char* data, size_t len) {
    #if defined(_WIN32) || defined(_WIN64)
    std::ofstream out(path, std::ofstream::binary | std::ofstream::trunc);
    if (!out.is_open()) return false;
    out.write(data, static_cast<std::streamsize>(len));
    out.close();
    return static_cast<bool>(out);
    #else
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t off = 0; off < len; ) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal — retry
            ok = false;
            break;
        }
        if (w == 0) {  // not expected for a regular file
            ok = false;
            break;
        }
        off += static_cast<size_t>(w);
    }
    if (close(fd) != 0) ok = false;
    return ok;
    #endif
}

// Read up to `max` bytes of `path` into `out`, reporting the count in `got`.
// On POSIX O_NOFOLLOW refuses to follow a symlink planted at that path, matching
// the write side (writeRawFile/writeVerified) — otherwise a hostile symlink could
// make resume slurp an arbitrary file the receiver can read into the buffer that
// saveSnapshot later writes back out. Returns false on open/read error.
bool readRawFile(const std::string& path, char* out, size_t max, size_t& got) {
    #if defined(_WIN32) || defined(_WIN64)
    std::ifstream in(path, std::ifstream::binary);
    if (!in.is_open()) return false;
    in.read(out, static_cast<std::streamsize>(max));  // Flawfinder: ignore (out sized to max by caller)
    got = static_cast<size_t>(in.gcount());
    return true;
    #else
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    got = 0;
    while (got < max) {
        ssize_t r = read(fd, out + got, max - got);  // Flawfinder: ignore (loop bounded by got < max)
        if (r < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal — retry
            close(fd);
            return false;
        }
        if (r == 0) break;  // EOF before max — caller checks got
        got += static_cast<size_t>(r);
    }
    close(fd);
    return true;
    #endif
}

// snapshot header: "FCIDX1"+NUL(7) + sha256(32) + file_length(8) + chunk(4).
constexpr size_t SNAPSHOT_HEADER = 51;

// Persist the partial buffer to <name>.part and a bitmap of received parts to
// <name>.part.idx, so a later --resume run keyed on the file's SHA-256 can pick
// up where this one left off. Best-effort: a failure only means no resume.
// Gated on --resume so a plain receive never leaves surprise .part files behind
// (nor pays the synchronous whole-buffer write) when interrupted.
void saveSnapshot() {
    if (!resume || file == nullptr || !have_session) return;
    const std::string partPath = fileName + ".part";
    if (!writeRawFile(partPath, file, file_length)) {
        std::cerr << "Warning: could not save resume snapshot to " << partPath << std::endl;
        return;
    }
    size_t total = totalParts();
    std::vector<char> idx(SNAPSHOT_HEADER + (total + 7) / 8, 0);
    memcpy(idx.data(), "FCIDX1", 7);  // 6 chars + trailing NUL
    memcpy(idx.data() + 7, expected_hash, 32);
    Utils::writeBytesFromNumber(idx.data() + 39, file_length, 8);
    Utils::writeBytesFromNumber(idx.data() + 47, chunk_size,  4);
    for (size_t p : parts) idx[SNAPSHOT_HEADER + p / 8] |= static_cast<char>(1 << (p % 8));
    writeRawFile(fileName + ".part.idx", idx.data(), idx.size());
}

// Remove the snapshot once the transfer has completed and been written out.
void discardSnapshot() {
    std::remove((fileName + ".part").c_str());
    std::remove((fileName + ".part.idx").c_str());
}

// Try to load a matching snapshot into `file`/`parts`. Only reuses data whose
// idx records the same SHA-256, size and chunk as the current transfer. Returns
// true on a successful resume (file/parts/received_bytes are populated).
bool tryResume(size_t announced, size_t chunk, const uint8_t hash[32]) {
    size_t total  = Protocol::totalParts(announced, chunk);
    size_t bmSize = (total + 7) / 8;

    // Header + bitmap are a fixed size for this transfer, so read them in one go
    // and require an exact length — a torn/truncated .idx is rejected outright.
    std::vector<char> idx(SNAPSHOT_HEADER + bmSize);
    size_t got = 0;
    if (!readRawFile(fileName + ".part.idx", idx.data(), idx.size(), got)) return false;
    if (got != idx.size()) return false;
    if (memcmp(idx.data(), "FCIDX1", 7) != 0) return false;
    if (memcmp(idx.data() + 7, hash, 32) != 0) return false;
    if (Utils::getNumberFromBytes(idx.data() + 39, 8) != announced) return false;
    if (Utils::getNumberFromBytes(idx.data() + 47, 4) != chunk) return false;
    const char* bm = idx.data() + SNAPSHOT_HEADER;

    char* buf = new (std::nothrow) char[announced];
    if (!buf) return false;
    got = 0;
    if (!readRawFile(fileName + ".part", buf, announced, got) || got != announced) {
        delete[] buf;
        return false;
    }

    delete[] file;
    file = buf;
    parts.clear();
    received_bytes = 0;
    for (size_t p = 0; p < total; ++p) {
        if (bm[p / 8] & (1 << (p % 8))) {
            parts.insert(p);
            received_bytes += Protocol::expectedPartSize(p, announced, chunk);
        }
    }
    return true;
}

// Snapshot the partial file and clean up after Ctrl+C/SIGTERM. Returns 130.
int onInterrupt(char* buffer) {
    reporter.finish();
    saveSnapshot();
    if (resume && file != nullptr) {
        std::cerr << "\nInterrupted; progress saved (retry with --resume)" << std::endl;
    } else {
        std::cerr << "\nInterrupted" << std::endl;
    }
    delete[] buffer;
    delete[] file;
    file = nullptr;
    return 130;
}

// ttl ran out before the sender ever sent FINISH (it crashed, was killed, or the
// network dropped mid-send). Persist any partial progress — a no-op without
// --resume or if nothing was received — so a later --resume can finish it, then
// clean up. Returns 2. Mirrors the checkParts timeout path so both timeouts save.
int onTimeout(char* buffer) {
    reporter.finish();
    saveSnapshot();
    if (resume && file != nullptr) {
        std::cerr << "Transfer timed out; progress saved (retry with --resume)" << std::endl;
    }
    delete[] buffer;
    delete[] file;
    file = nullptr;
    return 2;
}

// ttl expired during recovery with parts still missing. Persist + report. The
// caller has already freed its receive buffer. Returns 2.
int onRecoveryTimeout(size_t missing) {
    reporter.finish();  // clear the bar before the error line
    saveSnapshot();     // keep progress so a later --resume can finish it
    std::cerr << "Error: Transfer timed out with " << missing << " part(s) missing";
    if (resume) std::cerr << "; progress saved (retry with --resume)";
    std::cerr << std::endl;
    delete[] file;
    file = nullptr;
    return 2;
}

// Verify the reassembled file against the announced digest and write it out.
// Returns a process exit code.
int verifyAndWrite() {
    reporter.finish();

    uint8_t got[32];
    Sha256::hash(file, file_length, got);
    if (memcmp(got, expected_hash, 32) != 0) {
        std::cerr << "Error: checksum mismatch — received file is corrupt" << std::endl;
        // A resumed snapshot whose bytes are corrupt (e.g. torn write) would fail
        // here forever; drop it so the next --resume starts clean instead of
        // reloading the same poison.
        if (resume) discardSnapshot();
        delete[] file;
        file = nullptr;
        return 2;
    }

    // Refuse to clobber an existing file unless the user opted in.
    if (!overwrite && fileExists(fileName)) {
        std::cerr << "Error: output file " << fileName
                  << " already exists (use --overwrite to replace it)" << std::endl;
        delete[] file;
        file = nullptr;
        return 2;
    }

    int wc = writeVerified(fileName, file, file_length);
    if (wc != 0) {
        delete[] file;
        file = nullptr;
        return wc;
    }
    discardSnapshot();  // transfer complete — the .part snapshot is no longer needed

    double secs = reporter.elapsed();
    double rate = secs > 0 ? file_length / secs : 0;
    std::cout << "Received " << fileName << " (" << Progress::humanBytes(file_length) << ")";
    if (secs > 0) {
        std::cout << " in " << Progress::humanDuration(secs)
                  << " at " << Progress::humanRate(rate);
    }
    std::cout << "; sha256 verified" << std::endl;

    delete[] file;
    file = nullptr;
    return 0;
}

/**
 * Runs when "FINISH" packet is received
 * Gets empty parts and requests them from the server
 */
// Returns a process exit code: 0 on success, non-zero on failure.
int checkParts() {
    size_t bufcap = std::max(chunk_size + Protocol::TRANSFER_HEADER, static_cast<size_t>(2048));
    char* buffer = new (std::nothrow) char[bufcap];
    if (!buffer) {
        std::cerr << "Error: Can't allocate receive buffer" << std::endl;
        delete[] file;
        file = nullptr;
        return 1;
    }

    std::vector<size_t> emptyParts = getEmptyParts();

    while (ttl && !emptyParts.empty()) {
        if (g_interrupted) return onInterrupt(buffer);
        for (auto index : emptyParts) {
            Protocol::writeHeader(buffer, Protocol::Type::Resend, session_id);
            Protocol::putU32(buffer + Protocol::HEADER_SIZE, static_cast<uint32_t>(index));
            sendto(_socket, buffer, static_cast<int>(Protocol::RESEND_SIZE), 0,
                   reinterpret_cast<sockaddr*>(&broadcast_address),
                   sizeof(broadcast_address));
            if (verbose) std::cout << "Request part of file with index " << index << std::endl;
            if (pace_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(pace_us));
        }

        // Drain every packet already queued (until the socket times out via
        // SO_RCVTIMEO), applying each valid, wanted TRANSFER. Reading only one
        // packet per round made recovery O(N^2) in the number of missing parts:
        // one part recovered per round while re-requesting all of them, so N
        // losses took ~N^2 * delay to recover and buried the sender's replies.
        SOCKADDR_IN sender_address;
        memset(&sender_address, 0, sizeof(sender_address));
        addr_len sender_address_length = sizeof(sender_address);

        bool progressed = false;
        while (true) {
            auto length = recvfrom(_socket, buffer, static_cast<int>(bufcap), 0,
                                   reinterpret_cast<sockaddr*>(&sender_address),
                                   &sender_address_length);
            if (length <= 0) break;  // queue drained this round
            // A foreign protocol version deserves its one warning even when it
            // first shows up during recovery; everything else that is not a
            // TRANSFER for our session (e.g. our own looped-back RESENDs) is
            // rejected by handleTransfer's own validation.
            Protocol::Header h;
            if (Protocol::parseHeader(buffer, static_cast<size_t>(length), h) ==
                Protocol::Parse::BadVersion) {
                warnVersionOnce(h.version);
                continue;
            }
            if (handleTransfer(buffer, static_cast<int64_t>(length))) progressed = true;
        }

        // A new part refreshes ttl; a round with no progress counts down toward
        // the timeout. Our own looped-back RESENDs are not TRANSFERs, so a dead
        // sender still lets recovery terminate instead of livelocking.
        if (progressed) ttl = ttl_max;
        else            ttl--;

        emptyParts = getEmptyParts();
    }

    delete[] buffer;

    if (!emptyParts.empty()) return onRecoveryTimeout(emptyParts.size());

    return verifyAndWrite();
}

// Range-check the announced sizes. Prints and sets exit_code on failure.
bool announceValid(size_t announced, size_t chunk, int& exit_code) {
    if (announced == 0) {
        std::cerr << "Error: Sender announced empty file" << std::endl;
        exit_code = 2;
        return false;
    }
    if (announced > MAX_FILE_LENGTH) {
        std::cerr << "Error: Sender announced file size " << announced
                  << " bytes, exceeds limit of " << MAX_FILE_LENGTH << std::endl;
        exit_code = 2;
        return false;
    }
    // Chunk size must be in the sender's valid --mtu range. The lower bound
    // matters for safety: parts = file_length / chunk_size, so a forged tiny
    // chunk would make getEmptyParts() build a multi-billion-entry vector and
    // OOM/crash the receiver from two tiny packets.
    if (chunk < Protocol::MIN_CHUNK || chunk > Protocol::MAX_CHUNK) {
        std::cerr << "Error: Sender announced invalid chunk size " << chunk << std::endl;
        exit_code = 2;
        return false;
    }
    return true;
}

// Grow the receive buffer so it can hold a full chunk for this transfer.
bool growRecvBuffer(char*& buf, size_t& bufcap, int& exit_code) {
    size_t need = chunk_size + Protocol::TRANSFER_HEADER;
    if (need > bufcap) {
        delete[] buf;
        bufcap = need;
        buf = new (std::nothrow) char[bufcap];
        if (!buf) {
            std::cerr << "Error: Can't allocate receive buffer" << std::endl;
            exit_code = 1;
            return false;
        }
    }
    return true;
}

// Allocate a fresh (empty) file buffer for a new transfer.
bool allocateFileFresh(int& exit_code) {
    delete[] file;
    parts.clear();
    received_bytes = 0;
    file = new (std::nothrow) char[file_length];
    if (!file) {
        std::cerr << "Error: Can't allocate " << file_length << " bytes" << std::endl;
        exit_code = 1;
        return false;
    }
    return true;
}

// Grow the receive buffer and set up the file buffer: resume from a matching
// snapshot when --resume is given, otherwise start fresh. Sets resumed.
bool prepareStorage(char*& buf, size_t& bufcap, size_t announced, size_t chunk,
                    bool& resumed, int& exit_code) {
    if (!growRecvBuffer(buf, bufcap, exit_code)) return false;
    resumed = resume && tryResume(announced, chunk, expected_hash);
    if (!resumed && !allocateFileFresh(exit_code)) return false;
    return true;
}

// Parse and latch an ANNOUNCE (the session id comes pre-parsed from the common
// header). Sets exit_code and returns false if the caller should return that
// code; returns true to continue the receive loop. buf may be reallocated to
// fit the announced chunk size.
bool handleAnnounce(char*& buf, size_t& bufcap, int64_t length, uint32_t incoming_sid,
                    int& exit_code) {
    if (length < static_cast<int64_t>(Protocol::ANNOUNCE_FIXED)) return true;  // too short, ignore

    // An ANNOUNCE from a different session (a second sender, or our own sender
    // restarted) must not clobber the transfer already in progress.
    if (have_session && incoming_sid != session_id) {
        std::cerr << "Warning: ignoring announcement from another sender session" << std::endl;
        return true;
    }

    size_t announced   = Protocol::getU32(buf + Protocol::HEADER_SIZE);
    size_t incoming_cs = Protocol::getU32(buf + Protocol::HEADER_SIZE + 4);
    size_t name_len    = Protocol::getU16(buf + Protocol::HEADER_SIZE + 40);
    if (static_cast<size_t>(length) < Protocol::ANNOUNCE_FIXED + name_len) return true;  // truncated

    // Only a recognised packet from our sender refreshes ttl. Unrecognised
    // traffic (stray broadcasts, other receivers' RESENDs, garbage) deliberately
    // does not, so the timeout stays reachable and a hostile or noisy host cannot
    // keep the receiver alive forever.
    ttl = ttl_max;

    if (!announceValid(announced, incoming_cs, exit_code)) return false;

    // Duplicate retransmission of the same ANNOUNCE: keep accumulated parts.
    if (file != nullptr && announced == file_length && incoming_cs == chunk_size) return true;

    session_id   = incoming_sid;
    have_session = true;
    file_length  = announced;
    chunk_size   = incoming_cs;
    memcpy(expected_hash, buf + Protocol::HEADER_SIZE + 8, 32);
    announced_name = Protocol::sanitizeName(std::string(buf + Protocol::ANNOUNCE_FIXED, name_len));
    if (!fileNameFromCli) fileName = announced_name;

    bool resumed = false;
    if (!prepareStorage(buf, bufcap, announced, incoming_cs, resumed, exit_code)) return false;

    if (verbose) {
        std::cout << "Receive information about new file: " << fileName
                  << " (" << file_length << " bytes)" << std::endl;
        std::cout << "Number of parts: " << totalParts() << std::endl;
    }
    if (resumed) {
        std::cout << "Resuming " << fileName << ": " << parts.size()
                  << "/" << totalParts() << " parts already present" << std::endl;
    }
    reporter.start("Receiving " + fileName, file_length, verbose);
    reporter.update(received_bytes);
    return true;
}


// Latch the "sender finished" state from a FINISH packet for our session.
// The have_session gate matters: session_id starts at 0, so without it a forged
// FINISH with session 0 would terminate any receiver still waiting for its
// ANNOUNCE (a one-packet unauthenticated DoS).
void handleFinish(uint32_t incoming_sid, bool& finish) {
    if (!have_session || incoming_sid != session_id) return;
    ttl = ttl_max;
    if (!finish) {
        if (verbose) std::cout << "Server finished transferring" << std::endl;
        finish = true;
    }
}

// Dispatch one received datagram to the right handler. Returns -1 to keep
// receiving; any value >= 0 is a process exit code the caller should return
// after freeing its buffers. buf may be reallocated to fit the announced chunk.
int dispatchPacket(char*& buf, size_t& bufcap, int64_t length, bool& finish) {
    Protocol::Header h;
    switch (Protocol::parseHeader(buf, static_cast<size_t>(length), h)) {
        case Protocol::Parse::NotOurs:
            return -1;  // stray traffic on our port — not even worth a warning
        case Protocol::Parse::BadVersion:
            warnVersionOnce(h.version);
            return -1;
        case Protocol::Parse::Ok:
            break;
    }

    switch (h.type) {
        case Protocol::Type::Announce: {
            int exit_code = 0;
            if (!handleAnnounce(buf, bufcap, length, h.session, exit_code)) return exit_code;
            break;
        }
        case Protocol::Type::Transfer:
            if (file != nullptr && handleTransfer(buf, length)) ttl = ttl_max;
            break;
        case Protocol::Type::Finish:
            handleFinish(h.session, finish);
            break;
        default:
            break;  // other receivers' RESENDs, or an unknown type: ignore
    }
    return -1;
}

// Returns a process exit code: 0 on success, non-zero on failure.
int run() {
    bool finish = false; // Sender finished transferring

    size_t bufcap = std::max(static_cast<size_t>(2 * mtu), static_cast<size_t>(2048));
    char* buffer = new (std::nothrow) char[bufcap];
    if (!buffer) {
        std::cerr << "Error: Can't allocate receive buffer" << std::endl;
        return 1;
    }

    installSignalHandlers();
    if (!verbose) {
        std::cerr << "Waiting for a sender... (Ctrl+C to cancel)" << std::endl;
    }

    while (ttl > 0) {
        if (g_interrupted) return onInterrupt(buffer);
        // Sender finished transferring — move to the recovery phase (or bail if
        // we joined too late to ever have received the ANNOUNCE).
        if (finish) {
            if (file != nullptr) {
                delete[] buffer;
                return checkParts();
            }
            std::cerr << "Error: Received FINISH without an ANNOUNCE — joined too late" << std::endl;
            delete[] buffer;
            return 2;
        }

        auto length = recvfrom(_socket, buffer, static_cast<int>(bufcap), 0,
                               reinterpret_cast<sockaddr*>(&server_address),
                               &server_address_length);

        // Timeout (SO_RCVTIMEO) or socket error: count down toward giving up.
        if (length < 0) {
            ttl--;
            continue;
        }
        // A zero-length datagram is a valid UDP event; ignore it. Using the
        // recvfrom return value as the loop condition previously let any host
        // terminate the receiver (exit 0, no file written) with one empty packet.
        if (length == 0) {
            continue;
        }

        int code = dispatchPacket(buffer, bufcap, static_cast<int64_t>(length), finish);
        if (code >= 0) {
            delete[] buffer;
            delete[] file;
            file = nullptr;
            return code;
        }
    }
    // A Ctrl+C that landed exactly as ttl hit 0 skips the top-of-loop check
    // (recvfrom's ttl-- dropped ttl to 0 and the while guard exited first), so
    // re-check here to still take the interrupt path (snapshot + exit 130).
    if (g_interrupted) return onInterrupt(buffer);

    // ttl exhausted before FINISH: the transfer did not complete.
    return onTimeout(buffer);
}

} //namespace Receiver
