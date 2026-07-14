#include "joyeer/native/runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {

JoyeerString view(const std::string& value) {
    return JoyeerString {
        reinterpret_cast<const uint8_t*>(value.data()),
        static_cast<int64_t>(value.size()),
    };
}

TEST(NativeRuntimeTest, PerformsCheckedIntegerArithmetic) {
    EXPECT_EQ(joyeer_checked_add_int(40, 2), 42);
    EXPECT_EQ(joyeer_checked_sub_int(40, 2), 38);
    EXPECT_EQ(joyeer_checked_mul_int(-6, 7), -42);
}

TEST(NativeRuntimeDeathTest, TrapsIntegerOverflow) {
    EXPECT_DEATH(
            static_cast<void>(joyeer_checked_add_int(
                    std::numeric_limits<int64_t>::max(),
                    1)),
            "integer addition overflow");
    EXPECT_DEATH(
            static_cast<void>(joyeer_checked_mul_int(
                    std::numeric_limits<int64_t>::min(),
                    -1)),
            "integer multiplication overflow");
}

TEST(NativeRuntimeTest, ConcatenatesComparesAndIndexesStrings) {
    const std::string left = "Joy";
    const std::string right = "eer";
    auto joined = joyeer_string_concat(view(left), view(right));

    ASSERT_EQ(joined.count, 6);
    EXPECT_EQ(std::memcmp(joined.data, "Joyeer", 6), 0);
    EXPECT_TRUE(joyeer_string_equal(joined, view(std::string("Joyeer"))));
    EXPECT_LT(joyeer_string_compare(view(left), view(right)), 0);
    EXPECT_EQ(joyeer_string_byte_at(joined, 3), static_cast<uint8_t>('e'));

    joyeer_string_destroy(&joined);
    EXPECT_EQ(joined.data, nullptr);
    EXPECT_EQ(joined.count, 0);
}

TEST(NativeRuntimeDeathTest, TrapsStringBoundsFailures) {
    EXPECT_DEATH(
            static_cast<void>(joyeer_string_byte_at(view(std::string("x")), 1)),
            "string index out of bounds");
}

TEST(NativeRuntimeTest, CopiesIndexesAndMutatesArrays) {
    const std::array<int64_t, 3> source { 10, 20, 30 };
    auto array = joyeer_array_create(
            source.data(),
            static_cast<int64_t>(source.size()),
            static_cast<int64_t>(sizeof(int64_t)));

    ASSERT_EQ(array.count, 3);
    auto* middle = static_cast<int64_t*>(joyeer_array_at(array, 1));
    EXPECT_EQ(*middle, 20);
    *middle = 42;
    EXPECT_EQ(*static_cast<int64_t*>(joyeer_array_at(array, 1)), 42);

    joyeer_array_destroy(&array);
    EXPECT_EQ(array.data, nullptr);
}

TEST(NativeRuntimeDeathTest, TrapsArrayBoundsFailures) {
    const int64_t source = 1;
    auto array = joyeer_array_create(&source, 1, sizeof(source));
    EXPECT_DEATH(static_cast<void>(joyeer_array_at(array, -1)), "array index out of bounds");
    joyeer_array_destroy(&array);
}

struct IntEntry {
    int64_t key;
    int64_t value;
};

TEST(NativeRuntimeTest, LooksUpDictionaryValuesByPrimitiveKey) {
    const std::array<IntEntry, 2> entries { IntEntry { 1, 10 }, IntEntry { 2, 20 } };
    auto dictionary = joyeer_dictionary_create(
            entries.data(),
            entries.size(),
            sizeof(int64_t),
            sizeof(int64_t),
            sizeof(IntEntry),
            offsetof(IntEntry, value),
            JOYEER_DICTIONARY_KEY_INT);

    const int64_t key = 2;
    auto* value = static_cast<int64_t*>(joyeer_dictionary_at(
            dictionary,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_INT));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 20);
    *value = 99;
    EXPECT_EQ(entries[1].value, 20);

    joyeer_dictionary_destroy(&dictionary);
    EXPECT_EQ(dictionary.data, nullptr);
}

struct StringEntry {
    JoyeerString key;
    int64_t value;
};

TEST(NativeRuntimeTest, LooksUpDictionaryValuesByStringKey) {
    const std::string name = "answer";
    const StringEntry entry { view(name), 42 };
    auto dictionary = joyeer_dictionary_create(
            &entry,
            1,
            sizeof(JoyeerString),
            sizeof(int64_t),
            sizeof(StringEntry),
            offsetof(StringEntry, value),
            JOYEER_DICTIONARY_KEY_STRING);

    const auto key = view(name);
    const auto* value = static_cast<const int64_t*>(joyeer_dictionary_at(
            dictionary,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
    joyeer_dictionary_destroy(&dictionary);
}

} // namespace
