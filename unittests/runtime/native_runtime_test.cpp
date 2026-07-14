#include "joyeer/native/runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace {

int cloneCount = 0;
int destroyCount = 0;

JoyeerString view(const std::string& value) {
    return JoyeerString {
        reinterpret_cast<const uint8_t*>(value.data()),
        static_cast<int64_t>(value.size()),
    };
}

JoyeerString owned(const std::string& value) {
    JoyeerString result {};
    const auto source = view(value);
    joyeer_string_clone_abi(&result, source.data, source.count);
    return result;
}

void cloneString(void* destination, const void* source) {
    ++cloneCount;
    const auto* value = static_cast<const JoyeerString*>(source);
    joyeer_string_clone_abi(
            static_cast<JoyeerString*>(destination),
            value->data,
            value->count);
}

void destroyString(void* value) {
    ++destroyCount;
    joyeer_string_destroy_abi(static_cast<JoyeerString*>(value));
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
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(NativeRuntimeTest, ClonesOwnedStringsIndependently) {
    const std::string sourceText = "owned";
    JoyeerString first {};
    joyeer_string_clone_abi(&first, view(sourceText).data, sourceText.size());
    JoyeerString second {};
    joyeer_string_clone_abi(&second, first.data, first.count);

    ASSERT_NE(first.data, second.data);
    ASSERT_EQ(first.count, second.count);
    EXPECT_EQ(std::memcmp(first.data, second.data, first.count), 0);
    static_cast<uint8_t*>(const_cast<uint8_t*>(second.data))[0] = 'O';
    EXPECT_EQ(first.data[0], static_cast<uint8_t>('o'));
    EXPECT_EQ(second.data[0], static_cast<uint8_t>('O'));

    joyeer_string_destroy_abi(&second);
    joyeer_string_destroy_abi(&first);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(NativeRuntimeDeathTest, TrapsStringBoundsFailures) {
    EXPECT_DEATH(
            static_cast<void>(joyeer_string_byte_at(view(std::string("x")), 1)),
            "string index out of bounds");
}

        TEST(NativeRuntimeTest, ReadsFilesAsOwnedBinaryStrings) {
            const auto path = std::filesystem::temp_directory_path() /
                "joyeer-native-runtime-read-file.bin";
            const std::array<uint8_t, 7> expected { 'J', 'o', 'y', 0, 'e', 'e', 'r' };
            {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output.is_open());
            output.write(
                reinterpret_cast<const char*>(expected.data()),
                static_cast<std::streamsize>(expected.size()));
            ASSERT_TRUE(output.good());
            }

            const auto pathText = path.string();
            int32_t tag = -1;
            alignas(JoyeerString) std::array<uint8_t, sizeof(JoyeerString)> payload {};
            joyeer_read_file_abi(
                &tag,
                payload.data(),
                7,
                9,
                reinterpret_cast<const uint8_t*>(pathText.data()),
                static_cast<int64_t>(pathText.size()));

            EXPECT_EQ(tag, 7);
            JoyeerString contents {};
            std::memcpy(&contents, payload.data(), sizeof(contents));
            ASSERT_EQ(contents.count, static_cast<int64_t>(expected.size()));
            EXPECT_EQ(std::memcmp(contents.data, expected.data(), expected.size()), 0);
            joyeer_string_destroy_abi(&contents);
            std::filesystem::remove(path);
            EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
        }

        TEST(NativeRuntimeTest, ReturnsAnErrorForMissingFilesWithoutLeaking) {
            const auto path = std::filesystem::temp_directory_path() /
                "joyeer-native-runtime-definitely-missing.txt";
            std::filesystem::remove(path);
            const auto pathText = path.string();
            int32_t tag = -1;
            alignas(JoyeerString) std::array<uint8_t, sizeof(JoyeerString)> payload {};
            joyeer_read_file_abi(
                &tag,
                payload.data(),
                7,
                9,
                reinterpret_cast<const uint8_t*>(pathText.data()),
                static_cast<int64_t>(pathText.size()));

            EXPECT_EQ(tag, 9);
            int64_t error = 0;
            std::memcpy(&error, payload.data(), sizeof(error));
            EXPECT_NE(error, 0);
            EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
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
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(NativeRuntimeTest, DeepClonesAndRecursivelyDestroysArrayElements) {
    cloneCount = 0;
    destroyCount = 0;
    std::array<JoyeerString, 2> source {
        owned(std::string("left")),
        owned(std::string("right")),
    };
    JoyeerArray first {};
    joyeer_array_create_owned_abi(
            &first,
            source.data(),
            source.size(),
            sizeof(JoyeerString),
            cloneString,
            destroyString);
    JoyeerArray second {};
    joyeer_array_clone_abi(&second, first.data, first.count);

    ASSERT_EQ(cloneCount, 2);
    auto* firstElement = static_cast<JoyeerString*>(joyeer_array_at(first, 0));
    auto* secondElement = static_cast<JoyeerString*>(joyeer_array_at(second, 0));
    ASSERT_NE(firstElement->data, secondElement->data);
    static_cast<uint8_t*>(const_cast<uint8_t*>(secondElement->data))[0] = 'L';
    EXPECT_EQ(firstElement->data[0], static_cast<uint8_t>('l'));

    joyeer_array_destroy_abi(&second);
    joyeer_array_destroy_abi(&first);
    EXPECT_EQ(destroyCount, 4);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
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
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
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
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

struct OwnedStringEntry {
    JoyeerString key;
    JoyeerString value;
};

TEST(NativeRuntimeTest, DeepClonesAndRecursivelyDestroysDictionaryEntries) {
    cloneCount = 0;
    destroyCount = 0;
    const OwnedStringEntry entry {
        owned(std::string("key")),
        owned(std::string("value")),
    };
    JoyeerDictionary first {};
    joyeer_dictionary_create_owned_abi(
            &first,
            &entry,
            1,
            sizeof(JoyeerString),
            sizeof(JoyeerString),
            sizeof(OwnedStringEntry),
            offsetof(OwnedStringEntry, value),
            JOYEER_DICTIONARY_KEY_STRING,
            cloneString,
            destroyString,
            cloneString,
            destroyString);
    JoyeerDictionary second {};
    joyeer_dictionary_clone_abi(&second, first.data, first.count);

    ASSERT_EQ(cloneCount, 2);
    const std::string lookupKey = "key";
    const auto key = view(lookupKey);
    const auto* firstValue = static_cast<const JoyeerString*>(joyeer_dictionary_at(
            first,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    const auto* secondValue = static_cast<const JoyeerString*>(joyeer_dictionary_at(
            second,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    ASSERT_NE(firstValue->data, secondValue->data);

    joyeer_dictionary_destroy_abi(&second);
    joyeer_dictionary_destroy_abi(&first);
    EXPECT_EQ(destroyCount, 4);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

} // namespace
