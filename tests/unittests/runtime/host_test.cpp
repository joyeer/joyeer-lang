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

}
