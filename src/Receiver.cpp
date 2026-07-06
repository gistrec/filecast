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
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <fcntl.h>   // open, O_EXCL, O_NOFOLLOW, AT_FDCWD
#include <sys/stat.h>
#include <unistd.h>  // read, write, close, link, unlink
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

// Hard upper bound imposed by the v3 wire format: file_size is a 4-byte field.
// Disk-backed storage removes the RAM pressure, but v3 still cannot represent
// anything larger than this without a protocol bump.
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

// Wall-clock deadline for the current wait, refreshed only by a recognised packet
// from our sender (ANNOUNCE/TRANSFER/FINISH for our session). Deciding the timeout
// on elapsed real time — rather than on how long a single recvfrom sat idle — is
// what keeps it reachable: a host that floods the socket with junk faster than the
// 1s SO_RCVTIMEO never lets recvfrom report an idle read, but junk never pushes
// this deadline either, so the receiver still gives up ttl_max seconds after the
// last real packet. Steady clock, so a system-time jump cannot move it.
std::chrono::steady_clock::time_point receive_deadline;

void refreshDeadline() {
    receive_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_max);
}

bool deadlineExpired() {
    return std::chrono::steady_clock::now() >= receive_deadline;
}

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
std::string part_path;
bool storage_ready = false;
bool storage_failed = false;

#if defined(_WIN32) || defined(_WIN64)
HANDLE part_handle = INVALID_HANDLE_VALUE;
#else
int part_fd = -1;
int durableSync(int fd);
#endif

// Received-parts registry: one bit per part instead of an std::set<size_t>. A
// set cost ~48 bytes per stored part, so a 4 GiB file at a 64-byte chunk (67M
// parts) needed 3+ GB of RAM; the bitmap needs 8 MB. `count` mirrors the number
// of set bits so size() stays O(1). Its byte layout matches the .part.idx
// snapshot bitmap, so save/resume copy it wholesale.
struct PartBitmap {
    std::vector<uint8_t> bits;
    size_t count = 0;

    void reset(size_t total) { bits.assign((total + 7) / 8, 0); count = 0; }
    bool has(size_t i) const { return (bits[i >> 3] >> (i & 7)) & 1u; }
    void set(size_t i) {
        uint8_t m = static_cast<uint8_t>(1u << (i & 7));
        if (!(bits[i >> 3] & m)) { bits[i >> 3] |= m; ++count; }
    }
    size_t size() const { return count; }
};

PartBitmap parts;

size_t totalParts() {
    return Protocol::totalParts(file_length, chunk_size);
}

std::string snapshotPartPath() {
    return fileName + ".part";
}

