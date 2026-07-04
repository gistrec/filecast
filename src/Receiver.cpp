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
#include <string>
#include <algorithm>
#include <random>
#include <vector>
#include <set>

#if !defined(_WIN32) && !defined(_WIN64)
#include <fcntl.h>   // open, O_EXCL, O_NOFOLLOW
#endif

using namespace std::chrono_literals;


namespace Receiver {

// Hard upper bound on the file size announced by a sender. We allocate the file
// in RAM, so anything larger than this would either fail or cause OOM. Adjust
// only if you know the receiver has enough memory.
constexpr size_t MAX_FILE_LENGTH = 4ULL * 1024 * 1024 * 1024; // 4 GiB

// Wire protocol the receiver speaks; a NEW_PACKET with any other version is
// rejected rather than misparsed.
constexpr uint16_t PROTOCOL_VERSION = 2;

// NEW_PACKET v2 fixed header (see Sender.cpp) and the TRANSFER header size.
constexpr size_t NEW_PACKET_FIXED = 58;
constexpr size_t TRANSFER_HEADER  = 20;

// Session the receiver is currently bound to. The sender stamps a random id into
// every packet; we latch it from the first NEW_PACKET and then reject packets
// from any other session, so a second sender (or a restarted one) cannot mix a
// different file into our buffer and have us report success on garbage.
size_t session_id   = 0;
bool   have_session = false;

// Chunk size (the sender's MTU) announced in NEW_PACKET. The receiver slices the
// file by this value rather than its own --mtu, so a sender/receiver MTU
// mismatch no longer live-locks the transfer.
size_t chunk_size = 0;

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

// Validate and store one TRANSFER packet. Returns true when a new part was
// stored (shared by the main loop and the recovery loop so their validation
// can never drift apart).
bool handleTransfer(const char* buf, int64_t length) {
    if (length < static_cast<int64_t>(TRANSFER_HEADER)) return false;
    if (Utils::getNumberFromBytes(buf + 8, 4) != session_id) return false;

    size_t part  = Utils::getNumberFromBytes(buf + 12, 4);
    size_t size  = Utils::getNumberFromBytes(buf + 16, 4);
    size_t total = totalParts();
    if (part >= total) return false;
    if (parts.find(part) != parts.end()) return false;

    // For non-final parts size must equal the chunk size; for the final part it
    // must equal the remaining bytes. Anything else is malformed and would
    // either silently zero-pad data or write past the file buffer.
    if (size != Protocol::expectedPartSize(part, file_length, chunk_size)) return false;
    if (static_cast<size_t>(length) < size + TRANSFER_HEADER) return false;

    parts.insert(part);
    memcpy(file + part * chunk_size, buf + TRANSFER_HEADER, size);
    received_bytes += size;
    reporter.update(received_bytes);
    if (verbose) std::cout << "Receive " << part << " part with size " << size << std::endl;
    return true;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// Write len bytes to a randomly-named temp beside `target`, then rename onto it.
// The random suffix keeps a hostile sender from predicting the temp path, and on
// POSIX O_EXCL|O_NOFOLLOW means a symlink planted at that path is never followed.
// Returns a process exit code; on rename failure the verified temp is kept.
int writeVerified(const std::string& target, const char* data, size_t len) {
    std::random_device rd;
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ".part.%08x%08x",
             static_cast<unsigned>(rd()), static_cast<unsigned>(rd()));
    const std::string tmp = target + suffix;

    #if defined(_WIN32) || defined(_WIN64)
    std::ofstream out(tmp, std::ofstream::binary | std::ofstream::trunc);
    if (!out.is_open()) {
        std::cerr << "Error: Can't open output file " << tmp << std::endl;
        return 2;
    }
    out.write(data, static_cast<std::streamsize>(len));
    out.close();
    if (!out) {
        std::cerr << "Error: Failed to write output file " << tmp << std::endl;
        std::remove(tmp.c_str());
        return 2;
    }
    std::remove(target.c_str());  // rename won't clobber on Windows
    #else
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) {
        std::cerr << "Error: Can't create output file " << tmp << std::endl;
        return 2;
    }
    bool ok = true;
    for (size_t off = 0; off < len; ) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal — retry
            ok = false;
            break;
        }
        if (w == 0) { ok = false; break; }  // not expected for a regular file
        off += static_cast<size_t>(w);
    }
    if (close(fd) != 0) ok = false;
    if (!ok) {
        std::cerr << "Error: Failed to write output file " << tmp << std::endl;
        std::remove(tmp.c_str());
        return 2;
    }
    #endif

    if (std::rename(tmp.c_str(), target.c_str()) != 0) {
        std::cerr << "Error: Failed to finalize " << target
                  << "; verified data kept at " << tmp << std::endl;
        return 2;
    }
    return 0;
}

