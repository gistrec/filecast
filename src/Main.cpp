#include "cxxopts.hpp"
#include "Config.hpp"

#include <iostream>
#include <ostream>
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
    // The interface is subcommand-based: `filecast send <file>` /
    // `filecast receive [file]`. cxxopts has no native subcommands, so we peel
    // the command off argv[1] ourselves and let cxxopts parse the rest (options
    // plus the positional <file>).
    cxxopts::Options options("filecast", "Send a file to every host on a LAN at once, over UDP");

    options
        .custom_help("send|receive [options]")
        .positional_help("<file>")
        .show_positional_help();

    options.add_options()
        ("f,file",     "File to send, or where to save it when receiving", cxxopts::value<std::string>())
        ("to",         "Send to this IPv4 address instead of LAN broadcast", cxxopts::value<std::string>())
        ("p,port",     "Destination UDP port",                  cxxopts::value<int>()->default_value("33333"))
        ("bind-port",  "Local UDP port to bind on",             cxxopts::value<int>()->default_value("33333"))
        ("mtu",        "Max packet size in bytes",              cxxopts::value<int>()->default_value("1500"))
        ("ttl",        "Seconds of silence before giving up",   cxxopts::value<int>()->default_value("15"))
        ("delay-ms",   "Delay between successive packets, ms",  cxxopts::value<int>()->default_value("20"))
        ("h,help",     "Print help")
        ("version",    "Print version");

    options.parse_positional({"file"});

    auto printUsage = [&](std::ostream& os) {
        os << options.help();
        os << "\nCommands:\n"
           << "  send <file>       Broadcast <file> to every host on the LAN\n"
           << "  receive [file]    Receive a file (saved as [file], default file.out)\n"
           << "\nExamples:\n"
           << "  filecast send photo.jpg\n"
           << "  filecast receive\n"
           << "  filecast send photo.jpg --to 192.168.1.50\n";
    };

    // Global help/version before a subcommand: `filecast --help`, `filecast --version`.
    if (argc < 2) {
        std::cerr << "Error: no command given.\n\n";
        printUsage(std::cerr);
        return 1;
    }
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") {
        printUsage(std::cout);
        return 0;
    }
    if (command == "--version") {
        std::cout << "filecast " << FILECAST_VERSION << std::endl;
        return 0;
    }
    if (command != "send" && command != "receive") {
        std::cerr << "Error: unknown command '" << command << "'. Use 'send' or 'receive'.\n\n";
        printUsage(std::cerr);
        return 1;
    }
    const bool isSender = (command == "send");

    // Parse everything after the subcommand. Shifting by one makes cxxopts treat
    // the subcommand token as argv[0] (the program name it ignores), so options
    // and the positional <file> are parsed from argv[2..].
    auto result = [&]() -> cxxopts::ParseResult {
        try {
            return options.parse(argc - 1, argv + 1);
        } catch (const cxxopts::exceptions::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            std::exit(1);
        }
    }();

    // `filecast send --help` / `filecast receive --version`.
    if (result.count("help")) {
        printUsage(std::cout);
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

    // `send` needs an explicit file; `receive` defaults to file.out when omitted.
    if (isSender && !result.count("file")) {
        std::cerr << "Error: no file to send.\nUsage: filecast send <file>" << std::endl;
        return 1;
    }
    const std::string parsed_file =
        result.count("file") ? result["file"].as<std::string>() : "file.out";

    // No --to means LAN broadcast (the default); --to <ip> sends to one host.
    const bool useBroadcast = (result.count("to") == 0);
    const std::string target = useBroadcast ? std::string() : result["to"].as<std::string>();

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
    fileName = parsed_file;

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

    if (useBroadcast) {
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
        if (inet_pton(AF_INET, target.c_str(), &broadcast_address.sin_addr) != 1) {
            std::cerr << "Error: --to must be a valid IPv4 address" << std::endl;
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
    int rc = isSender ? Sender::run() : Receiver::run();

    CLOSE_SOCKET(_socket);
    CLEANUP_NETWORK();
    return rc;
}