// Close the part file, optionally flushing to stable storage first. Returns true
// only when the requested flush AND the close both succeed — a failed durable
// sync means the bytes may never reach disk. The handle is always released.
bool closePartFile(bool flush) {
    #if defined(_WIN32) || defined(_WIN64)
    if (part_handle == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    if (flush && !FlushFileBuffers(part_handle)) ok = false;
    if (!CloseHandle(part_handle)) ok = false;
    part_handle = INVALID_HANDLE_VALUE;
    return ok;
    #else
    if (part_fd < 0) return true;
    bool ok = true;
    if (flush && durableSync(part_fd) != 0) ok = false;
    if (close(part_fd) != 0) ok = false;
    part_fd = -1;
    return ok;
    #endif
}

#if defined(_WIN32) || defined(_WIN64)
// Refuse a reparse point/symlink or hardlink; needs the handle opened with
// FILE_FLAG_OPEN_REPARSE_POINT and without truncation.
bool isLoneRegularFile(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
    return info.nNumberOfLinks == 1;
}
#else
// O_NOFOLLOW rejects a symlink but not a pre-planted hardlink; refuse anything
// that isn't a lone regular file so our writes can't reach a linked victim.
bool isLoneRegularFile(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1;
}
#endif

bool openPartFile(bool reuse_existing) {
    closePartFile(false);
    part_path = snapshotPartPath();
    storage_ready = false;
    storage_failed = false;

    #if defined(_WIN32) || defined(_WIN64)
    // OPEN_ALWAYS avoids truncating and FILE_FLAG_OPEN_REPARSE_POINT opens the
    // link itself, so isLoneRegularFile can refuse it before any write.
    DWORD disposition = reuse_existing ? OPEN_EXISTING : OPEN_ALWAYS;
    part_handle = CreateFileA(part_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              disposition, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (part_handle == INVALID_HANDLE_VALUE) return false;

    if (!isLoneRegularFile(part_handle)) {
        closePartFile(false);
        return false;
    }

    LARGE_INTEGER pos;
    if (reuse_existing) {
        if (!GetFileSizeEx(part_handle, &pos) ||
            static_cast<uint64_t>(pos.QuadPart) != static_cast<uint64_t>(file_length)) {
            closePartFile(false);
            return false;
        }
    } else {
        pos.QuadPart = static_cast<LONGLONG>(file_length);
        if (!SetFilePointerEx(part_handle, pos, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(part_handle)) {
            closePartFile(false);
            return false;
        }
    }
    #else
    // No O_TRUNC: it would wipe a pre-planted hardlink's victim before the
    // isLoneRegularFile guard below could refuse it.
    part_fd = open(part_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0644);
    if (part_fd < 0) return false;

    if (!isLoneRegularFile(part_fd)) {
        closePartFile(false);
        return false;
    }
    if (reuse_existing) {
        struct stat st;
        if (fstat(part_fd, &st) != 0 || st.st_size < 0 ||
            static_cast<size_t>(st.st_size) != file_length) {
            closePartFile(false);
            return false;
        }
    } else if (ftruncate(part_fd, static_cast<off_t>(file_length)) != 0) {
        closePartFile(false);
        return false;
    }
    #endif

    storage_ready = true;
    return true;
}

bool writePartAt(size_t offset, const char* data, size_t len) {
    if (!storage_ready) return false;
    #if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(part_handle, pos, nullptr, FILE_BEGIN)) return false;
    size_t off = 0;
    while (off < len) {
        DWORD chunk = (len - off > 0x40000000u) ? 0x40000000u
                                                : static_cast<DWORD>(len - off);
        DWORD wrote = 0;
        if (!WriteFile(part_handle, data + off, chunk, &wrote, nullptr) || wrote == 0) {
            return false;
        }
        off += wrote;
    }
    return true;
    #else
    size_t done = 0;
    while (done < len) {
        ssize_t w = pwrite(part_fd, data + done, len - done,
                           static_cast<off_t>(offset + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        done += static_cast<size_t>(w);
    }
    return true;
    #endif
}

// Hash the received .part file back to compare against the announced digest. The
// caller must have already flushed and closed it (verifyAndWrite does): a fresh
// read handle is served from the page cache, so a failed sync can't be seen here.
bool hashPartFile(uint8_t out[32]) {
    std::ifstream in(part_path, std::ifstream::binary);
    if (!in.is_open()) return false;
    return Sha256::hashStream(in, file_length, out);
}

void removePartFiles() {
    // part_path is set as soon as openPartFile() runs, which only happens while
    // handling an ANNOUNCE. Empty means no part file opened this run — fileName
    // is still "", so removing fileName+".part" would nuke a bare ".part" in cwd.
    if (part_path.empty()) return;
    closePartFile(false);
    std::remove(snapshotPartPath().c_str());
    std::remove((fileName + ".part.idx").c_str());
    storage_ready = false;
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
    if (parts.has(part)) return false;

    // For non-final parts size must equal the chunk size; for the final part it
    // must equal the remaining bytes. Anything else is malformed and would
    // either silently zero-pad data or write past the output file.
    if (size != Protocol::expectedPartSize(part, file_length, chunk_size)) return false;
    if (static_cast<size_t>(length) < size + Protocol::TRANSFER_HEADER) return false;

    if (!writePartAt(part * chunk_size, buf + Protocol::TRANSFER_HEADER, size)) {
        std::cerr << "Error: Failed to write received data to " << part_path << std::endl;
        storage_failed = true;
        return false;
    }
    parts.set(part);
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

// Atomically move `tmp` onto `target`, replacing any existing file. The payload
// was already flushed by the caller; here we make the rename itself durable.
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

// Outcome of the no-clobber finalize: moved into place, refused because the
// target exists, or failed outright.
enum class Finalize { Ok, Exists, Error };

// Atomically move `tmp` onto `target` only if `target` does not exist. The
// existence check and the move must be a single operation: a separate
// exists()+rename() lets a file created between the two be silently replaced,
// voiding the no-overwrite promise. Windows MoveFileEx without REPLACE_EXISTING
// refuses an existing target atomically; on POSIX renameatx_np(RENAME_EXCL)
// (macOS) and link()+unlink() both fail with EEXIST instead of replacing.
// Filesystems supporting neither (FAT, some network mounts) fall back to
// check+rename — non-atomic, but no worse than the pre-atomic behaviour.
Finalize finalizeNoClobber(const std::string& tmp, const std::string& target) {
    #if defined(_WIN32) || defined(_WIN64)
    if (MoveFileExA(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
        return Finalize::Ok;
    }
    DWORD err = GetLastError();
    return (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)
        ? Finalize::Exists : Finalize::Error;
    #else
    #if defined(__APPLE__)
    if (renameatx_np(AT_FDCWD, tmp.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCL) == 0) {
        syncParentDir(target);
        return Finalize::Ok;
    }
    if (errno == EEXIST) return Finalize::Exists;
    // ENOTSUP and friends (e.g. SMB/NFS mounts): fall through to link().
    #endif
    if (link(tmp.c_str(), target.c_str()) == 0) {
        unlink(tmp.c_str());  // best-effort; a leftover tmp loses no data
        syncParentDir(target);
        return Finalize::Ok;
    }
    if (errno == EEXIST) return Finalize::Exists;
    if (fileExists(target)) return Finalize::Exists;
    return finalizeReplace(tmp, target) ? Finalize::Ok : Finalize::Error;
    #endif
}

// Move the verified on-disk .part file into place; the caller already flushed and
// closed it. On failure the .part file is kept so the user does not lose it.
int finalizeVerifiedPart(const std::string& target) {
    if (overwrite) {
        if (finalizeReplace(part_path, target)) {
            storage_ready = false;
            return 0;
        }
        std::cerr << "Error: Failed to finalize " << target
                  << "; verified data kept at " << part_path << std::endl;
        return 2;
    }
    switch (finalizeNoClobber(part_path, target)) {
        case Finalize::Ok:
            storage_ready = false;
            return 0;
        case Finalize::Exists:
            std::cerr << "Error: output file " << target
                      << " already exists (use --overwrite to replace it); "
                      << "verified data kept at " << part_path << std::endl;
            return 2;
        case Finalize::Error:
        default:
            std::cerr << "Error: Failed to finalize " << target
                      << "; verified data kept at " << part_path << std::endl;
            return 2;
    }
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
    // No O_TRUNC: refuse a pre-planted hardlink (isLoneRegularFile) before
    // truncating, so we can't wipe the linked victim.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    if (!isLoneRegularFile(fd) || ftruncate(fd, 0) != 0) {
        close(fd);
        return false;
    }
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
// the write side — otherwise a hostile symlink could make resume trust an
// arbitrary file the receiver can read. Returns false on open/read error.
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

// Flush the partial <name>.part file and persist a bitmap of received parts to
// <name>.part.idx, so a later --resume run keyed on the file's SHA-256 can pick
// up where this one left off. Best-effort: a failure only means no resume.
// Gated on --resume so a plain receive never leaves surprise .part files behind
// (nor pays the synchronous whole-buffer write) when interrupted.
bool saveSnapshot() {
    if (!resume || !storage_ready || !have_session) return false;
    // A failed flush leaves the .part unreliable, so don't advertise a snapshot a
    // later --resume would trust with parts that never durably landed.
    if (!closePartFile(true)) return false;
    size_t total = totalParts();
    size_t bmSize = (total + 7) / 8;
    std::vector<char> idx(SNAPSHOT_HEADER + bmSize, 0);
    memcpy(idx.data(), "FCIDX1", 7);  // 6 chars + trailing NUL
    memcpy(idx.data() + 7, expected_hash, 32);
    Utils::writeBytesFromNumber(idx.data() + 39, file_length, 8);
    Utils::writeBytesFromNumber(idx.data() + 47, chunk_size,  4);
    // The registry bitmap already has the on-disk layout, so copy it wholesale.
    memcpy(idx.data() + SNAPSHOT_HEADER, parts.bits.data(), bmSize);
    return writeRawFile(fileName + ".part.idx", idx.data(), idx.size());
}

// Remove the snapshot once the transfer has completed and been written out.
void discardSnapshot() {
    std::remove((fileName + ".part").c_str());
    std::remove((fileName + ".part.idx").c_str());
}

// Try to load a matching snapshot into the part file and part registry. Only
// reuses data whose idx records the same SHA-256, size and chunk as the current
// transfer. Returns true on a successful resume.
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

    if (!openPartFile(true)) return false;

    parts.reset(total);
    received_bytes = 0;
    for (size_t p = 0; p < total; ++p) {
        if (bm[p / 8] & (1 << (p % 8))) {
            parts.set(p);
            received_bytes += Protocol::expectedPartSize(p, announced, chunk);
        }
    }
    return true;
}

// Snapshot the partial file and clean up after Ctrl+C/SIGTERM. Returns 130.
int onInterrupt(char* buffer) {
    reporter.finish();
    bool saved = saveSnapshot();
    if (saved) {
        std::cerr << "\nInterrupted; progress saved (retry with --resume)" << std::endl;
    } else {
        std::cerr << "\nInterrupted" << std::endl;
        removePartFiles();
    }
    delete[] buffer;
    return 130;
}

// ttl ran out before the sender ever sent FINISH (it crashed, was killed, or the
// network dropped mid-send). Persist any partial progress — a no-op without
// --resume or if nothing was received — so a later --resume can finish it, then
// clean up. Returns 2. Mirrors the checkParts timeout path so both timeouts save.
int onTimeout(char* buffer) {
    reporter.finish();
    bool saved = saveSnapshot();
    if (saved) {
        std::cerr << "Transfer timed out; progress saved (retry with --resume)" << std::endl;
    } else {
        removePartFiles();
    }
    delete[] buffer;
    return 2;
}

// ttl expired during recovery with parts still missing. Persist + report. The
// caller has already freed its receive buffer. Returns 2.
int onRecoveryTimeout(size_t missing) {
    reporter.finish();  // clear the bar before the error line
    bool saved = saveSnapshot();  // keep progress so a later --resume can finish it
    std::cerr << "Error: Transfer timed out with " << missing << " part(s) missing";
    if (saved) std::cerr << "; progress saved (retry with --resume)";
    else removePartFiles();
    std::cerr << std::endl;
    return 2;
}

// Verify the reassembled file against the announced digest and write it out.
// Returns a process exit code.
int verifyAndWrite() {
    reporter.finish();

    // Flush to stable storage before trusting the bytes: the checksum below is
    // served from the page cache, so a failed durable sync would otherwise pass
    // as "sha256 verified" on data that never reached the disk.
    if (!closePartFile(true)) {
        std::cerr << "Error: Failed to flush received data to disk" << std::endl;
        if (resume) discardSnapshot();
        else removePartFiles();
        return 2;
    }

    uint8_t got[32];
    if (!hashPartFile(got)) {
        std::cerr << "Error: Failed to read received file for checksum" << std::endl;
        if (resume) discardSnapshot();
        else removePartFiles();
        return 2;
    }
    if (memcmp(got, expected_hash, 32) != 0) {
        std::cerr << "Error: checksum mismatch — received file is corrupt" << std::endl;
        // A resumed snapshot whose bytes are corrupt (e.g. torn write) would fail
        // here forever; drop it so the next --resume starts clean instead of
        // reloading the same poison.
        if (resume) discardSnapshot();
        else removePartFiles();
        return 2;
    }

    // Refuse to clobber an existing file unless the user opted in. This check
    // fails fast before the atomic no-clobber finalize, which still enforces it
    // if the file appears mid-write.
    if (!overwrite && fileExists(fileName)) {
        std::cerr << "Error: output file " << fileName
                  << " already exists (use --overwrite to replace it)" << std::endl;
        if (!resume) removePartFiles();
        return 2;
    }

    int wc = finalizeVerifiedPart(fileName);
    if (wc != 0) {
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
        removePartFiles();
        return 1;
    }

    size_t total = totalParts();

    // Wall-clock deadline (refreshed only by a recovered part), so a junk flood
    // that keeps recvfrom busy can't freeze recovery the way a per-round counter
    // would. Recovery still gives up ttl_max seconds after the last real part.
    refreshDeadline();

    while (!deadlineExpired() && parts.size() < total) {
        if (g_interrupted) return onInterrupt(buffer);
        // Re-request every still-missing part. Walking the bitmap in place avoids
        // materialising a full missing-parts vector, which for a 67M-part file
        // would be a 512 MB allocation on top of the registry it came from.
        for (size_t index = 0; index < total; ++index) {
            if (parts.has(index)) continue;
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

        while (true) {
            // Bail on Ctrl+C or the deadline before each read, so a junk flood
            // that keeps recvfrom busy can't spin the drain loop forever.
            if (g_interrupted) return onInterrupt(buffer);
            if (deadlineExpired()) break;

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
            // Only a recovered part pushes the deadline; junk and our own
            // looped-back RESENDs don't, so recovery still terminates.
            if (handleTransfer(buffer, static_cast<int64_t>(length))) refreshDeadline();
            if (storage_failed) {
                delete[] buffer;
                if (!resume) removePartFiles();
                return 2;
            }
        }
    }

    delete[] buffer;

    if (parts.size() < total) return onRecoveryTimeout(total - parts.size());

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
    // chunk would size the received-parts bitmap at multiple billions of bits
    // and OOM/crash the receiver from two tiny packets.
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

// Create a fresh on-disk part file for a new transfer.
bool createPartFresh(int& exit_code) {
    closePartFile(false);
    parts.reset(totalParts());
    received_bytes = 0;
    if (!openPartFile(false)) {
        std::cerr << "Error: Can't create temporary output file " << snapshotPartPath() << std::endl;
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
    if (!resumed && !createPartFresh(exit_code)) return false;
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

    // Storage open means we are committed to this session's file. Drop a differing
    // same-session ANNOUNCE before announceValid() and the deadline refresh, so a forged
    // empty/oversized one can neither fail validation and kill the receiver nor
    // hold it open. A matching one just pushes the deadline out; a restart draws a new session.
    if (storage_ready) {
        bool same = announced == file_length && incoming_cs == chunk_size &&
                    memcmp(expected_hash, buf + Protocol::HEADER_SIZE + 8, 32) == 0;
        if (!same) {
            std::cerr << "Warning: ignoring conflicting announcement for the active session"
                      << std::endl;
            return true;
        }
        refreshDeadline();
        return true;
    }

    // Only a recognised packet from our sender pushes the timeout out. Unrecognised
    // traffic (stray broadcasts, other receivers' RESENDs, garbage) deliberately
    // does not, so the deadline stays reachable and a hostile or noisy host cannot
    // keep the receiver alive forever.
    refreshDeadline();

    if (!announceValid(announced, incoming_cs, exit_code)) return false;

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
    refreshDeadline();
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
            if (storage_ready && handleTransfer(buf, length)) refreshDeadline();
            if (storage_failed) return 2;
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

    refreshDeadline();
    while (!deadlineExpired()) {
        if (g_interrupted) return onInterrupt(buffer);
        // Sender finished transferring — move to the recovery phase (or bail if
        // we joined too late to ever have received the ANNOUNCE).
        if (finish) {
            if (storage_ready) {
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

        // Timeout (SO_RCVTIMEO) or socket error: nothing to process this round. The
        // wall-clock deadline — not this idle tick — decides when to give up, so a
        // socket kept busy with junk can no longer starve the countdown.
        if (length < 0) {
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
            if (code != 0 && !resume) removePartFiles();
            return code;
        }
    }
    // A Ctrl+C that landed exactly as the deadline expired skips the top-of-loop
    // check (the while guard saw the deadline first), so re-check here to still
    // take the interrupt path (snapshot + exit 130).
    if (g_interrupted) return onInterrupt(buffer);

    // ttl exhausted before FINISH: the transfer did not complete.
    return onTimeout(buffer);
}

} //namespace Receiver
