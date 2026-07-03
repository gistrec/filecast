#include "Utils.hpp"
#include "Config.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <vector>
#include <set>

using namespace std::chrono_literals;


namespace Receiver {

// Hard upper bound on the file size announced by a sender. We allocate the file
// in RAM, so anything larger than this would either fail or cause OOM. Adjust
// only if you know the receiver has enough memory.
constexpr size_t MAX_FILE_LENGTH = 4ULL * 1024 * 1024 * 1024; // 4 GiB

// Session the receiver is currently bound to. The sender stamps a random id into
// every packet; we latch it from the first NEW_PACKET and then reject packets
// from any other session, so a second sender (or a restarted one) cannot mix a
// different file into our buffer and have us report success on garbage.
size_t session_id   = 0;
bool   have_session = false;

/**
 * List of received parts
 */
std::set<size_t> parts;

/**
 * Get empty parts
 */
std::vector<size_t> getEmptyParts() {
    std::vector<size_t> result;
    size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);
    for (size_t i = 0; i < total_parts; i++) {
        if (parts.find(i) == parts.end()) result.push_back(i);
    }
    return result;
}

/**
 * Runs when "FINISH" packet is received
 * Gets empty parts and requests them from the server
 */
// Returns a process exit code: 0 on success, non-zero on failure.
int checkParts() {
    char* buffer = new (std::nothrow) char[2 * mtu];
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
            std::cout << "Request part of file with index " << index << std::endl;
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
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
            auto length = recvfrom(_socket, buffer, 2 * mtu, 0,
                                   reinterpret_cast<sockaddr*>(&sender_address),
                                   &sender_address_length);
            if (length <= 0) break;                              // queue drained this round
            if (strncmp(buffer, "TRANSFER", 8) != 0) continue;   // e.g. our own RESEND
            if (static_cast<size_t>(length) < 20) continue;
            if (Utils::getNumberFromBytes(buffer + 8, 4) != session_id) continue;

            size_t part        = Utils::getNumberFromBytes(buffer + 12, 4);
            size_t size        = Utils::getNumberFromBytes(buffer + 16, 4);
            size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);

            // For non-final parts size must equal MTU; for the final part it must
            // equal the remaining bytes. Anything else is malformed and would
            // either silently zero-pad data or write past the file buffer.
            size_t expected = (part + 1 < total_parts)
                ? static_cast<size_t>(mtu)
                : (file_length - part * static_cast<size_t>(mtu));
            if (part < total_parts && parts.find(part) == parts.end() &&
                size == expected && static_cast<size_t>(length) >= size + 20) {
                parts.insert(part);
                memcpy(file + part * static_cast<size_t>(mtu), buffer + 20, size);
                std::cout << "Receive " << part << " part with size " << size << std::endl;
                progressed = true;
            }
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
        std::cerr << "Error: Transfer timed out, file is incomplete" << std::endl;
        delete[] file;
        file = nullptr;
        return 2;
    }

    std::ofstream output(fileName, std::ofstream::binary);
    if (!output.is_open()) {
        std::cerr << "Error: Can't open output file " << fileName << std::endl;
        delete[] file;
        file = nullptr;
        return 2;
    }
    output.write(file, static_cast<std::streamsize>(file_length));
    if (!output) {
        std::cerr << "Error: Failed to write output file " << fileName << std::endl;
        delete[] file;
        file = nullptr;
        return 2;
    }
    output.close();
    std::cout << "File successfully received" << std::endl;

    delete[] file;
    file = nullptr;
    return 0;
}


// Returns a process exit code: 0 on success, non-zero on failure.
int run() {
    bool finish = false; // Sender finished transferring

    char* buffer = new (std::nothrow) char[2 * mtu];
    if (!buffer) {
        std::cerr << "Error: Can't allocate receive buffer" << std::endl;
        return 1;
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

        auto length = recvfrom(_socket, buffer, 2 * mtu, 0,
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

        if (strncmp(buffer, "NEW_PACKET", 10) == 0 && static_cast<size_t>(length) >= 18) {
            size_t incoming_sid = Utils::getNumberFromBytes(buffer + 10, 4);
            // A NEW_PACKET from a different session (a second sender, or our own
            // sender restarted) must not clobber the transfer already in progress.
            if (have_session && incoming_sid != session_id) {
                std::cerr << "Warning: ignoring NEW_PACKET from another sender session" << std::endl;
                continue;
            }
            // Only a recognised packet from our sender refreshes ttl. Unrecognised
            // traffic (stray broadcasts, other receivers' RESENDs, garbage)
            // deliberately does not, so the timeout stays reachable and a hostile
            // or noisy host cannot keep the receiver alive forever.
            ttl = ttl_max;
            size_t announced = Utils::getNumberFromBytes(buffer + 14, 4);
            if (announced == 0) {
                std::cerr << "Error: Sender announced empty file" << std::endl;
                delete[] buffer;
                return 2;
            }
            if (announced > MAX_FILE_LENGTH) {
                std::cerr << "Error: Sender announced file size " << announced
                          << " bytes, exceeds limit of " << MAX_FILE_LENGTH << std::endl;
                delete[] buffer;
                return 2;
            }

            // Sender retransmits NEW_PACKET a few times for robustness; if we
            // already have a buffer for the same size, treat this as a
            // duplicate and keep accumulated parts.
            if (file != nullptr && announced == file_length) continue;

            session_id   = incoming_sid;
            have_session = true;
            file_length  = announced;

            delete[] file;
            parts.clear();
            file = new (std::nothrow) char[file_length];
            if (!file) {
                std::cerr << "Error: Can't allocate " << file_length << " bytes" << std::endl;
                delete[] buffer;
                return 1;
            }
            memset(file, 0, file_length);

            std::cout << "Receive information about new file size: " << file_length << std::endl;
            std::cout << "Number of parts: " << (file_length + mtu - 1) / mtu << std::endl;
        } else if (strncmp(buffer, "TRANSFER", 8) == 0 && file != nullptr) {
            if (static_cast<size_t>(length) < 20) continue;
            if (Utils::getNumberFromBytes(buffer + 8, 4) != session_id) continue;
            size_t part        = Utils::getNumberFromBytes(buffer + 12, 4); // Read section "index"
            size_t size        = Utils::getNumberFromBytes(buffer + 16, 4); // Read section "size"
            size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);

            if (part >= total_parts) continue;
            // For non-final parts size must equal MTU; for the final part it must
            // equal the remaining bytes. Anything else is malformed and would
            // either silently zero-pad data or write past the file buffer.
            size_t expected = (part + 1 < total_parts)
                ? static_cast<size_t>(mtu)
                : (file_length - part * static_cast<size_t>(mtu));
            if (size != expected) continue;
            if (static_cast<size_t>(length) < size + 20) continue;

            ttl = ttl_max;
            parts.insert(part);
            std::cout << "Receive " << part << " part with size " << size << std::endl;

            memcpy(file + part * static_cast<size_t>(mtu), buffer + 20, size);
        } else if (strncmp(buffer, "FINISH", 6) == 0) {
            if (static_cast<size_t>(length) < 10) continue;
            if (Utils::getNumberFromBytes(buffer + 6, 4) != session_id) continue;
            ttl = ttl_max;
            // If receiver didn't receive a finish message
            if (!finish) {
                std::cout << "Server finished transferring" << std::endl;
                finish = true;
            }
        }
    }
    // ttl exhausted before FINISH: the transfer did not complete.
    delete[] buffer;
    delete[] file;
    file = nullptr;
    return 2;
}

} //namespace Receiver
