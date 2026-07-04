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
}

TEST(Protocol, SanitizeNameAllowsNormalNames) {
    EXPECT_EQ(Protocol::sanitizeName("console.log"), "console.log");  // not a device
    EXPECT_EQ(Protocol::sanitizeName("com10.bin"), "com10.bin");      // COM10 is not reserved
}

TEST(Protocol, TotalPartsBoundaries) {
    EXPECT_EQ(Protocol::totalParts(1, 1500), 1u);
    EXPECT_EQ(Protocol::totalParts(1500, 1500), 1u);
    EXPECT_EQ(Protocol::totalParts(1501, 1500), 2u);
    EXPECT_EQ(Protocol::totalParts(3000, 1500), 2u);
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
    EXPECT_TRUE(Protocol::announceInRange(maxFile, 65507, maxFile));
    EXPECT_FALSE(Protocol::announceInRange(0, 1500, maxFile));         // empty file
    EXPECT_FALSE(Protocol::announceInRange(maxFile + 1, 1500, maxFile)); // too big
    EXPECT_FALSE(Protocol::announceInRange(1000, 63, maxFile));        // chunk too small (OOM guard)
    EXPECT_FALSE(Protocol::announceInRange(1000, 65508, maxFile));     // chunk too big
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