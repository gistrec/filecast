#ifndef FILECAST_CONFIG_H
#define FILECAST_CONFIG_H

#include "Utils.hpp"

#include <cstdint>
#include <string>


extern std::string fileName;     // File Name to transfer or receive
extern bool        fileNameFromCli; // true if the user gave an explicit output name
extern bool        verbose;      // per-packet logging instead of a progress bar
extern bool        overwrite;    // allow overwriting an existing output file
extern bool        resume;       // resume from a .part snapshot if one matches

extern int mtu;     // Max packet size to send and receive

extern int ttl;     // Current wait time for new packages before shutting down
extern int ttl_max; // Maximum wait time for new packages before shutting down

extern int64_t pace_us; // Inter-packet pause in microseconds (from --rate/--delay-ms)

extern SOCKET _socket;

extern SOCKADDR_IN server_address;
extern SOCKADDR_IN client_address;
extern SOCKADDR_IN broadcast_address;

extern addr_len server_address_length;
extern addr_len client_address_length;

extern size_t file_length; // File size in bytes
extern char*  buffer;      // Packet buffer

#endif //FILECAST_CONFIG_H
