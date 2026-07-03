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
void checkParts() {
    char* buffer = new (std::nothrow) char[2 * mtu];
    if (!buffer) {
        std::cerr << "Error: Can't allocate receive buffer" << std::endl;
        delete[] file;
        file = nullptr;
        return;
    }

    std::vector<size_t> emptyParts = getEmptyParts();

    while (ttl && !emptyParts.empty()) {
        for (auto index : emptyParts) {
            snprintf(buffer, 7, "RESEND");
            Utils::writeBytesFromNumber(buffer + 6, index, 4);
            sendto(_socket, buffer, 10, 0, reinterpret_cast<sockaddr*>(&broadcast_address),
                   sizeof(broadcast_address));
            std::cout << "Request part of file with index " << index << std::endl;
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        SOCKADDR_IN sender_address;
        memset(&sender_address, 0, sizeof(sender_address));
        addr_len sender_address_length = sizeof(sender_address);
        auto length = recvfrom(_socket, buffer, 2 * mtu, 0,
                               reinterpret_cast<sockaddr*>(&sender_address), &sender_address_length);

        if (length <= 0) {
            ttl--;
            continue;
        }

        // Only a valid, wanted TRANSFER counts as progress and refreshes ttl.
        // Everything else — most importantly our own broadcast RESENDs looped
        // back to us by the OS in the default (--broadcast yes, port==bind-port)
        // configuration — must count down toward the timeout. Otherwise, once
        // the sender goes away, the receiver never times out: it endlessly
        // re-requests the same parts and floods the LAN with RESENDs forever.
        bool progressed = false;
        if (strncmp(buffer, "TRANSFER", 8) == 0) {
            size_t part        = Utils::getNumberFromBytes(buffer +  8, 4);
            size_t size        = Utils::getNumberFromBytes(buffer + 12, 4);
            size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);

            // For non-final parts size must equal MTU; for the final part it must
            // equal the remaining bytes. Anything else is malformed and would
            // either silently zero-pad data or write past the file buffer.
            size_t expected = (part + 1 < total_parts)
                ? static_cast<size_t>(mtu)
                : (file_length - part * static_cast<size_t>(mtu));
            if (part < total_parts && size == expected &&
                static_cast<size_t>(length) >= size + 16) {
                parts.insert(part);
                memcpy(file + part * static_cast<size_t>(mtu), buffer + 16, size);
                std::cout << "Receive " << part << " part with size " << size << std::endl;
                progressed = true;
            }
        }

        if (progressed) ttl = ttl_max;
        else            ttl--;

        emptyParts = getEmptyParts();
    }

    delete[] buffer;

    if (!emptyParts.empty()) {
        std::cerr << "Error: Transfer timed out, file is incomplete" << std::endl;
        delete[] file;
        file = nullptr;
        return;
    }

    std::ofstream output(fileName, std::ofstream::binary);
    if (!output.is_open()) {
        std::cerr << "Error: Can't open output file " << fileName << std::endl;
        delete[] file;
        file = nullptr;
        return;
    }
    output.write(file, static_cast<std::streamsize>(file_length));
    if (!output) {
        std::cerr << "Error: Failed to write output file " << fileName << std::endl;
        delete[] file;
        file = nullptr;
        return;
    }
    output.close();
    std::cout << "File successfully received" << std::endl;

    delete[] file;
    file = nullptr;
}


void run() {
    bool finish = false; // Sender finished transferring

    char* buffer = new (std::nothrow) char[2 * mtu];
    if (!buffer) {
        std::cerr << "Error: Can't allocate receive buffer" << std::endl;
        return;
    }

    while (ttl > 0) {
        // Sender finished transferring — move to the recovery phase (or bail if
        // we joined too late to ever have received NEW_PACKET).
        if (finish) {
            if (file != nullptr) {
                delete[] buffer;
                checkParts();
                return;
            }
            std::cerr << "Error: Received FINISH without NEW_PACKET — joined too late" << std::endl;
            delete[] buffer;
            return;
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

        if (strncmp(buffer, "NEW_PACKET", 10) == 0 && static_cast<size_t>(length) >= 14) {
            // Only a recognised protocol packet from the sender refreshes ttl.
            // Unrecognised traffic (stray broadcasts, other receivers' RESENDs,
            // garbage) deliberately does not, so the timeout stays reachable and
            // a hostile or noisy host cannot keep the receiver alive forever.
            ttl = ttl_max;
            size_t announced = Utils::getNumberFromBytes(buffer + 10, 4);
            if (announced == 0) {
                std::cerr << "Error: Sender announced empty file" << std::endl;
                delete[] buffer;
                return;
            }
            if (announced > MAX_FILE_LENGTH) {
                std::cerr << "Error: Sender announced file size " << announced
                          << " bytes, exceeds limit of " << MAX_FILE_LENGTH << std::endl;
                delete[] buffer;
                return;
            }

            // Sender retransmits NEW_PACKET a few times for robustness; if we
            // already have a buffer for the same size, treat this as a
            // duplicate and keep accumulated parts.
            if (file != nullptr && announced == file_length) continue;

            file_length = announced;

            delete[] file;
            parts.clear();
            file = new (std::nothrow) char[file_length];
            if (!file) {
                std::cerr << "Error: Can't allocate " << file_length << " bytes" << std::endl;
                delete[] buffer;
                return;
            }
            memset(file, 0, file_length);

            std::cout << "Receive information about new file size: " << file_length << std::endl;
            std::cout << "Number of parts: " << (file_length + mtu - 1) / mtu << std::endl;
        } else if (strncmp(buffer, "TRANSFER", 8) == 0 && file != nullptr) {
            size_t part        = Utils::getNumberFromBytes(buffer +  8, 4); // Read section "index"
            size_t size        = Utils::getNumberFromBytes(buffer + 12, 4); // Read section "size"
            size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);

            if (part >= total_parts) continue;
            // For non-final parts size must equal MTU; for the final part it must
            // equal the remaining bytes. Anything else is malformed and would
            // either silently zero-pad data or write past the file buffer.
            size_t expected = (part + 1 < total_parts)
                ? static_cast<size_t>(mtu)
                : (file_length - part * static_cast<size_t>(mtu));
            if (size != expected) continue;
            if (static_cast<size_t>(length) < size + 16) continue;

            ttl = ttl_max;
            parts.insert(part);
            std::cout << "Receive " << part << " part with size " << size << std::endl;

            memcpy(file + part * static_cast<size_t>(mtu), buffer + 16, size);
        } else if (strncmp(buffer, "FINISH", 6) == 0) {
            ttl = ttl_max;
            // If receiver didn't receive a finish message
            if (!finish) {
                std::cout << "Server finished transferring" << std::endl;
                finish = true;
            }
        }
    }
    delete[] buffer;
    delete[] file;
    file = nullptr;
}

} //namespace Receiver
