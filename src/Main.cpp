#include "cxxopts.hpp"
#include "Config.hpp"
#include "Protocol.hpp"

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

// How packets are addressed: LAN broadcast (default), one host (--to), or an
// IP multicast group (--multicast).
enum class SendMode { Broadcast, Unicast, Multicast };

// Everything parsed from the CLI, validated and ready to act on.
struct CliOptions {
    int         mtu       = 1500;
    int         ttl       = 15;
    int         port      = 33333;
    int         bind_port = 33333;
    int         rate      = 100;        // target send rate, Mbit/s
    int64_t     pace_us   = 0;          // inter-packet pause derived from rate/delay
    std::string file;
    bool        file_from_cli = false;  // true if the user named the file
    SendMode    mode      = SendMode::Broadcast;
    std::string target;                 // IP for unicast / group for multicast
    bool        has_iface = false;      // --iface given (multicast interface pinned)
    std::string iface;                  // its original string, for messages
    struct in_addr iface_addr{};        // parsed --iface (IP_MULTICAST_IF / imr_interface)
    bool        verbose   = false;      // per-packet logging instead of a bar
    bool        overwrite = false;      // allow overwriting an existing file
    bool        resume    = false;      // resume from a .part snapshot
};

void buildOptions(cxxopts::Options& options) {
    options
        .custom_help("send|receive [options]")
        .positional_help("<file>")
        .show_positional_help();

    options.add_options()
        ("f,file",    "File to send, or where to save it when receiving", cxxopts::value<std::string>())
        ("to",        "Send to this IPv4 address instead of LAN broadcast", cxxopts::value<std::string>())
        ("multicast", "Use this IPv4 multicast group (224.0.0.0-239.255.255.255)", cxxopts::value<std::string>())
        ("iface",     "Multicast interface IPv4 address (which NIC to use; --multicast only)", cxxopts::value<std::string>())
        ("p,port",    "Destination UDP port",                 cxxopts::value<int>()->default_value("33333"))
        ("bind-port", "Local UDP port to bind on",            cxxopts::value<int>()->default_value("33333"))
        ("mtu",       "Max packet size in bytes",             cxxopts::value<int>()->default_value("1500"))
        ("ttl",       "Seconds of silence before giving up",  cxxopts::value<int>()->default_value("15"))
        ("rate",      "Target send rate, Mbit/s",             cxxopts::value<int>()->default_value("100"))
        ("delay-ms",  "Override inter-packet pause, ms (advanced, overrides --rate)", cxxopts::value<int>())
        ("v,verbose", "Log every packet instead of a progress bar")
        ("overwrite", "Overwrite an existing output file")
        ("resume",    "Resume an interrupted transfer from its .part snapshot")
        ("h,help",    "Print help")
        ("version",   "Print version");

    options.parse_positional({"file"});
}

