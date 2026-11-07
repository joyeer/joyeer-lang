#include "runtime/runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

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


// The Unicode "well-formed UTF-8 byte sequences" table (Unicode Table 3-7),
// written independently of the implementation under test.
bool referenceWellFormed(const std::vector<uint8_t>& bytes) {
    size_t index = 0;
    const auto in = [&](size_t offset, int low, int high) {
        return index + offset < bytes.size() && bytes[index + offset] >= low && bytes[index + offset] <= high;
    };
    while (index < bytes.size()) {
        const int first = bytes[index];
        if (first <= 0x7f) {
            index += 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            if (!in(1, 0x80, 0xbf)) return false;
            index += 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            const int low = first == 0xe0 ? 0xa0 : 0x80;
            const int high = first == 0xed ? 0x9f : 0xbf;
            if (!in(1, low, high) || !in(2, 0x80, 0xbf)) return false;
            index += 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            const int low = first == 0xf0 ? 0x90 : 0x80;
            const int high = first == 0xf4 ? 0x8f : 0xbf;
            if (!in(1, low, high) || !in(2, 0x80, 0xbf) || !in(3, 0x80, 0xbf)) return false;
            index += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool validUtf8(const std::vector<uint8_t>& bytes) {
    return joyeer_host_valid_utf8(bytes.empty() ? nullptr : bytes.data(),
                                  static_cast<int64_t>(bytes.size()));
}

TEST(NativeHostTest, MatchesTheUnicodeTableForEveryShortSequence) {
    EXPECT_TRUE(validUtf8({}));
    // Every one-, two- and three-byte sequence.
    for (int a = 0; a < 256; ++a) {
        std::vector<uint8_t> one { static_cast<uint8_t>(a) };
        ASSERT_EQ(validUtf8(one), referenceWellFormed(one)) << a;
        for (int b = 0; b < 256; ++b) {
            std::vector<uint8_t> two { static_cast<uint8_t>(a), static_cast<uint8_t>(b) };
            ASSERT_EQ(validUtf8(two), referenceWellFormed(two)) << a << ' ' << b;
        }
    }
    std::vector<uint8_t> three(3);
    for (int a = 0xc2; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            for (int c = 0; c < 256; ++c) {
                three[0] = static_cast<uint8_t>(a);
                three[1] = static_cast<uint8_t>(b);
                three[2] = static_cast<uint8_t>(c);
                if (validUtf8(three) != referenceWellFormed(three)) {
                    FAIL() << a << ' ' << b << ' ' << c;
                }
            }
        }
    }
}

TEST(NativeHostTest, MatchesTheUnicodeTableForBoundaryFourByteSequencesAndMixedStrings) {
    const std::array<int, 18> edges { 0x00, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0,
                                      0xc1, 0xc2, 0xdf, 0xe0, 0xed, 0xf0, 0xf4, 0xf5, 0xff };
    for (const int a : edges) {
        for (const int b : edges) {
            for (const int c : edges) {
                for (const int d : edges) {
                    const std::vector<uint8_t> four { static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                                      static_cast<uint8_t>(c), static_cast<uint8_t>(d) };
                    ASSERT_EQ(validUtf8(four), referenceWellFormed(four))
                            << a << ' ' << b << ' ' << c << ' ' << d;
                }
            }
        }
    }
    // Valid scalars at every width, concatenated, and each prefix of them.
    const std::vector<uint8_t> text { 'a', 0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80,
                                      0xf4, 0x8f, 0xbf, 0xbf, 0x00, 'z' };
    EXPECT_TRUE(validUtf8(text));
    for (size_t length = 0; length <= text.size(); ++length) {
        const std::vector<uint8_t> prefix(text.begin(), text.begin() + static_cast<long>(length));
        EXPECT_EQ(validUtf8(prefix), referenceWellFormed(prefix)) << length;
    }
}

}
