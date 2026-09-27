#ifndef FILECAST_UTILS_H
#define FILECAST_UTILS_H

#if defined(_WIN32) || defined(_WIN64)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define addr_len int
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#define CLOSE_SOCKET(s) closesocket(s)
#define CLEANUP_NETWORK() WSACleanup()
#else
#define SOCKET int
#define SOCKADDR_IN sockaddr_in
#define addr_len socklen_t
#define INVALID_SOCKET (-1)
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>   // ip_mreq, IP_ADD_MEMBERSHIP, IPPROTO_IP (not guaranteed via arpa/inet.h)
#define CLOSE_SOCKET(s) close(s)
#define CLEANUP_NETWORK() ((void)0)
#endif

#include <cstddef>
#include <cerrno>


namespace Utils {
    /** Error code of the recvfrom() that just failed (Winsock: not errno). */
    inline int lastRecvErrorCode() {
#if defined(_WIN32) || defined(_WIN64)
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    /**
     * Did a failed recvfrom() report a per-datagram event rather than silence?
     * These return immediately, so counting them as idle seconds drains a
     * timeout budget in microseconds (on Winsock an oversized datagram fails
     * the read, and an ICMP reject surfaces on the next one). Unknown codes
     * return false and take the idle path, so a broken socket cannot spin.
     */
    inline bool isDatagramRecvError(int code) {
        // Everything else, WSAETIMEDOUT and EAGAIN included, takes the idle path.
#if defined(_WIN32) || defined(_WIN64)
        return code == WSAEINTR || code == WSAEMSGSIZE || code == WSAECONNRESET ||
               code == WSAENETRESET || code == WSAECONNREFUSED ||
               code == WSAEHOSTUNREACH || code == WSAENETUNREACH;
#else
        return code == EINTR || code == EMSGSIZE || code == ECONNRESET ||
               code == ENETRESET || code == ECONNREFUSED ||
               code == EHOSTUNREACH || code == ENETUNREACH;
#endif
    }

    /**
     * Decode an unsigned integer from a big-endian byte sequence.
     */
    inline size_t getNumberFromBytes(const char* buffer, int count) {
        size_t number = 0;
        for (int i = 0; i < count; i++) {
            number = number << 8;
            number = number | (static_cast<unsigned char>(buffer[i]));
        }
        return number;
    }

    /**
     * Encode an unsigned integer into a big-endian byte sequence. Bytes beyond
     * the width of size_t are written as 0 rather than shifting past the type
     * width, which is undefined behaviour (count > sizeof(size_t) is used by the
     * unit tests and would otherwise trip UBSan).
     */
    inline void writeBytesFromNumber(char* buffer, size_t number, int count) {
        constexpr int bits = static_cast<int>(sizeof(size_t) * 8);
        for (int i = 0; i < count; i++) {
            int shift = i * 8;
            unsigned char byte = (shift < bits)
                ? static_cast<unsigned char>(number >> shift)
                : 0u;
            buffer[count - i - 1] = static_cast<char>(byte);
        }
    }
}

#endif //FILECAST_UTILS_H
