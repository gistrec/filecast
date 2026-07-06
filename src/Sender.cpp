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
#include <string>
#include <algorithm>
#include <map>
#include <random>

using namespace std::chrono_literals;


namespace Sender {

// Random per-transfer id, generated in run() and stamped into every packet so a
// receiver can reject packets from a different sender (a second concurrent
// sender, or a restart) instead of silently mixing two files into one buffer.
uint32_t session_id = 0;

// The file size travels in a 4-byte ANNOUNCE field, so anything that does not
// fit into 32 bits would be silently truncated on the wire (a 4 GiB file would
// be announced as 0). Reject such files up front instead.
constexpr size_t MAX_WIRE_FILE_LENGTH = 0xFFFFFFFFULL;

// Strip any directory part; only the base name travels on the wire so the
// receiver cannot be told to write outside its working directory.
std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Rate-limits re-sends: maps a part index to the second it was last re-sent, so
// a burst of RESEND requests for the same part isn't served more than once a
// second. Populated lazily on the first RESEND for a part, so it grows with the
// packet loss actually seen, not with the file size — a 67M-part file no longer
// preallocates 67M entries (~3 GB) up front.
std::map<size_t, int64_t> sent_part;
std::ifstream input_file;

bool readFileAt(size_t offset, char* out, size_t len) {
    input_file.clear();
    input_file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_file.good()) return false;
    input_file.read(out, static_cast<std::streamsize>(len));  // Flawfinder: ignore (out sized to len by caller)
    return static_cast<size_t>(input_file.gcount()) == len;
}

bool sendPart(size_t part_index) {
    size_t offset        = part_index * static_cast<size_t>(mtu);
    size_t packet_length = file_length - offset;
    if (packet_length > static_cast<size_t>(mtu)) packet_length = static_cast<size_t>(mtu);

    Protocol::writeHeader(buffer, Protocol::Type::Transfer, session_id);
    Protocol::putU32(buffer + Protocol::HEADER_SIZE,     static_cast<uint32_t>(part_index));
    Protocol::putU32(buffer + Protocol::HEADER_SIZE + 4, static_cast<uint32_t>(packet_length));
    if (!readFileAt(offset, buffer + Protocol::TRANSFER_HEADER, packet_length)) {
        std::cerr << "Error: Failed to read part " << part_index
                  << " from " << fileName << std::endl;
        return false;
    }

    auto sent = sendto(_socket, buffer, static_cast<int>(packet_length + Protocol::TRANSFER_HEADER), 0,
                       reinterpret_cast<sockaddr*>(&broadcast_address), sizeof(broadcast_address));
    if (sent < 0) {
        std::cerr << "Warning: Failed to send part " << part_index << std::endl;
        return true;
    }
    if (verbose) {
        std::cout << "Part " << part_index << " with size " << packet_length << " was sent" << std::endl;
    }
    return true;
}

// Pause between packets to pace the transfer (0 = blast at full speed).
void pace() {
    if (pace_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(pace_us));
}

void sendFinish() {
    Protocol::writeHeader(buffer, Protocol::Type::Finish, session_id);
    if (sendto(_socket, buffer, static_cast<int>(Protocol::FINISH_SIZE), 0,
               reinterpret_cast<sockaddr*>(&broadcast_address),
               sizeof(broadcast_address)) < 0) {
        std::cerr << "Warning: Failed to send FINISH" << std::endl;
    }
}

