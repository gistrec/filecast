#include "Utils.hpp"
#include "Config.hpp"
#include "Sha256.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <string>
#include <map>
#include <random>

using namespace std::chrono_literals;


namespace Sender {

// TRANSFER header: "TRANSFER"(8) + session id(4) + part number(4) + length(4).
constexpr int HEADER_SIZE = 20;

// Wire protocol version, announced in NEW_PACKET so a receiver can reject a
// sender it does not understand instead of misparsing the stream.
constexpr uint16_t PROTOCOL_VERSION = 2;

// NEW_PACKET v2 fixed header, before the variable-length file name:
// "NEW_PACKET"(10) + version(2) + session(4) + file_length(4) + chunk_size(4)
// + sha256(32) + name_len(2) = 58 bytes.
constexpr int NEW_PACKET_FIXED = 58;

// Random per-transfer id, generated in run() and stamped into every packet so a
// receiver can reject packets from a different sender (a second concurrent
// sender, or a restart) instead of silently mixing two files into one buffer.
uint32_t session_id = 0;

// The file size is announced in a 4-byte field in NEW_PACKET, so anything that
// does not fit into 32 bits would be silently truncated on the wire (a 4 GiB
// file would be announced as 0). Reject such files up front instead.
constexpr size_t MAX_WIRE_FILE_LENGTH = 0xFFFFFFFFULL;

// Strip any directory part; only the base name travels on the wire so the
// receiver cannot be told to write outside its working directory.
std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/**
 * Since server may receive many requests for sending some part
 * It is necessary to limit the sending of the same parts for a while
 * This container contains part number and time when the part was sent
 */
std::map<size_t, long> sent_part;

void sendPart(size_t part_index) {
    size_t offset        = part_index * static_cast<size_t>(mtu);
    size_t packet_length = file_length - offset;
    if (packet_length > static_cast<size_t>(mtu)) packet_length = static_cast<size_t>(mtu);

    snprintf(buffer, 9, "TRANSFER");
    Utils::writeBytesFromNumber(buffer +  8, session_id,    4); // Write section "session"
    Utils::writeBytesFromNumber(buffer + 12, part_index,    4); // Write section "number"
    Utils::writeBytesFromNumber(buffer + 16, packet_length, 4); // Write section "length"
    memcpy(buffer + 20, file + offset, packet_length);          // Write section "data"

    auto sent = sendto(_socket, buffer, static_cast<int>(packet_length + HEADER_SIZE), 0,
                       reinterpret_cast<sockaddr*>(&broadcast_address), sizeof(broadcast_address));
    if (sent < 0) {
        std::cerr << "Warning: Failed to send part " << part_index << std::endl;
        return;
    }
    std::cout << "Part " << part_index << " with size " << packet_length << " was sent" << std::endl;
}

void sendFinish() {
    snprintf(buffer, 7, "FINISH");
    Utils::writeBytesFromNumber(buffer + 6, session_id, 4);
    if (sendto(_socket, buffer, 10, 0,
               reinterpret_cast<sockaddr*>(&broadcast_address),
               sizeof(broadcast_address)) < 0) {
        std::cerr << "Warning: Failed to send FINISH" << std::endl;
    }
}

// Load the whole file into the global `file` buffer. Returns 0 on success, or
// an error code after cleaning up the file buffer.
int loadFileIntoMemory() {
    std::ifstream input(fileName, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "Error: Can't open file " << fileName << std::endl;
        return 1;
    }

    input.seekg(0, std::ios::end);
    auto end_pos = input.tellg();
    if (end_pos < 0) {
        std::cerr << "Error: Can't determine file size" << std::endl;
        return 1;
    }
    file_length = static_cast<size_t>(end_pos);
    input.seekg(0, std::ios::beg);

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

    file = new (std::nothrow) char[file_length];
    if (!file) {
        std::cerr << "Error: Can't allocate " << file_length << " bytes" << std::endl;
        return 1;
    }
    input.read(file, static_cast<std::streamsize>(file_length));
    if (static_cast<size_t>(input.gcount()) != file_length) {
        std::cerr << "Error: Could not read entire file" << std::endl;
        delete[] file;
        file = nullptr;
        return 1;
    }
    return 0;
}

// Build and broadcast NEW_PACKET (retransmitted a few times, since a single
// drop would strand every receiver and there is no RESEND for it).
void sendNewPacket() {
    std::random_device rd;
    session_id = static_cast<uint32_t>(rd());

    // Digest of the whole file; the receiver verifies against it after reassembly.
    uint8_t file_hash[32];
    Sha256::hash(file, file_length, file_hash);
    std::cout << "Ok: sha256 " << Sha256::hex(file_hash) << std::endl;

    // Name the receiver saves under unless it was given an explicit output path.
    // Clamp so the whole NEW_PACKET fits the send buffer (2 * mtu) and the length
    // field (a byte count well under 2^16).
    std::string name = baseName(fileName);
    size_t max_name = static_cast<size_t>(2 * mtu) - NEW_PACKET_FIXED;
    if (max_name > 255) max_name = 255;
    if (name.size() > max_name) name.resize(max_name);

    snprintf(buffer, 11, "NEW_PACKET");
    Utils::writeBytesFromNumber(buffer + 10, PROTOCOL_VERSION,          2);
    Utils::writeBytesFromNumber(buffer + 12, session_id,               4);
    Utils::writeBytesFromNumber(buffer + 16, file_length,              4);
    Utils::writeBytesFromNumber(buffer + 20, static_cast<size_t>(mtu), 4);
    memcpy(buffer + 24, file_hash, 32);
    Utils::writeBytesFromNumber(buffer + 56, name.size(),              2);
    memcpy(buffer + NEW_PACKET_FIXED, name.data(), name.size());
    int new_packet_len = NEW_PACKET_FIXED + static_cast<int>(name.size());

    for (int i = 0; i < 3; ++i) {
        if (sendto(_socket, buffer, new_packet_len, 0,
                   reinterpret_cast<sockaddr*>(&broadcast_address),
                   sizeof(broadcast_address)) < 0) {
            std::cerr << "Warning: Failed to send NEW_PACKET" << std::endl;
        }
        if (i + 1 < 3 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    std::cout << "Ok: Sent information about new file with size " << file_length << std::endl;
}

// Serve RESEND requests until ttl expires, re-announcing FINISH periodically.
void serveResends(size_t total_parts) {
    long lastFinishSendTime = 0;

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

        if (strncmp(buffer, "RESEND", 6) != 0) continue;
        // Ignore RESENDs shorter than the header or belonging to a different
        // session (another sender's receivers requesting their own transfer).
        if (result < 14) continue;
        if (Utils::getNumberFromBytes(buffer + 6, 4) != session_id) continue;
        size_t part = Utils::getNumberFromBytes(buffer + 10, 4);
        if (part >= total_parts) continue;

        auto epoch    = std::chrono::system_clock::now().time_since_epoch();
        long duration = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();

        ttl = ttl_max;

        if (duration - sent_part[part] >= 1) {
            sent_part[part] = duration;
            std::cout << "Client requested part of file with index " << part << std::endl;
            sendPart(part);
        }
        // Re-announce completion at most once a second.
        if (duration - lastFinishSendTime >= 1) {
            lastFinishSendTime = duration;
            sendFinish();
        }
    }
}

// Returns a process exit code: 0 on success, non-zero on failure.
int run() {
    buffer = new char[2 * mtu];

    if (loadFileIntoMemory() != 0) {
        delete[] buffer;
        buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        return 1;
    }
    std::cout << "Ok: File successfully copied to RAM" << std::endl;

    sendNewPacket();

    size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);
    for (size_t part_index = 0; part_index < total_parts; ++part_index) {
        sent_part.insert({ part_index, 0 });
        sendPart(part_index);
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    sendFinish();
    std::cout << "Ok: File transfer complete" << std::endl;

    serveResends(total_parts);
    std::cout << "Ok: Transfer session ended" << std::endl;

    delete[] buffer;
    delete[] file;
    buffer = nullptr;
    file   = nullptr;
    return 0;
}


} //namespace Sender
