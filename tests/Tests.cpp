#include <gtest/gtest.h>

#include "../src/Utils.hpp"
#include "../src/Sha256.hpp"
#include "../src/Protocol.hpp"
#include "../src/Progress.hpp"

#include <string>
#include <cstring>

class BytesConverter : public ::testing::TestWithParam<std::tuple<size_t, int>> {
public:
    size_t value;
    int    count;

protected:
    void SetUp() override {
        std::tie(value, count) = GetParam();
    }
};

INSTANTIATE_TEST_SUITE_P(
    CombinationsTest, BytesConverter,
    ::testing::Combine(
        ::testing::Values(0U, 1U, 2U, 1000U, 65535U, 2147483647U),
        ::testing::Values(4, 8, 16)));

TEST_P(BytesConverter, getIntFromBytes) {
    char buffer[16] = { 0 };

    Utils::writeBytesFromNumber(buffer, value, count);

    size_t result = Utils::getNumberFromBytes(buffer, count);

    EXPECT_EQ(result, value);
}

static std::string sha256Hex(const std::string& s) {
    uint8_t digest[32];
    Sha256::hash(s.data(), s.size(), digest);
    return Sha256::hex(digest);
}

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(sha256Hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256Hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256Hex("The quick brown fox jumps over the lazy dog"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Sha256, CrossesBlockBoundary) {
    // 1 000 000 'a' -> a well-known SHA-256 test vector; exercises multiple
    // 64-byte blocks and the length padding.
    std::string million(1000000, 'a');
    EXPECT_EQ(sha256Hex(million),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, DetectsSingleBitFlip) {
    std::string a(256, 'x');
    std::string b = a;
    b[100] ^= 0x01;
    EXPECT_NE(sha256Hex(a), sha256Hex(b));
}

// --- Wire framing (common header) -------------------------------------------

TEST(Protocol, HeaderRoundTrip) {
    char buf[Protocol::HEADER_SIZE];
    Protocol::writeHeader(buf, Protocol::Type::Transfer, 0xDEADBEEFu);
    EXPECT_EQ(memcmp(buf, "FCST", 4), 0);

    Protocol::Header h;
    EXPECT_EQ(Protocol::parseHeader(buf, sizeof(buf), h), Protocol::Parse::Ok);
    EXPECT_EQ(h.version, Protocol::VERSION);
    EXPECT_EQ(h.type, Protocol::Type::Transfer);
    EXPECT_EQ(h.session, 0xDEADBEEFu);
}

TEST(Protocol, HeaderRejectsShortAndForeign) {
    char buf[Protocol::HEADER_SIZE];
    Protocol::writeHeader(buf, Protocol::Type::Finish, 42);

    Protocol::Header h;
    // Truncated: even one byte short of the header is not ours.
    EXPECT_EQ(Protocol::parseHeader(buf, Protocol::HEADER_SIZE - 1, h), Protocol::Parse::NotOurs);
    EXPECT_EQ(Protocol::parseHeader(buf, 0, h), Protocol::Parse::NotOurs);

    // Wrong magic: stray traffic (including the old text-tagged v2 packets)
    // must be silently ignored, not misparsed.
    char stray[16] = {0};
    memcpy(stray, "NEW_PACKET", 10);
    EXPECT_EQ(Protocol::parseHeader(stray, sizeof(stray), h), Protocol::Parse::NotOurs);
}

TEST(Protocol, HeaderAcceptsUnknownType) {
    // Forward compatibility contract: an unknown type byte within the current
    // version parses Ok — the dispatcher ignores it — rather than being
    // mistaken for foreign traffic.
    char buf[Protocol::HEADER_SIZE];
    Protocol::writeHeader(buf, static_cast<Protocol::Type>(200), 99);

    Protocol::Header h;
    EXPECT_EQ(Protocol::parseHeader(buf, sizeof(buf), h), Protocol::Parse::Ok);
    EXPECT_EQ(static_cast<uint8_t>(h.type), 200);
    EXPECT_EQ(h.session, 99u);
}

TEST(Protocol, HeaderReportsForeignVersion) {
    char buf[Protocol::HEADER_SIZE];
    Protocol::writeHeader(buf, Protocol::Type::Announce, 7);
    buf[4] = static_cast<char>(Protocol::VERSION + 1);

    Protocol::Header h;
    EXPECT_EQ(Protocol::parseHeader(buf, sizeof(buf), h), Protocol::Parse::BadVersion);
    // The header is still filled in so the caller can say which version it saw.
    EXPECT_EQ(h.version, Protocol::VERSION + 1);
    EXPECT_EQ(h.session, 7u);
}

TEST(Protocol, FieldHelpersRoundTrip) {
    char buf[4];
    for (uint32_t v : {0u, 1u, 0xFFu, 0x1234u, 0xFFFFu, 0x12345678u, 0xFFFFFFFFu}) {
        Protocol::putU32(buf, v);
        EXPECT_EQ(Protocol::getU32(buf), v);
    }
    for (uint16_t v : {uint16_t{0}, uint16_t{1}, uint16_t{0x1234}, uint16_t{0xFFFF}}) {
        Protocol::putU16(buf, v);
        EXPECT_EQ(Protocol::getU16(buf), v);
    }
    // Big-endian on the wire: most significant byte first.
    Protocol::putU32(buf, 0x0A0B0C0Du);
    EXPECT_EQ(buf[0], 0x0A);
    EXPECT_EQ(buf[3], 0x0D);
}

// --- Protocol helpers -------------------------------------------------------

TEST(Protocol, SanitizeNameStripsDirectories) {
    EXPECT_EQ(Protocol::sanitizeName("photo.jpg"), "photo.jpg");
    EXPECT_EQ(Protocol::sanitizeName("a/b/c.txt"), "c.txt");
    EXPECT_EQ(Protocol::sanitizeName("../../etc/passwd"), "passwd");
    EXPECT_EQ(Protocol::sanitizeName("dir\\sub\\file.bin"), "file.bin");
}

TEST(Protocol, SanitizeNameRejectsDangerous) {
    EXPECT_EQ(Protocol::sanitizeName(""), "file.out");
    EXPECT_EQ(Protocol::sanitizeName("."), "file.out");
    EXPECT_EQ(Protocol::sanitizeName(".."), "file.out");
    EXPECT_EQ(Protocol::sanitizeName("C:evil"), "file.out");          // drive-relative
    EXPECT_EQ(Protocol::sanitizeName("notes.txt:hidden"), "file.out"); // ADS
    EXPECT_EQ(Protocol::sanitizeName("CON"), "file.out");             // device
    EXPECT_EQ(Protocol::sanitizeName("com1.txt"), "file.out");        // device + ext, any case
    EXPECT_EQ(Protocol::sanitizeName("LPT9"), "file.out");
    EXPECT_EQ(Protocol::sanitizeName("con "), "file.out");            // trailing space
    EXPECT_EQ(Protocol::sanitizeName("aux "), "file.out");            // trailing space, other device
    EXPECT_EQ(Protocol::sanitizeName("com1 .txt"), "file.out");       // trailing space before ext
    EXPECT_EQ(Protocol::sanitizeName("console.log"), "console.log");  // base "console" is not a device
}

TEST(Protocol, SanitizeNameClampsLength) {
    // Clamp to MAX_NAME_LEN so "<name>.part" / "<name>.part.idx" stay within
    // NAME_MAX; otherwise a legit long name — or a crafted over-long ANNOUNCE —
    // makes the receiver's open()/rename() fail with ENAMETOOLONG.
    std::string over(500, 'a');
    EXPECT_EQ(Protocol::sanitizeName(over).size(), Protocol::MAX_NAME_LEN);
    std::string exact(Protocol::MAX_NAME_LEN, 'b');
    EXPECT_EQ(Protocol::sanitizeName(exact), exact);
}

TEST(Protocol, SanitizeNameRejectsControlBytes) {
    using std::string;
    // Embedded NUL: without this guard the name passes every other check but the
    // on-disk path truncates at the NUL ("evil"), diverging from what we print.
    EXPECT_EQ(Protocol::sanitizeName(string("evil\0.txt", 9)), "file.out");
    EXPECT_EQ(Protocol::sanitizeName(string("\0", 1)), "file.out");
    // ANSI escape / DEL / other control bytes would inject into the receiver's
    // terminal when the name is echoed; reject them all.
    EXPECT_EQ(Protocol::sanitizeName("\033[2J\033[Hpwned"), "file.out");  // ESC (0x1b)
    EXPECT_EQ(Protocol::sanitizeName("bell\a.txt"), "file.out");          // BEL (0x07)
    EXPECT_EQ(Protocol::sanitizeName("line\nbreak.txt"), "file.out");     // LF  (0x0a)
    EXPECT_EQ(Protocol::sanitizeName("tab\tchar.txt"), "file.out");       // TAB (0x09)
    EXPECT_EQ(Protocol::sanitizeName(string("del\x7f.txt", 8)), "file.out"); // DEL (0x7f)
    // A control byte living only in a stripped directory component must not
    // condemn an otherwise-clean base name.
    EXPECT_EQ(Protocol::sanitizeName(string("dir\n/clean.txt", 14)), "clean.txt");
}

TEST(Protocol, SanitizeNameAllowsNormalNames) {
    EXPECT_EQ(Protocol::sanitizeName("console.log"), "console.log");  // not a device
    EXPECT_EQ(Protocol::sanitizeName("com10.bin"), "com10.bin");      // COM10 is not reserved
    // Bytes >= 0x20 stay put — spaces, punctuation and high/UTF-8 bytes are fine.
    EXPECT_EQ(Protocol::sanitizeName("my report (final).pdf"), "my report (final).pdf");
    EXPECT_EQ(Protocol::sanitizeName("\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.txt"),
              "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.txt");               // UTF-8 "файл.txt"
}

TEST(Protocol, TotalPartsBoundaries) {
    EXPECT_EQ(Protocol::totalParts(1, 1500), 1u);
    EXPECT_EQ(Protocol::totalParts(1500, 1500), 1u);
    EXPECT_EQ(Protocol::totalParts(1501, 1500), 2u);
    EXPECT_EQ(Protocol::totalParts(3000, 1500), 2u);
    // Max v3 wire file size: file_length + chunk_size must not overflow, or a
    // 32-bit size_t wraps the count to ~0. A real guard on 32-bit builds; on a
    // 64-bit size_t it can't overflow, so here it just pins the expected count.
    EXPECT_EQ(Protocol::totalParts(0xFFFFFFFFu, 64), 67108864u);
    EXPECT_EQ(Protocol::totalParts(0xFFFFFFFFu, 65489), 65584u);
}

TEST(Protocol, ExpectedPartSize) {
    // 3001 bytes over 1500-byte chunks -> parts of 1500, 1500, 1.
    EXPECT_EQ(Protocol::expectedPartSize(0, 3001, 1500), 1500u);
    EXPECT_EQ(Protocol::expectedPartSize(1, 3001, 1500), 1500u);
    EXPECT_EQ(Protocol::expectedPartSize(2, 3001, 1500), 1u);
    // Exact multiple: last part is a full chunk.
    EXPECT_EQ(Protocol::expectedPartSize(1, 3000, 1500), 1500u);
}

TEST(Protocol, AnnounceInRange) {
    const size_t maxFile = 4ULL * 1024 * 1024 * 1024;
    EXPECT_TRUE(Protocol::announceInRange(1, 64, maxFile));
    EXPECT_TRUE(Protocol::announceInRange(maxFile, 65489, maxFile));   // MAX_CHUNK (fits a UDP datagram)
    EXPECT_FALSE(Protocol::announceInRange(0, 1500, maxFile));         // empty file
    EXPECT_FALSE(Protocol::announceInRange(maxFile + 1, 1500, maxFile)); // too big
    EXPECT_FALSE(Protocol::announceInRange(1000, 63, maxFile));        // chunk too small (OOM guard)
    EXPECT_FALSE(Protocol::announceInRange(1000, 65490, maxFile));     // chunk too big (TRANSFER_HEADER would overflow the datagram)
}

// --- Progress formatting ----------------------------------------------------

TEST(Progress, HumanBytes) {
    EXPECT_EQ(Progress::humanBytes(0), "0 B");
    EXPECT_EQ(Progress::humanBytes(512), "512 B");
    EXPECT_EQ(Progress::humanBytes(1024), "1.0 KiB");
    EXPECT_EQ(Progress::humanBytes(1536), "1.5 KiB");
    EXPECT_EQ(Progress::humanBytes(1024ULL * 1024), "1.0 MiB");
    EXPECT_EQ(Progress::humanBytes(1024ULL * 1024 * 1024), "1.0 GiB");
}

TEST(Progress, HumanDuration) {
    EXPECT_EQ(Progress::humanDuration(3.2), "3.2s");
    EXPECT_EQ(Progress::humanDuration(63), "1m03s");
    EXPECT_EQ(Progress::humanDuration(-5), "0.0s");     // clamped, no negative
    EXPECT_EQ(Progress::humanDuration(1e12), ">99h");   // clamped, no int overflow/UB
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}