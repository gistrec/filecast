#ifndef FILECAST_PROTOCOL_H
#define FILECAST_PROTOCOL_H

// Pure, socket-free protocol helpers shared by the receiver and the unit tests.
// Keeping the framing/validation logic here (rather than inline in Receiver.cpp)
// makes it testable without spinning up sockets.

#include <cstddef>
#include <cstdint>
#include <cctype>
#include <algorithm>
#include <string>

namespace Protocol {

// ---- Wire framing (protocol v3) ---------------------------------------------
//
// Every packet starts with the same 10-byte header:
//
//   magic "FCST"(4) + version(1) + type(1) + session(4)
//
// The 4-byte magic keeps stray UDP traffic on our port from being misparsed
// (and is still recognizable to a human reading a tcpdump hex dump); carrying
// the version in every packet — not just the announcement — means any future
// format change degrades into a clear one-line warning instead of silence.
// The header layout itself is the compatibility contract: future versions keep
// these 10 bytes so old builds can still say *why* they are ignoring traffic.

constexpr char    MAGIC[4] = {'F', 'C', 'S', 'T'};
constexpr uint8_t VERSION  = 3;

enum class Type : uint8_t {
    Announce = 1,  // transfer metadata: file size, chunk size, sha256, name
    Transfer = 2,  // one chunk of file data
    Finish   = 3,  // sender has sent every part
    Resend   = 4,  // receiver asks for a lost part
};

constexpr size_t HEADER_SIZE = 10;
// Fixed part of ANNOUNCE, before the variable-length file name:
// header + file_size(4) + chunk_size(4) + sha256(32) + name_len(2).
constexpr size_t ANNOUNCE_FIXED = HEADER_SIZE + 42;
// TRANSFER framing before the payload: header + part(4) + length(4).
constexpr size_t TRANSFER_HEADER = HEADER_SIZE + 8;
constexpr size_t FINISH_SIZE = HEADER_SIZE;
constexpr size_t RESEND_SIZE = HEADER_SIZE + 4;  // header + part(4)

// Big-endian field helpers, self-contained so this header stays socket-free
// (Utils.hpp drags in the platform socket headers).
inline void putU16(char* p, uint16_t v) {
    p[0] = static_cast<char>(v >> 8);
    p[1] = static_cast<char>(v);
}

inline void putU32(char* p, uint32_t v) {
    p[0] = static_cast<char>(v >> 24);
    p[1] = static_cast<char>(v >> 16);
    p[2] = static_cast<char>(v >> 8);
    p[3] = static_cast<char>(v);
}

inline uint16_t getU16(const char* p) {
    return static_cast<uint16_t>((static_cast<unsigned char>(p[0]) << 8) |
                                 static_cast<unsigned char>(p[1]));
}

inline uint32_t getU32(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

struct Header {
    uint8_t  version = 0;
    Type     type    = Type::Announce;  // may hold a value outside the enum
    uint32_t session = 0;
};

enum class Parse {
    Ok,          // ours, current version; header filled in
    NotOurs,     // too short or wrong magic — silently ignore
    BadVersion,  // our magic, different version — worth a (single) warning
};

// Write the common header. The caller guarantees buf holds HEADER_SIZE bytes.
inline void writeHeader(char* buf, Type type, uint32_t session) {
    buf[0] = MAGIC[0];
    buf[1] = MAGIC[1];
    buf[2] = MAGIC[2];
    buf[3] = MAGIC[3];
    buf[4] = static_cast<char>(VERSION);
    buf[5] = static_cast<char>(type);
    putU32(buf + 6, session);
}

// Validate the magic/version and extract the header fields. On BadVersion the
// header is still filled in, so the caller can report which version it saw.
inline Parse parseHeader(const char* buf, size_t length, Header& h) {
    if (length < HEADER_SIZE) return Parse::NotOurs;
    if (buf[0] != MAGIC[0] || buf[1] != MAGIC[1] ||
        buf[2] != MAGIC[2] || buf[3] != MAGIC[3]) {
        return Parse::NotOurs;
    }
    h.version = static_cast<uint8_t>(buf[4]);
    h.type    = static_cast<Type>(buf[5]);
    h.session = getU32(buf + 6);
    return (h.version == VERSION) ? Parse::Ok : Parse::BadVersion;
}

// Largest IPv4 UDP payload; a TRANSFER datagram is TRANSFER_HEADER + chunk, so
// the chunk must stay TRANSFER_HEADER below it or sendto() hits EMSGSIZE.
constexpr size_t MAX_UDP_PAYLOAD = 65507;

// Valid chunk size range (the sender's --mtu bounds). The lower bound also caps
// the part count, so a forged tiny chunk cannot blow up the receiver.
constexpr size_t MIN_CHUNK = 64;
constexpr size_t MAX_CHUNK = MAX_UDP_PAYLOAD - TRANSFER_HEADER;  // 65489

inline size_t totalParts(size_t file_length, size_t chunk_size) {
    if (chunk_size == 0) return 0;  // guard the reusable helper against misuse
    // Overflow-safe ceiling division: file_length + chunk_size - 1 would wrap a
    // 32-bit size_t near the 0xFFFFFFFF wire limit and collapse the count to ~0.
    return file_length / chunk_size + (file_length % chunk_size != 0 ? 1 : 0);
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
    // The extension (everything from the first '.') is already dropped above, so
    // `stem` holds only the base name and can never contain a dot. Windows also
    // strips trailing spaces from that base before matching, so "con ", "com1 "
    // and the "com1 " base of "com1 .txt" still resolve to devices — trim them.
    stem.erase(stem.find_last_not_of(' ') + 1);
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
        stem[3] >= '1' && stem[3] <= '9') {
        return true;
    }
    return false;
}

// Does the name contain a control byte (< 0x20, incl. NUL) or DEL (0x7f)?
// Two distinct attacks ride in via such bytes from a hostile announcement:
//   * an embedded NUL truncates the C string we hand to open()/rename(), so the
//     file lands on disk under a shorter name than the one we printed (and the
//     ".part" suffix silently disappears);
//   * an ANSI escape (ESC, 0x1b) smuggled into the name hijacks the receiver's
//     terminal when we echo "Receiving <name>" / "Received <name>".
inline bool hasControlChar(const std::string& name) {
    return std::any_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

// Longest base name the receiver will materialize on disk. It appends the
// ".part.idx" suffix (9 bytes) to build the resume-snapshot path, and most
// filesystems cap a single path component at NAME_MAX (255). Keeping the base
// name at or under this bound means "<name>.part" and "<name>.part.idx" both
// stay within NAME_MAX, so an otherwise-valid transfer cannot fail open() or
// rename() with ENAMETOOLONG — whether the long name is legitimate or a crafted
// over-long ANNOUNCE aimed at a still-waiting receiver.
constexpr size_t MAX_NAME_LEN = 255 - 9;  // room for the ".part.idx" suffix

// Reduce a sender-supplied name to a safe base name in the working directory:
// strip directories, reject traversal, ':' (Windows drive-relative / ADS),
// control/NUL bytes and reserved device names by falling back to "file.out",
// then clamp the length so the ".part"/".part.idx" paths fit NAME_MAX.
inline std::string sanitizeName(const std::string& raw) {
    size_t slash = raw.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") return "file.out";
    if (name.find(':') != std::string::npos) return "file.out";
    if (hasControlChar(name)) return "file.out";
    if (isReservedDeviceName(name)) return "file.out";
    if (name.size() > MAX_NAME_LEN) name.resize(MAX_NAME_LEN);
    return name;
}

}  // namespace Protocol

#endif  // FILECAST_PROTOCOL_H