// Verify the reassembled file against the announced digest and write it out.
// Returns a process exit code.
int verifyAndWrite() {
    reporter.finish();

    uint8_t got[32];
    Sha256::hash(file, file_length, got);
    if (memcmp(got, expected_hash, 32) != 0) {
        std::cerr << "Error: checksum mismatch — received file is corrupt" << std::endl;
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
    size_t bufcap = std::max(chunk_size + TRANSFER_HEADER, static_cast<size_t>(2048));
    char* buffer = new (std::nothrow) char[bufcap];
    if (!buffer) {
        std::cerr << "Error: Can't allocate receive buffer" << std::endl;
        delete[] file;
        file = nullptr;
        return 1;
    }

    std::vector<size_t> emptyParts = getEmptyParts();

    while (ttl && !emptyParts.empty()) {
        for (auto index : emptyParts) {
            snprintf(buffer, 7, "RESEND");
            Utils::writeBytesFromNumber(buffer +  6, session_id, 4);
            Utils::writeBytesFromNumber(buffer + 10, index,      4);
            sendto(_socket, buffer, 14, 0, reinterpret_cast<sockaddr*>(&broadcast_address),
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
            if (length <= 0) break;                              // queue drained this round
            if (strncmp(buffer, "TRANSFER", 8) != 0) continue;   // e.g. our own RESEND
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

    if (!emptyParts.empty()) {
        reporter.finish();  // clear the bar before the error line
        std::cerr << "Error: Transfer timed out, file is incomplete" << std::endl;
        delete[] file;
        file = nullptr;
        return 2;
    }

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

// Grow the receive buffer to hold a full chunk and (re)allocate the file buffer
// for a new transfer. Sets exit_code and returns false on allocation failure.
bool allocateBuffers(char*& buf, size_t& bufcap, int& exit_code) {
    size_t need = chunk_size + TRANSFER_HEADER;
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

// Parse and latch a NEW_PACKET. Sets exit_code and returns false if the caller
// should return that code; returns true to continue the receive loop. buf may be
// reallocated to fit the announced chunk size.
bool handleNewPacket(char*& buf, size_t& bufcap, int64_t length, int& exit_code) {
    if (length < static_cast<int64_t>(NEW_PACKET_FIXED)) return true;  // too short, ignore
    if (Utils::getNumberFromBytes(buf + 10, 2) != PROTOCOL_VERSION) {
        std::cerr << "Warning: ignoring NEW_PACKET with unsupported protocol version" << std::endl;
        return true;
    }

    size_t incoming_sid = Utils::getNumberFromBytes(buf + 12, 4);
    // A NEW_PACKET from a different session (a second sender, or our own sender
    // restarted) must not clobber the transfer already in progress.
    if (have_session && incoming_sid != session_id) {
        std::cerr << "Warning: ignoring NEW_PACKET from another sender session" << std::endl;
        return true;
    }

    size_t announced   = Utils::getNumberFromBytes(buf + 16, 4);
    size_t incoming_cs = Utils::getNumberFromBytes(buf + 20, 4);
    size_t name_len    = Utils::getNumberFromBytes(buf + 56, 2);
    if (static_cast<size_t>(length) < NEW_PACKET_FIXED + name_len) return true;  // truncated

    // Only a recognised packet from our sender refreshes ttl. Unrecognised
    // traffic (stray broadcasts, other receivers' RESENDs, garbage) deliberately
    // does not, so the timeout stays reachable and a hostile or noisy host cannot
    // keep the receiver alive forever.
    ttl = ttl_max;

    if (!announceValid(announced, incoming_cs, exit_code)) return false;

    // Duplicate retransmission of the same NEW_PACKET: keep accumulated parts.
    if (file != nullptr && announced == file_length && incoming_cs == chunk_size) return true;

    session_id   = incoming_sid;
    have_session = true;
    file_length  = announced;
    chunk_size   = incoming_cs;
    memcpy(expected_hash, buf + 24, 32);
    announced_name = Protocol::sanitizeName(std::string(buf + NEW_PACKET_FIXED, name_len));
    if (!fileNameFromCli) fileName = announced_name;

    if (!allocateBuffers(buf, bufcap, exit_code)) return false;

    if (verbose) {
        std::cout << "Receive information about new file: " << fileName
                  << " (" << file_length << " bytes)" << std::endl;
        std::cout << "Number of parts: " << totalParts() << std::endl;
    }
    reporter.start("Receiving " + fileName, file_length, verbose);
    return true;
}


// Latch the "sender finished" state from a FINISH packet for our session.
void handleFinish(const char* buf, int64_t length, bool& finish) {
    if (length < 10) return;
    if (Utils::getNumberFromBytes(buf + 6, 4) != session_id) return;
    ttl = ttl_max;
    if (!finish) {
        if (verbose) std::cout << "Server finished transferring" << std::endl;
        finish = true;
    }
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

    if (!verbose) {
        std::cerr << "Waiting for a sender... (Ctrl+C to cancel)" << std::endl;
    }

    while (ttl > 0) {
        // Sender finished transferring — move to the recovery phase (or bail if
        // we joined too late to ever have received NEW_PACKET).
        if (finish) {
            if (file != nullptr) {
                delete[] buffer;
                return checkParts();
            }
            std::cerr << "Error: Received FINISH without NEW_PACKET — joined too late" << std::endl;
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

        if (strncmp(buffer, "NEW_PACKET", 10) == 0) {
            int exit_code = 0;
            if (!handleNewPacket(buffer, bufcap, static_cast<int64_t>(length), exit_code)) {
                delete[] buffer;
                delete[] file;
                file = nullptr;
                return exit_code;
            }
        } else if (strncmp(buffer, "TRANSFER", 8) == 0 && file != nullptr) {
            if (handleTransfer(buffer, static_cast<int64_t>(length))) ttl = ttl_max;
        } else if (strncmp(buffer, "FINISH", 6) == 0) {
            handleFinish(buffer, static_cast<int64_t>(length), finish);
        }
    }
    // ttl exhausted before FINISH: the transfer did not complete.
    reporter.finish();  // clear the bar if one was drawn
    delete[] buffer;
    delete[] file;
    file = nullptr;
    return 2;
}

} //namespace Receiver
