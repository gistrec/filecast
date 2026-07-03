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

namespace {

// Result of picking the subcommand off argv[1].
enum class Command { Send, Receive, Handled };

// Everything parsed from the CLI, validated and ready to act on.
struct CliOptions {
    int         mtu       = 1500;
    int         ttl       = 15;
    int         port      = 33333;
    int         bind_port = 33333;
    int         delay_ms  = 20;
    std::string file;
    bool        use_broadcast = true;  // false when --to <ip> is given
    std::string target;                // destination IP for unicast
};

void buildOptions(cxxopts::Options& options) {
    options
        .custom_help("send|receive [options]")
        .positional_help("<file>")
        .show_positional_help();

    options.add_options()
        ("f,file",    "File to send, or where to save it when receiving", cxxopts::value<std::string>())
        ("to",        "Send to this IPv4 address instead of LAN broadcast", cxxopts::value<std::string>())
        ("p,port",    "Destination UDP port",                 cxxopts::value<int>()->default_value("33333"))
        ("bind-port", "Local UDP port to bind on",            cxxopts::value<int>()->default_value("33333"))
        ("mtu",       "Max packet size in bytes",             cxxopts::value<int>()->default_value("1500"))
        ("ttl",       "Seconds of silence before giving up",  cxxopts::value<int>()->default_value("15"))
        ("delay-ms",  "Delay between successive packets, ms", cxxopts::value<int>()->default_value("20"))
        ("h,help",    "Print help")
        ("version",   "Print version");

    options.parse_positional({"file"});
}

void printUsage(cxxopts::Options& options, std::ostream& os) {
    os << options.help();
    os << "\nCommands:\n"
       << "  send <file>       Broadcast <file> to every host on the LAN\n"
       << "  receive [file]    Receive a file (saved as [file], default file.out)\n"
       << "\nExamples:\n"
       << "  filecast send photo.jpg\n"
       << "  filecast receive\n"
       << "  filecast send photo.jpg --to 192.168.1.50\n";
}

// Peel the subcommand off argv[1], handling global --help/--version. Sets
// exit_code and returns Command::Handled when the caller should just exit.
Command pickCommand(int argc, char* argv[], cxxopts::Options& options, int& exit_code) {
    if (argc < 2) {
        std::cerr << "Error: no command given.\n\n";
        printUsage(options, std::cerr);
        exit_code = 1;
        return Command::Handled;
    }
    const std::string command = argv[1];
    if (command == "-h" || command == "--help") {
        printUsage(options, std::cout);
        exit_code = 0;
        return Command::Handled;
    }
    if (command == "--version") {
        std::cout << "filecast " << FILECAST_VERSION << std::endl;
        exit_code = 0;
        return Command::Handled;
    }
    if (command == "send")    return Command::Send;
    if (command == "receive") return Command::Receive;

    std::cerr << "Error: unknown command '" << command << "'. Use 'send' or 'receive'.\n\n";
    printUsage(options, std::cerr);
    exit_code = 1;
    return Command::Handled;
}

// Validate the numeric flags. Prints and returns false on the first bad value.
bool validateNumericFlags(const CliOptions& opt) {
    if (opt.mtu < 64 || opt.mtu > 65507) {
        std::cerr << "Error: --mtu must be between 64 and 65507" << std::endl;
        return false;
    }
    if (opt.ttl <= 0) {
        std::cerr << "Error: --ttl must be greater than 0" << std::endl;
        return false;
    }
    if (opt.port <= 0 || opt.port > 65535) {
        std::cerr << "Error: --port must be between 1 and 65535" << std::endl;
        return false;
    }
    if (opt.bind_port <= 0 || opt.bind_port > 65535) {
        std::cerr << "Error: --bind-port must be between 1 and 65535" << std::endl;
        return false;
    }
    if (opt.delay_ms < 0) {
        std::cerr << "Error: --delay-ms must be 0 or greater" << std::endl;
        return false;
    }
    return true;
}

// Fill a CliOptions from the parsed result and validate it. Prints and returns
// false on any problem.
bool collectOptions(const cxxopts::ParseResult& result, bool is_sender, CliOptions& opt) {
    opt.mtu       = result["mtu"].as<int>();
    opt.ttl       = result["ttl"].as<int>();
    opt.port      = result["port"].as<int>();
    opt.bind_port = result["bind-port"].as<int>();
    opt.delay_ms  = result["delay-ms"].as<int>();

    if (!validateNumericFlags(opt)) return false;

    // send needs an explicit file; receive defaults to file.out.
    if (is_sender && result.count("file") == 0) {
        std::cerr << "Error: no file to send.\nUsage: filecast send <file>" << std::endl;
        return false;
    }
    opt.file = (result.count("file") != 0) ? result["file"].as<std::string>() : "file.out";

    opt.use_broadcast = (result.count("to") == 0);
    opt.target = opt.use_broadcast ? std::string() : result["to"].as<std::string>();
    return true;
}

// Create the socket, apply options and bind. Returns a process exit code
// (0 on success); on failure it has already cleaned up.
int setupSocket(const CliOptions& opt) {
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
    client_address.sin_port = htons(static_cast<uint16_t>(opt.bind_port));
    client_address.sin_addr.s_addr = INADDR_ANY;

    memcpy(&server_address, &client_address, sizeof(server_address));

    broadcast_address.sin_family = AF_INET;
    broadcast_address.sin_port = htons(static_cast<uint16_t>(opt.port));

    if (opt.use_broadcast) {
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
        if (inet_pton(AF_INET, opt.target.c_str(), &broadcast_address.sin_addr) != 1) {
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

    return 0;
}

}  // namespace


int main(int argc, char* argv[]) {
    cxxopts::Options options("filecast", "Send a file to every host on a LAN at once, over UDP");
    buildOptions(options);

    int exit_code = 0;
    const Command command = pickCommand(argc, argv, options, exit_code);
    if (command == Command::Handled) {
        return exit_code;
    }
    const bool is_sender = (command == Command::Send);

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

    if (result.count("help")) {
        printUsage(options, std::cout);
        return 0;
    }
    if (result.count("version")) {
        std::cout << "filecast " << FILECAST_VERSION << std::endl;
        return 0;
    }

    CliOptions opt;
    if (!collectOptions(result, is_sender, opt)) {
        return 1;
    }

    #if defined(_WIN32) || defined(_WIN64)
    WORD socketVer = MAKEWORD(2, 2);
    WSADATA wsaData;
    if (WSAStartup(socketVer, &wsaData) != 0) {
        std::cerr << "Error: WSAStartup failed" << std::endl;
        return 1;
    }
    #endif

    mtu      = opt.mtu;
    ttl      = opt.ttl;
    ttl_max  = opt.ttl;
    delay_ms = opt.delay_ms;
    fileName = opt.file;

    int rc = setupSocket(opt);
    if (rc != 0) {
        return rc;
    }

    // Run receiver or sender; propagate its status as the process exit code so
    // automation (deploy scripts, CI, Ansible) can tell success from failure.
    rc = is_sender ? Sender::run() : Receiver::run();

    CLOSE_SOCKET(_socket);
    CLEANUP_NETWORK();
    return rc;
}
