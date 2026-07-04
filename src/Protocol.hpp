#ifndef FILECAST_PROTOCOL_H
#define FILECAST_PROTOCOL_H

// Pure, socket-free protocol helpers shared by the receiver and the unit tests.
// Keeping the framing/validation logic here (rather than inline in Receiver.cpp)
// makes it testable without spinning up sockets.

#include <cstddef>
#include <cctype>
#include <algorithm>
#include <string>

namespace Protocol {

// Valid chunk size range (the sender's --mtu bounds). The lower bound also caps
// the part count, so a forged tiny chunk cannot blow up the receiver.
constexpr size_t MIN_CHUNK = 64;
constexpr size_t MAX_CHUNK = 65507;

inline size_t totalParts(size_t file_length, size_t chunk_size) {
    if (chunk_size == 0) return 0;  // guard the reusable helper against misuse
    return (file_length + chunk_size - 1) / chunk_size;
}

// Expected payload size of `part`: the full chunk for every part but the last,
// and the remaining bytes for the last part.
inline size_t expectedPartSize(size_t part, size_t file_length, size_t chunk_size) {
    size_t total = totalParts(file_length, chunk_size);
    return (part + 1 < total) ? chunk_size : (file_length - part * chunk_size);
}

// Are the announced file/chunk sizes acceptable? file in (0, maxFile],
// chunk in [MIN_CHUNK, MAX_CHUNK].
inline bool announceInRange(size_t announced, size_t chunk, size_t maxFile) {
    if (announced == 0 || announced > maxFile) return false;
    if (chunk < MIN_CHUNK || chunk > MAX_CHUNK) return false;
    return true;
}

// A Windows reserved device name (CON, NUL, COM1..9, LPT1..9), optionally with
// an extension — opening it writes to a device, not a file.
inline bool isReservedDeviceName(const std::string& name) {
    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
        stem[3] >= '1' && stem[3] <= '9') {
        return true;
    }
    return false;
}

// Reduce a sender-supplied name to a safe base name in the working directory:
// strip directories, and reject traversal, ':' (Windows drive-relative / ADS)
// and reserved device names by falling back to "file.out".
inline std::string sanitizeName(const std::string& raw) {
    size_t slash = raw.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") return "file.out";
    if (name.find(':') != std::string::npos) return "file.out";
    if (isReservedDeviceName(name)) return "file.out";
    return name;
}

}  // namespace Protocol

#endif  // FILECAST_PROTOCOL_H