// Open the input file and validate the v3 wire-size limit.
int openInputFile() {
    input_file.open(fileName, std::ios::binary);
    if (!input_file.is_open()) {
        std::cerr << "Error: Can't open file " << fileName << std::endl;
        return 1;
    }

    input_file.seekg(0, std::ios::end);
    auto end_pos = input_file.tellg();
    if (end_pos < 0) {
        std::cerr << "Error: Can't determine file size" << std::endl;
        return 1;
    }
    file_length = static_cast<size_t>(end_pos);
    input_file.seekg(0, std::ios::beg);

    if (file_length == 0) {
        std::cerr << "Error: File is empty" << std::endl;
        return 1;
    }
    if (file_length > MAX_WIRE_FILE_LENGTH) {
        std::cerr << "Error: File is too large (" << file_length
                  << " bytes); the 4-byte wire format supports at most "
                  << MAX_WIRE_FILE_LENGTH << " bytes" << std::endl;
        return 1;
    }
    return 0;
}

bool hashInputFile(uint8_t out[32]) {
    input_file.clear();
    input_file.seekg(0, std::ios::beg);
    if (!Sha256::hashStream(input_file, file_length, out)) {
        std::cerr << "Error: Could not hash entire file" << std::endl;
        return false;
    }

    input_file.clear();
    input_file.seekg(0, std::ios::beg);
    return true;
}

// Build and broadcast the ANNOUNCE (retransmitted a few times, since a single
// drop would strand every receiver and there is no RESEND for it).
bool sendAnnounce() {
    std::random_device rd;
    session_id = static_cast<uint32_t>(rd());

    // Digest of the whole file; the receiver verifies against it after reassembly.
    uint8_t file_hash[32];
    if (!hashInputFile(file_hash)) return false;
    if (verbose) std::cout << "Ok: sha256 " << Sha256::hex(file_hash) << std::endl;

    // Name the receiver saves under unless it was given an explicit output path.
    // Clamp so the whole ANNOUNCE fits the send buffer (2 * mtu) and the length
    // field (a byte count well under 2^16).
    std::string name = baseName(fileName);
    size_t max_name = static_cast<size_t>(2 * mtu) - Protocol::ANNOUNCE_FIXED;
    if (max_name > 255) max_name = 255;
    if (name.size() > max_name) name.resize(max_name);

    Protocol::writeHeader(buffer, Protocol::Type::Announce, session_id);
    Protocol::putU32(buffer + Protocol::HEADER_SIZE,     static_cast<uint32_t>(file_length));
    Protocol::putU32(buffer + Protocol::HEADER_SIZE + 4, static_cast<uint32_t>(mtu));
    memcpy(buffer + Protocol::HEADER_SIZE + 8, file_hash, 32);
    Protocol::putU16(buffer + Protocol::HEADER_SIZE + 40, static_cast<uint16_t>(name.size()));
    memcpy(buffer + Protocol::ANNOUNCE_FIXED, name.data(), name.size());
    int announce_len = static_cast<int>(Protocol::ANNOUNCE_FIXED + name.size());

    for (int i = 0; i < 3; ++i) {
        if (sendto(_socket, buffer, announce_len, 0,
                   reinterpret_cast<sockaddr*>(&broadcast_address),
                   sizeof(broadcast_address)) < 0) {
            std::cerr << "Warning: Failed to send ANNOUNCE" << std::endl;
        }
        if (i + 1 < 3) pace();
    }
    if (verbose) {
        std::cout << "Ok: Sent information about new file with size " << file_length << std::endl;
    }
    return true;
}

