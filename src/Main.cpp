#include "cxxopts.hpp"
#include "Config.hpp"

#include <iostream>
#include <string>
#include <cstdlib>

#ifndef FILECAST_VERSION
#define FILECAST_VERSION "1.0.0"
#endif

namespace Receiver { int run(); }
namespace Sender   { int run(); }


static void cleanupAndExit(int code) {
    if (_socket != INVALID_SOCKET) {
        CLOSE_SOCKET(_socket);
    }
    CLEANUP_NETWORK();
    std::exit(code);
}


int main(int argc, char* argv[]) {
    // Parsing input parameters from the CLI
    cxxopts::Options options("filecast", "UDP broadcast file transfer over LAN");

    options
        .positional_help("[optional args]")
        .show_positional_help();

    options.add_options()
        ("f,file",     "File name",                             cxxopts::value<std::string>()->default_value("file.out"))
        ("t,type",     "Receiver or sender",                    cxxopts::value<std::string>()->default_value("sender"))
        ("broadcast",  "Broadcast address",                     cxxopts::value<std::string>()->default_value("yes"))
        ("p,port",     "Destination port for outgoing packets", cxxopts::value<int>()->default_value("33333"))
        ("bind-port",  "Local port to bind on",                 cxxopts::value<int>()->default_value("33333"))
        ("mtu",        "MTU packet",                            cxxopts::value<int>()->default_value("1500"))
        ("ttl",        "Time to live",                          cxxopts::value<int>()->default_value("15"))
        ("delay-ms",   "Delay between successive packets, ms",  cxxopts::value<int>()->default_value("20"))
        ("h,help",     "Print help")
        ("version",    "Print version");

    auto result = [&]() -> cxxopts::ParseResult {
        try {
            return options.parse(argc, argv);
        } catch (const cxxopts::exceptions::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            std::exit(1);
        }
    }();

    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    if (result.count("version")) {
        std::cout << "filecast " << FILECAST_VERSION << std::endl;
        return 0;
    }

    // Validate CLI parameters before touching sockets so we can fail fast
    int parsed_mtu       = result["mtu"].as<int>();
    int parsed_ttl       = result["ttl"].as<int>();
    int parsed_port      = result["port"].as<int>();
    int parsed_bind_port = result["bind-port"].as<int>();
    int parsed_delay_ms  = result["delay-ms"].as<int>();

    if (parsed_mtu < 64 || parsed_mtu > 65507) {
        std::cerr << "Error: --mtu must be between 64 and 65507" << std::endl;
        return 1;
    }
    if (parsed_ttl <= 0) {
        std::cerr << "Error: --ttl must be greater than 0" << std::endl;
        return 1;
    }
    if (parsed_port <= 0 || parsed_port > 65535) {
        std::cerr << "Error: --port must be between 1 and 65535" << std::endl;
        return 1;
    }
    if (parsed_bind_port <= 0 || parsed_bind_port > 65535) {
        std::cerr << "Error: --bind-port must be between 1 and 65535" << std::endl;
        return 1;
    }
    if (parsed_delay_ms < 0) {
        std::cerr << "Error: --delay-ms must be 0 or greater" << std::endl;
        return 1;
    }

    const std::string type = result["type"].as<std::string>();
    if (type != "sender" && type != "receiver") {
        std::cerr << "Error: --type must be 'sender' or 'receiver'" << std::endl;
        return 1;
    }

    const std::string broadcast_arg = result["broadcast"].as<std::string>();

    #if defined(_WIN32) || defined(_WIN64)
    WORD socketVer = MAKEWORD(2, 2);
    WSADATA wsaData;
    if (WSAStartup(socketVer, &wsaData) != 0) {
        std::cerr << "Error: WSAStartup failed" << std::endl;
        return 1;
    }
    #endif

    mtu      = parsed_mtu;
    ttl      = parsed_ttl;
    ttl_max  = parsed_ttl;
    delay_ms = parsed_delay_ms;
    fileName = result["file"].as<std::string>();

    _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (_socket == INVALID_SOCKET) {
        std::cerr << "Error: Can't create socket" << std::endl;
        CLEANUP_NETWORK();
        return 1;
    }
    std::cout << "Ok: Socket created" << std::endl;

    int reuseAddr = 1;
    if (setsockopt(_socket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr)) != 0) {
        std::cerr << "Warning: Failed to set SO_REUSEADDR" << std::endl;
    }
    #ifdef SO_REUSEPORT
    int reusePort = 1;
    if (setsockopt(_socket, SOL_SOCKET, SO_REUSEPORT,
                   reinterpret_cast<const char*>(&reusePort), sizeof(reusePort)) != 0) {
        std::cerr << "Warning: Failed to set SO_REUSEPORT" << std::endl;
    }
    #endif

    client_address.sin_family = AF_INET;
    client_address.sin_port = htons(static_cast<uint16_t>(parsed_bind_port));
    client_address.sin_addr.s_addr = INADDR_ANY;

    memcpy(&server_address, &client_address, sizeof(server_address));

    broadcast_address.sin_family = AF_INET;
    broadcast_address.sin_port = htons(static_cast<uint16_t>(parsed_port));

    if (broadcast_arg == "yes") {
        int broadcastEnable = 1;
        if (setsockopt(_socket, SOL_SOCKET, SO_BROADCAST,
                       reinterpret_cast<const char*>(&broadcastEnable),
                       sizeof(broadcastEnable)) != 0) {
            std::cerr << "Error: Can't get access to broadcast" << std::endl;
            cleanupAndExit(1);
        }
        std::cout << "Ok: Got access to broadcast" << std::endl;
        broadcast_address.sin_addr.s_addr = INADDR_BROADCAST;
    } else {
        if (inet_pton(AF_INET, broadcast_arg.c_str(), &broadcast_address.sin_addr) != 1) {
            std::cerr << "Error: --broadcast must be 'yes' or a valid IPv4 address" << std::endl;
            cleanupAndExit(1);
        }
    }

    if (bind(_socket, reinterpret_cast<sockaddr*>(&client_address), sizeof(client_address)) != 0) {
        std::cerr << "Error: Can't bind socket" << std::endl;
        cleanupAndExit(1);
    }
    std::cout << "Ok: Socket bound" << std::endl;

    #if defined(_WIN32) || defined(_WIN64)
    DWORD tv = 1000;  // user timeout in milliseconds [ms]
    setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
    #else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
    #endif

    // Run receiver or sender; propagate its status as the process exit code so
    // automation (deploy scripts, CI, Ansible) can tell success from failure.
    int rc = (type == "receiver") ? Receiver::run() : Sender::run();

    CLOSE_SOCKET(_socket);
    CLEANUP_NETWORK();
    return rc;
}
