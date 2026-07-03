#include <gtest/gtest.h>

#include "../src/Utils.hpp"
#include "../src/Sha256.hpp"

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

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}