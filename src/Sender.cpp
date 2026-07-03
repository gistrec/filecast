#include "Utils.hpp"
#include "Config.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <map>

using namespace std::chrono_literals;


namespace Sender {

constexpr int HEADER_SIZE = 16;

// The file size is announced in a 4-byte field in NEW_PACKET, so anything that
// does not fit into 32 bits would be silently truncated on the wire (a 4 GiB
// file would be announced as 0). Reject such files up front instead.
constexpr size_t MAX_WIRE_FILE_LENGTH = 0xFFFFFFFFULL;

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
    Utils::writeBytesFromNumber(buffer +  8, part_index,    4); // Write section "number"
    Utils::writeBytesFromNumber(buffer + 12, packet_length, 4); // Write section "length"
    memcpy(buffer + 16, file + offset, packet_length);          // Write section "data"

    auto sent = sendto(_socket, buffer, static_cast<int>(packet_length + HEADER_SIZE), 0,
                       reinterpret_cast<sockaddr*>(&broadcast_address), sizeof(broadcast_address));
    if (sent < 0) {
        std::cerr << "Warning: Failed to send part " << part_index << std::endl;
        return;
    }
    std::cout << "Part " << part_index << " with size " << packet_length << " was sent" << std::endl;
}

void run() {
    buffer = new char[2 * mtu];

    std::ifstream input(fileName, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "Error: Can't open file " << fileName << std::endl;
        delete[] buffer;
        buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        exit(-1);
    }

    input.seekg(0, std::ios::end);
    auto end_pos = input.tellg();
    if (end_pos < 0) {
        std::cerr << "Error: Can't determine file size" << std::endl;
        delete[] buffer;
        buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        exit(-1);
    }
    file_length = static_cast<size_t>(end_pos);
    input.seekg(0, std::ios::beg);

    if (file_length == 0) {
        std::cerr << "Error: File is empty" << std::endl;
        delete[] buffer;
        buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        exit(-1);
    }

    if (file_length > MAX_WIRE_FILE_LENGTH) {
        std::cerr << "Error: File is too large (" << file_length
                  << " bytes); the 4-byte wire format supports at most "
                  << MAX_WIRE_FILE_LENGTH << " bytes" << std::endl;
        delete[] buffer;
        buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        exit(-1);
    }

    file = new (std::nothrow) char[file_length];
    if (!file) {
        std::cerr << "Error: Can't allocate " << file_length << " bytes" << std::endl;
        delete[] buffer;
        buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        exit(-1);
    }
    input.read(file, static_cast<std::streamsize>(file_length));
    if (static_cast<size_t>(input.gcount()) != file_length) {
        std::cerr << "Error: Could not read entire file" << std::endl;
        delete[] file;   file   = nullptr;
        delete[] buffer; buffer = nullptr;
        CLOSE_SOCKET(_socket);
        CLEANUP_NETWORK();
        exit(-1);
    }

    std::cout << "Ok: File successfully copied to RAM" << std::endl;

    snprintf(buffer, 11, "NEW_PACKET");
    Utils::writeBytesFromNumber(buffer + 10, file_length, 4);
    // NEW_PACKET is sent only once (no RESEND mechanism for it), so a single
    // dropped packet would strand every receiver. Retransmit a few times to
    // make the start of the transfer robust to typical LAN loss.
    for (int i = 0; i < 3; ++i) {
        if (sendto(_socket, buffer, 14, 0,
                   reinterpret_cast<sockaddr*>(&broadcast_address),
                   sizeof(broadcast_address)) < 0) {
            std::cerr << "Warning: Failed to send NEW_PACKET" << std::endl;
        }
        if (i + 1 < 3 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

    std::cout << "Ok: Sent information about new file with size " << file_length << std::endl;

    size_t total_parts = (file_length + mtu - 1) / static_cast<size_t>(mtu);

    for (size_t part_index = 0; part_index < total_parts; ++part_index) {
        sent_part.insert({ part_index, 0 });
        sendPart(part_index);
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    snprintf(buffer, 7, "FINISH");
    if (sendto(_socket, buffer, 6, 0,
               reinterpret_cast<sockaddr*>(&broadcast_address),
               sizeof(broadcast_address)) < 0) {
        std::cerr << "Warning: Failed to send FINISH" << std::endl;
    }
    std::cout << "Ok: File transfer complete" << std::endl;

    long lastFinishSendTime = 0; // Last time, when sender sent file transfer completion information

    SOCKADDR_IN sender_address;
    memset(&sender_address, 0, sizeof(sender_address));
    addr_len sender_address_length = sizeof(sender_address);

    while (ttl) {
        auto result = recvfrom(_socket, buffer, 100, 0,
                               reinterpret_cast<sockaddr*>(&sender_address), &sender_address_length);

        // No incoming requests for a while - resend FINISH and decrement ttl.
        if (result <= 0) {
            ttl--;
            snprintf(buffer, 7, "FINISH");
            if (sendto(_socket, buffer, 6, 0,
                       reinterpret_cast<sockaddr*>(&broadcast_address),
                       sizeof(broadcast_address)) < 0) {
                std::cerr << "Warning: Failed to send FINISH" << std::endl;
            }
            continue;
        }

        if (strncmp(buffer, "RESEND", 6) == 0) {
            size_t part = Utils::getNumberFromBytes(buffer + 6, 4);

            if (part >= total_parts) continue;

            auto now      = std::chrono::system_clock::now();
            auto epoch    = now.time_since_epoch();
            long duration = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();

            ttl = ttl_max;

            if (duration - sent_part[part] >= 1) {
                sent_part[part] = duration;
                std::cout << "Client requested part of file with index " << part << std::endl;
                sendPart(part);
            }

            // sending file completion information every second
            if (duration - lastFinishSendTime >= 1) {
                lastFinishSendTime = duration;
                snprintf(buffer, 7, "FINISH");
                if (sendto(_socket, buffer, 6, 0,
                           reinterpret_cast<sockaddr*>(&broadcast_address),
                           sizeof(broadcast_address)) < 0) {
                    std::cerr << "Warning: Failed to send FINISH" << std::endl;
                }
            }
        }
    }
    std::cout << "Ok: Transfer session ended" << std::endl;

    delete[] buffer;
    delete[] file;
    buffer = nullptr;
    file   = nullptr;
}


} //namespace Sender
