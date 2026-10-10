#include "host_internal.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>

namespace {

TEST(NativeHostTest, ValidatesUnicodeScalarUtf8WithoutRequiringTextContents) {
    for (const auto& text : std::array<std::string, 6> {
            "", "plain", "\xc2\x80", "\xe0\xa0\x80", "\xed\x9f\xbf",
            "\xf4\x8f\xbf\xbf" }) {
        EXPECT_TRUE(joyeer_host_valid_utf8(
                reinterpret_cast<const uint8_t*>(text.data()),
                static_cast<int64_t>(text.size())));
    }
    const uint8_t nul[] { 0 };
    EXPECT_TRUE(joyeer_host_valid_utf8(nul, 1));
    EXPECT_TRUE(joyeer_host_valid_utf8(nullptr, 0));
}

TEST(NativeHostTest, RejectsOverlongSurrogateTruncatedAndOutOfRangeUtf8) {
    for (const auto& text : std::array<std::string, 11> {
            "\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80", "\xed\xa0\x80",
            "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
            "\xc2", "\xe2\x82", "\xf0\x9f\x98" }) {
        EXPECT_FALSE(joyeer_host_valid_utf8(
                reinterpret_cast<const uint8_t*>(text.data()),
                static_cast<int64_t>(text.size())));
    }
    EXPECT_FALSE(joyeer_host_valid_utf8(nullptr, 1));
    EXPECT_FALSE(joyeer_host_valid_utf8(nullptr, -1));
}

TEST(NativeHostTest, DistinguishesEmptyArgumentsFromEmptyPaths) {
    int64_t error = -1;
    auto* argument = joyeer_host_string(nullptr, 0, true, &error);
    ASSERT_NE(argument, nullptr);
    EXPECT_EQ(argument[0], 0);
    EXPECT_EQ(error, 0);
    free(argument);
    EXPECT_EQ(joyeer_host_string(nullptr, 0, false, &error), nullptr);
    EXPECT_NE(error, 0);
}

TEST(NativeHostTest, RejectsEmbeddedNulAndInvalidEncodingAtHostBoundary) {
    int64_t error = 0;
    const uint8_t embedded[] { 'a', 0, 'b' };
    EXPECT_EQ(joyeer_host_string(embedded, 3, true, &error), nullptr);
    EXPECT_NE(error, 0);
    const uint8_t invalid[] { 0xed, 0xa0, 0x80 };
    EXPECT_EQ(joyeer_host_string(invalid, 3, true, &error), nullptr);
    EXPECT_NE(error, 0);
}

TEST(NativeHostTest, ConvertsSupplementaryUnicodeWithoutReplacement) {
    const uint8_t bytes[] { 0xf0, 0x9f, 0x98, 0x80 };
    int64_t error = -1;
    auto* result = joyeer_host_string(bytes, 4, false, &error);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(error, 0);
#if defined(_WIN32)
    EXPECT_EQ(result[0], 0xd83d);
    EXPECT_EQ(result[1], 0xde00);
    EXPECT_EQ(result[2], 0);
#else
    EXPECT_EQ(std::string(result), "\xf0\x9f\x98\x80");
#endif
    free(result);
}

}