// Serve RESEND requests until ttl expires, re-announcing FINISH periodically.
// Returns false on a fatal read error; reports successful re-sends in `resent`.
bool serveResends(size_t total_parts, size_t& resent) {
    int64_t lastFinishSendTime = 0;

    SOCKADDR_IN sender_address;
    memset(&sender_address, 0, sizeof(sender_address));
    addr_len sender_address_length = sizeof(sender_address);

    while (ttl) {
        // Read into the full buffer (2 * mtu). The socket is bound to the same
        // port we broadcast to, so the OS also delivers copies of our own
        // TRANSFER packets here; on Windows a 100-byte buffer made those
        // oversized datagrams fail with WSAEMSGSIZE, which was misread as a
        // timeout and prematurely drained ttl, killing the resend phase.
        auto result = recvfrom(_socket, buffer, 2 * mtu, 0,
                               reinterpret_cast<sockaddr*>(&sender_address), &sender_address_length);

        // No incoming requests for a while - resend FINISH and decrement ttl.
        if (result <= 0) {
            ttl--;
            sendFinish();
            continue;
        }

        // The socket hears our own TRANSFER/FINISH broadcasts too; only RESENDs
        // for our session and current protocol version matter here. A foreign
        // version is reported once — it explains "why is nothing recovering".
        Protocol::Header h;
        Protocol::Parse parsed = Protocol::parseHeader(buffer, static_cast<size_t>(result), h);
        if (parsed == Protocol::Parse::BadVersion) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::cerr << "Warning: ignoring packets with protocol version "
                          << static_cast<int>(h.version) << " (this build speaks "
                          << static_cast<int>(Protocol::VERSION) << ")" << std::endl;
            }
            continue;
        }
        if (parsed != Protocol::Parse::Ok) continue;
        if (h.type != Protocol::Type::Resend) continue;
        if (static_cast<size_t>(result) < Protocol::RESEND_SIZE) continue;
        if (h.session != session_id) continue;
        size_t part = Protocol::getU32(buffer + Protocol::HEADER_SIZE);
        if (part >= total_parts) continue;

        // Monotonic clock: a backward NTP step must not stall RESENDs by making
        // (duration - sent_part[part]) negative for already-served parts.
        auto epoch    = std::chrono::steady_clock::now().time_since_epoch();
        int64_t duration = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();

        ttl = ttl_max;

        if (duration - sent_part[part] >= 1) {
            sent_part[part] = duration;
            if (verbose) {
                std::cout << "Client requested part of file with index " << part << std::endl;
            }
            if (!sendPart(part)) return false;
            ++resent;
        }
        // Re-announce completion at most once a second.
        if (duration - lastFinishSendTime >= 1) {
            lastFinishSendTime = duration;
            sendFinish();
        }
    }
    return true;
}

// Returns a process exit code: 0 on success, non-zero on failure.
int run() {
    buffer = new char[2 * mtu];

    if (openInputFile() != 0) {
        delete[] buffer;
        buffer = nullptr;
        // The socket is main()'s to close (as on every other return path);
        // closing it here too made main()'s CLOSE_SOCKET a double-close.
        return 1;
    }
    if (verbose) std::cout << "Ok: File opened for streaming" << std::endl;

    if (!sendAnnounce()) {
        delete[] buffer;
        buffer = nullptr;
        input_file.close();
        return 1;
    }

    const std::string name = baseName(fileName);
    Progress::Reporter reporter;
    reporter.start("Sending " + name, file_length, verbose);

    size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);
    for (size_t part_index = 0; part_index < total_parts; ++part_index) {
        if (!sendPart(part_index)) {
            reporter.finish();
            delete[] buffer;
            buffer = nullptr;
            input_file.close();
            return 1;
        }
        reporter.update(std::min((part_index + 1) * static_cast<size_t>(mtu), file_length));
        pace();
    }
    reporter.finish();

    double secs = reporter.elapsed();
    double rate = secs > 0 ? file_length / secs : 0;
    std::cout << "Sent " << name << " (" << Progress::humanBytes(file_length)
              << ") in " << Progress::humanDuration(secs)
              << " at " << Progress::humanRate(rate) << std::endl;

    sendFinish();

    size_t resent = 0;
    if (!serveResends(total_parts, resent)) {
        delete[] buffer;
        buffer = nullptr;
        input_file.close();
        return 1;
    }
    if (resent > 0) {
        std::cout << "Re-sent " << resent << " part(s) on request" << std::endl;
    }
    if (verbose) std::cout << "Ok: Transfer session ended" << std::endl;

    delete[] buffer;
    buffer = nullptr;
    input_file.close();
    return 0;
}


} //namespace Sender