void printUsage(cxxopts::Options& options, std::ostream& os) {
    os << options.help();
    os << "\nCommands:\n"
       << "  send <file>       Broadcast <file> to every host on the LAN\n"
       << "  receive [file]    Receive a file (saved under the sender's name by default)\n"
       << "\nExamples:\n"
       << "  filecast send photo.jpg\n"
       << "  filecast receive\n"
       << "  filecast send photo.jpg --to 192.168.1.50 --rate 500\n"
       << "  filecast send photo.jpg --multicast 239.1.2.3\n";
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
    if (opt.mtu < static_cast<int>(Protocol::MIN_CHUNK) ||
        opt.mtu > static_cast<int>(Protocol::MAX_CHUNK)) {
        std::cerr << "Error: --mtu must be between " << Protocol::MIN_CHUNK
                  << " and " << Protocol::MAX_CHUNK << std::endl;
        return false;
    }
    if (opt.ttl <= 0) {
        std::cerr << "Error: --ttl must be greater than 0" << std::endl;
        return false;
    }
    // Cap it so ttl*10s (the sender's resend-phase deadline) can't overflow a
    // steady_clock time_point; a day of silence is already far more than needed.
    if (opt.ttl > 86400) {
        std::cerr << "Error: --ttl must be at most 86400 (24h)" << std::endl;
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
    if (opt.rate <= 0) {
        std::cerr << "Error: --rate must be greater than 0" << std::endl;
        return false;
    }
    return true;
}

// Resolve the destination mode (broadcast / --to unicast / --multicast group)
// and the optional --iface into opt. Prints and returns false on any problem.
bool collectDestination(const cxxopts::ParseResult& result, CliOptions& opt) {
    if (result.count("to") && result.count("multicast")) {
        std::cerr << "Error: --to and --multicast are mutually exclusive" << std::endl;
        return false;
    }
    if (result.count("multicast")) {
        opt.mode = SendMode::Multicast;
        opt.target = result["multicast"].as<std::string>();
    } else if (result.count("to")) {
        opt.mode = SendMode::Unicast;
        opt.target = result["to"].as<std::string>();
    } else {
        opt.mode = SendMode::Broadcast;
    }

    // --iface pins the NIC used for multicast (send + join). It has no meaning
    // for broadcast/unicast, so reject it there rather than silently ignore it.
    if (result.count("iface")) {
        if (opt.mode != SendMode::Multicast) {
            std::cerr << "Error: --iface only applies to --multicast" << std::endl;
            return false;
        }
        opt.iface = result["iface"].as<std::string>();
        if (inet_pton(AF_INET, opt.iface.c_str(), &opt.iface_addr) != 1) {
            std::cerr << "Error: --iface must be a valid IPv4 address" << std::endl;
            return false;
        }
        opt.has_iface = true;
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
    opt.rate      = result["rate"].as<int>();

    if (!validateNumericFlags(opt)) return false;

    // Pace packets by --rate (Mbit/s); an explicit --delay-ms overrides it (used
    // by the loopback tests with 0 to blast at full speed).
    if (result.count("delay-ms")) {
        int delay_ms = result["delay-ms"].as<int>();
        if (delay_ms < 0) {
            std::cerr << "Error: --delay-ms must be 0 or greater" << std::endl;
            return false;
        }
        opt.pace_us = static_cast<int64_t>(delay_ms) * 1000;
    } else {
        opt.pace_us = static_cast<int64_t>(opt.mtu) * 8 / opt.rate;
    }

    opt.verbose   = (result.count("verbose") != 0);
    opt.overwrite = (result.count("overwrite") != 0);
    opt.resume    = (result.count("resume") != 0);

    // send needs an explicit file; receive may take the name from the sender.
    const bool has_file = (result.count("file") != 0);
    if (is_sender && !has_file) {
        std::cerr << "Error: no file to send.\nUsage: filecast send <file>" << std::endl;
        return false;
    }
    opt.file          = has_file ? result["file"].as<std::string>() : std::string();
    opt.file_from_cli = has_file;

    return collectDestination(result, opt);
}

// Fill broadcast_address with the destination and apply mode-specific options
// (enable SO_BROADCAST / validate a multicast group). Returns 0 on success.
int configureDestination(const CliOptions& opt) {
    broadcast_address.sin_family = AF_INET;
    broadcast_address.sin_port = htons(static_cast<uint16_t>(opt.port));

    if (opt.mode == SendMode::Broadcast) {
        int broadcastEnable = 1;
        if (setsockopt(_socket, SOL_SOCKET, SO_BROADCAST,
                       reinterpret_cast<const char*>(&broadcastEnable),
                       sizeof(broadcastEnable)) != 0) {
            std::cerr << "Error: Can't get access to broadcast" << std::endl;
            return 1;
        }
        if (verbose) std::cout << "Ok: Got access to broadcast" << std::endl;
        broadcast_address.sin_addr.s_addr = INADDR_BROADCAST;
        return 0;
    }

    const char* flag = (opt.mode == SendMode::Multicast) ? "--multicast" : "--to";
    if (inet_pton(AF_INET, opt.target.c_str(), &broadcast_address.sin_addr) != 1) {
        std::cerr << "Error: " << flag << " must be a valid IPv4 address" << std::endl;
        return 1;
    }
    if (opt.mode == SendMode::Multicast) {
        // 224.0.0.0/4 — top four bits are 1110. Default IP_MULTICAST_TTL (1) and
        // IP_MULTICAST_LOOP (on) suit a single subnet and same-host tests, so we
        // leave them unset and avoid the platform-specific option-type quirks.
        uint32_t host = ntohl(broadcast_address.sin_addr.s_addr);
        if ((host >> 28) != 0xE) {
            std::cerr << "Error: --multicast must be in 224.0.0.0-239.255.255.255" << std::endl;
            return 1;
        }
        // Pin outgoing multicast (the sender's TRANSFERs, the receiver's RESENDs)
        // to the requested NIC. Without this the kernel picks the egress
        // interface, which on a multi-homed host may be the wrong one.
        if (opt.has_iface &&
            setsockopt(_socket, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&opt.iface_addr),
                       sizeof(opt.iface_addr)) != 0) {
            std::cerr << "Error: Can't set multicast interface " << opt.iface << std::endl;
            return 1;
        }
        if (opt.has_iface && verbose) {
            std::cout << "Ok: Multicast interface " << opt.iface << std::endl;
        }
    }
    return 0;
}

// Join the multicast group so this socket receives packets addressed to it
// (both the receiver's data and the sender's view of RESENDs). No-op otherwise.
int joinMulticast(const CliOptions& opt) {
    if (opt.mode != SendMode::Multicast) return 0;
    struct ip_mreq mreq;
    mreq.imr_multiaddr = broadcast_address.sin_addr;
    // Join on the requested NIC, or let the kernel choose (INADDR_ANY).
    mreq.imr_interface.s_addr = opt.has_iface ? opt.iface_addr.s_addr : INADDR_ANY;
    if (setsockopt(_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        std::cerr << "Error: Can't join multicast group " << opt.target << std::endl;
        return 1;
    }
    if (verbose) std::cout << "Ok: Joined multicast group " << opt.target << std::endl;
    return 0;
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
    if (verbose) std::cout << "Ok: Socket created" << std::endl;

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

    if (configureDestination(opt) != 0) {
        cleanupAndExit(1);
    }

    if (bind(_socket, reinterpret_cast<sockaddr*>(&client_address), sizeof(client_address)) != 0) {
        std::cerr << "Error: Can't bind socket" << std::endl;
        cleanupAndExit(1);
    }
    if (verbose) std::cout << "Ok: Socket bound" << std::endl;

    // Joining the group must happen after bind so the membership sticks to the
    // bound port. Broadcast/unicast modes are no-ops here.
    if (joinMulticast(opt) != 0) {
        cleanupAndExit(1);
    }

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

    mtu             = opt.mtu;
    ttl             = opt.ttl;
    ttl_max         = opt.ttl;
    pace_us         = opt.pace_us;
    fileName        = opt.file;
    fileNameFromCli = opt.file_from_cli;
    verbose         = opt.verbose;
    overwrite       = opt.overwrite;
    resume          = opt.resume;

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
