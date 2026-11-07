#include "runtime/runtime.h"
#include "runtime/native.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

TEST(NativeBufferTest, CopiesBinaryBuffersAndStringListsWithRuntimeOwnership) {
    const auto baseline = joyeer_runtime_active_allocations();
    const uint8_t data[] {0, 255, 65};
    JoyeerArray bytes {};
    joyeer_native_bytes_init(&bytes);
    joyeer_native_bytes_assign(&bytes, data, 3);
    EXPECT_EQ(bytes.count, 3);
    EXPECT_EQ(std::memcmp(bytes.data, data, 3), 0);
    joyeer_native_bytes_assign(&bytes, static_cast<const uint8_t*>(bytes.data), bytes.count);
    JoyeerString name {};
    joyeer_native_string_assign(&name, data, 3);
    JoyeerArray names {};
    joyeer_native_strings_init(&names);
    joyeer_native_strings_assign(&names, &name, 1);
    joyeer_string_destroy_abi(&name);
    JoyeerArray copy {};
    joyeer_array_clone_abi(&copy, names.data, names.count);
    joyeer_array_destroy_abi(&names);
    const auto* copied = static_cast<const JoyeerString*>(copy.data);
    EXPECT_EQ(copied->count, 3);
    EXPECT_EQ(std::memcmp(copied->data, data, 3), 0);
    joyeer_array_destroy_abi(&copy);
    joyeer_array_destroy_abi(&bytes);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

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

// The runtime's by-value and element-access conveniences are not part of its
// ABI: generated code uses the flat `_abi` functions and checks bounds inline.
// These helpers express the same operations over that ABI.
JoyeerString concatStrings(JoyeerString left, JoyeerString right) {
    JoyeerString result {};
    joyeer_string_concat_abi(&result, left.data, left.count, right.data, right.count);
    return result;
}

bool stringsEqual(JoyeerString left, JoyeerString right) {
    return joyeer_string_equal_abi(left.data, left.count, right.data, right.count);
}

int64_t compareStrings(JoyeerString left, JoyeerString right) {
    return joyeer_string_compare_abi(left.data, left.count, right.data, right.count);
}


JoyeerArray createArray(const void* values, int64_t count, int64_t elementSize) {
    JoyeerArray result {};
    joyeer_array_create_owned_abi(&result, values, count, elementSize, nullptr, nullptr);
    return result;
}

template <typename T>
T* arrayAt(const JoyeerArray& array, int64_t index) {
    EXPECT_GE(index, 0);
    EXPECT_LT(index, array.count);
    return static_cast<T*>(array.data) + index;
}

JoyeerDictionary createDictionary(
        const void* entries, int64_t count, int64_t keySize, int64_t valueSize,
        int64_t entrySize, int64_t valueOffset, int32_t keyKind) {
    JoyeerDictionary result {};
    joyeer_dictionary_create_owned_abi(
            &result, entries, count, keySize, valueSize, entrySize, valueOffset, keyKind,
            nullptr, nullptr, nullptr, nullptr);
    return result;
}

void* dictionaryAt(
        const JoyeerDictionary& dictionary, const void* key, int64_t keySize, int32_t keyKind) {
    return joyeer_dictionary_at_abi(dictionary.data, dictionary.count, key, keySize, keyKind);
}

TEST(RuntimeTest, ConcatenatesComparesAndIndexesStrings) {
    const std::string left = "Joy";
    const std::string right = "eer";
    auto joined = concatStrings(view(left), view(right));

    ASSERT_EQ(joined.count, 6);
    EXPECT_EQ(std::memcmp(joined.data, "Joyeer", 6), 0);
    EXPECT_TRUE(stringsEqual(joined, view(std::string("Joyeer"))));
    EXPECT_LT(compareStrings(view(left), view(right)), 0);
    EXPECT_EQ(joined.data[3], static_cast<uint8_t>('e'));

    joyeer_string_destroy_abi(&joined);
    EXPECT_EQ(joined.data, nullptr);
    EXPECT_EQ(joined.count, 0);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, ClonesOwnedStringsIndependently) {
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

TEST(RuntimeTest, ConvertsBytesToOwnedSingleByteStrings) {
    JoyeerString text {};
    joyeer_byte_to_string_abi(&text, static_cast<uint8_t>('J'));

    ASSERT_EQ(text.count, 1);
    EXPECT_EQ(text.data[0], static_cast<uint8_t>('J'));
    joyeer_string_destroy_abi(&text);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, CopiesIndexesAndMutatesArrays) {
    const std::array<int64_t, 3> source { 10, 20, 30 };
    auto array = createArray(
            source.data(),
            static_cast<int64_t>(source.size()),
            static_cast<int64_t>(sizeof(int64_t)));

    ASSERT_EQ(array.count, 3);
    auto* middle = arrayAt<int64_t>(array, 1);
    EXPECT_EQ(*middle, 20);
    *middle = 42;
    EXPECT_EQ(*arrayAt<int64_t>(array, 1), 42);

    joyeer_array_destroy_abi(&array);
    EXPECT_EQ(array.data, nullptr);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, AppendsTrivialElementsAndGrowsCapacity) {
    JoyeerArray array {};
    joyeer_array_create_owned_abi(
            &array,
            nullptr,
            0,
            sizeof(int64_t),
            nullptr,
            nullptr);

    for (int64_t value = 0; value < 17; ++value) {
        const int64_t appended = value * 3;
        joyeer_array_append_owned_abi(&array, &appended);
    }

    ASSERT_EQ(array.count, 17);
    EXPECT_GE(array.capacity, array.count);
    for (int64_t index = 0; index < array.count; ++index) {
        EXPECT_EQ(*arrayAt<int64_t>(array, index), index * 3);
    }
    joyeer_array_destroy_abi(&array);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, TakesAndRecursivelyDestroysAppendedOwnedElements) {
    cloneCount = 0;
    destroyCount = 0;
    JoyeerArray array {};
    joyeer_array_create_owned_abi(
            &array,
            nullptr,
            0,
            sizeof(JoyeerString),
            cloneString,
            destroyString);
    const auto first = owned(std::string("first"));
    const auto second = owned(std::string("second"));

    joyeer_array_append_owned_abi(&array, &first);
    joyeer_array_append_owned_abi(&array, &second);

    ASSERT_EQ(array.count, 2);
    EXPECT_EQ(cloneCount, 0);
    const auto* firstStored = arrayAt<JoyeerString>(array, 0);
    const auto* secondStored = arrayAt<JoyeerString>(array, 1);
    EXPECT_TRUE(stringsEqual(*firstStored, view(std::string("first"))));
    EXPECT_TRUE(stringsEqual(*secondStored, view(std::string("second"))));
    joyeer_array_destroy_abi(&array);
    EXPECT_EQ(destroyCount, 2);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, DeepClonesAndRecursivelyDestroysArrayElements) {
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
    auto* firstElement = arrayAt<JoyeerString>(first, 0);
    auto* secondElement = arrayAt<JoyeerString>(second, 0);
    ASSERT_NE(firstElement->data, secondElement->data);
    static_cast<uint8_t*>(const_cast<uint8_t*>(secondElement->data))[0] = 'L';
    EXPECT_EQ(firstElement->data[0], static_cast<uint8_t>('l'));

    joyeer_array_destroy_abi(&second);
    joyeer_array_destroy_abi(&first);
    EXPECT_EQ(destroyCount, 4);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

struct IntEntry {
    int64_t key;
    int64_t value;
};

TEST(RuntimeTest, LooksUpDictionaryValuesByPrimitiveKey) {
    const std::array<IntEntry, 2> entries { IntEntry { 1, 10 }, IntEntry { 2, 20 } };
    auto dictionary = createDictionary(
            entries.data(),
            entries.size(),
            sizeof(int64_t),
            sizeof(int64_t),
            sizeof(IntEntry),
            offsetof(IntEntry, value),
            JOYEER_DICTIONARY_KEY_INT);

    const int64_t key = 2;
    auto* value = static_cast<int64_t*>(dictionaryAt(
            dictionary,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_INT));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 20);
    *value = 99;
    EXPECT_EQ(entries[1].value, 20);

    joyeer_dictionary_destroy_abi(&dictionary);
    EXPECT_EQ(dictionary.data, nullptr);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

struct StringEntry {
    JoyeerString key;
    int64_t value;
};

struct OwnedStringEntry {
    JoyeerString key;
    JoyeerString value;
};

template <typename Key>
void expectDictionaryFind(
        const Key& storedKey,
        const Key& hitKey,
        const Key& missingKey,
        int32_t keyKind) {
    struct Entry {
        Key key;
        int64_t value;
    };
    const Entry entry { storedKey, 42 };
    auto dictionary = createDictionary(
            &entry, 1, sizeof(Key), sizeof(int64_t),
            sizeof(Entry), offsetof(Entry, value), keyKind);
    const auto allocations = joyeer_runtime_active_allocations();
    const auto* found = static_cast<const int64_t*>(joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &hitKey, sizeof(Key), keyKind));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, 42);
    EXPECT_EQ(found, joyeer_dictionary_at_abi(
            dictionary.data, dictionary.count, &hitKey, sizeof(Key), keyKind));
    EXPECT_EQ(joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &missingKey, sizeof(Key), keyKind), nullptr);
    EXPECT_EQ(dictionary.count, 1);
    EXPECT_EQ(joyeer_runtime_active_allocations(), allocations);
    joyeer_dictionary_destroy_abi(&dictionary);

    auto empty = createDictionary(
            nullptr, 0, sizeof(Key), sizeof(int64_t),
            sizeof(Entry), offsetof(Entry, value), keyKind);
    const auto emptyAllocations = joyeer_runtime_active_allocations();
    EXPECT_EQ(joyeer_dictionary_find_abi(
            empty.data, empty.count, &hitKey, sizeof(Key), keyKind), nullptr);
    EXPECT_EQ(joyeer_runtime_active_allocations(), emptyAllocations);
    joyeer_dictionary_destroy_abi(&empty);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, FindsPresentMissingAndEmptyIntegerDictionaryEntries) {
    expectDictionaryFind<int64_t>(2, 2, 3, JOYEER_DICTIONARY_KEY_INT);
}

TEST(RuntimeTest, FindsPresentMissingAndEmptyBooleanDictionaryEntries) {
    expectDictionaryFind<bool>(false, false, true, JOYEER_DICTIONARY_KEY_BOOL);
}

TEST(RuntimeTest, FindsPresentMissingAndEmptyByteDictionaryEntries) {
    expectDictionaryFind<uint8_t>(255, 255, 0, JOYEER_DICTIONARY_KEY_BYTE);
}

TEST(RuntimeTest, FindsPresentMissingAndEmptyStringDictionaryEntriesByContent) {
    const std::string stored("key\0suffix", 10);
    const std::string hit(stored);
    const std::string missing("key");
    ASSERT_NE(stored.data(), hit.data());
    expectDictionaryFind(view(stored), view(hit), view(missing), JOYEER_DICTIONARY_KEY_STRING);
}

TEST(RuntimeTest, FindsZeroSizedDictionaryValuesWithNonNullPresence) {
    const std::array<int64_t, 2> keys { 1, 2 };
    auto dictionary = createDictionary(
            keys.data(), keys.size(), sizeof(int64_t), 0,
            sizeof(int64_t), sizeof(int64_t), JOYEER_DICTIONARY_KEY_INT);
    const int64_t hit = 2;
    const int64_t missing = 3;
    auto* found = joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &hit, sizeof(hit), JOYEER_DICTIONARY_KEY_INT);
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found, static_cast<uint8_t*>(dictionary.data) + sizeof(keys));
    EXPECT_EQ(found, dictionaryAt(
            dictionary, &hit, sizeof(hit), JOYEER_DICTIONARY_KEY_INT));
    EXPECT_EQ(joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &missing, sizeof(missing),
            JOYEER_DICTIONARY_KEY_INT), nullptr);
    joyeer_dictionary_destroy_abi(&dictionary);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, FindsBorrowedStorageWithoutCloningOrConsumingDictionaryEntries) {
    cloneCount = 0;
    destroyCount = 0;
    const std::array<OwnedStringEntry, 2> entries {
        OwnedStringEntry { owned("key"), owned("value") },
        OwnedStringEntry { owned("other"), owned("unchanged") },
    };
    JoyeerDictionary dictionary {};
    joyeer_dictionary_create_owned_abi(
            &dictionary, entries.data(), entries.size(),
            sizeof(JoyeerString), sizeof(JoyeerString),
            sizeof(OwnedStringEntry), offsetof(OwnedStringEntry, value),
            JOYEER_DICTIONARY_KEY_STRING, cloneString, destroyString,
            cloneString, destroyString);
    auto key = owned("key");
    const auto originalData = dictionary.data;
    const auto allocations = joyeer_runtime_active_allocations();
    const auto* found = static_cast<const JoyeerString*>(joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &key, sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->data, entries[0].value.data);
    EXPECT_EQ(found, joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &key, sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    const std::string missingText = "missing";
    const auto missing = view(missingText);
    EXPECT_EQ(joyeer_dictionary_find_abi(
            dictionary.data, dictionary.count, &missing, sizeof(missing),
            JOYEER_DICTIONARY_KEY_STRING), nullptr);
    EXPECT_EQ(cloneCount, 0);
    EXPECT_EQ(destroyCount, 0);
    EXPECT_EQ(dictionary.data, originalData);
    EXPECT_EQ(dictionary.count, 2);
    EXPECT_TRUE(stringsEqual(key, entries[0].key));
    EXPECT_EQ(joyeer_runtime_active_allocations(), allocations);

    JoyeerString result {};
    cloneString(&result, found);
    EXPECT_EQ(cloneCount, 1);
    EXPECT_NE(result.data, found->data);
    joyeer_dictionary_destroy_abi(&dictionary);
    EXPECT_TRUE(stringsEqual(result, view(std::string("value"))));
    EXPECT_EQ(destroyCount, 4);
    destroyString(&result);
    joyeer_string_destroy_abi(&key);
    EXPECT_EQ(destroyCount, 5);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeDeathTest, StrictDictionaryLookupStillTrapsForMissingAndEmptyEntries) {
    const IntEntry entry { 1, 42 };
    const int64_t missing = 2;
    for (const int64_t count : { 0, 1 }) {
        auto dictionary = createDictionary(
                &entry, count, sizeof(int64_t), sizeof(int64_t),
                sizeof(IntEntry), offsetof(IntEntry, value), JOYEER_DICTIONARY_KEY_INT);
        EXPECT_EQ(joyeer_dictionary_find_abi(
                dictionary.data, dictionary.count, &missing, sizeof(missing),
                JOYEER_DICTIONARY_KEY_INT), nullptr);
        EXPECT_DEATH(
                static_cast<void>(dictionaryAt(
                        dictionary, &missing, sizeof(missing), JOYEER_DICTIONARY_KEY_INT)),
                "dictionary key not found");
        EXPECT_DEATH(
                static_cast<void>(joyeer_dictionary_at_abi(
                        dictionary.data, dictionary.count, &missing, sizeof(missing),
                        JOYEER_DICTIONARY_KEY_INT)),
                "dictionary key not found");
        joyeer_dictionary_destroy_abi(&dictionary);
    }
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeDeathTest, DictionaryFindPreservesInvalidLookupDiagnostics) {
    const int64_t key = 1;
    auto dictionary = createDictionary(
            nullptr, 0, sizeof(int64_t), sizeof(int64_t),
            sizeof(IntEntry), offsetof(IntEntry, value), JOYEER_DICTIONARY_KEY_INT);
    for (const auto lookup : { joyeer_dictionary_find_abi, joyeer_dictionary_at_abi }) {
        EXPECT_DEATH(
                static_cast<void>(lookup(
                        nullptr, 0, &key, sizeof(key), JOYEER_DICTIONARY_KEY_INT)),
                "invalid dictionary lookup");
        EXPECT_DEATH(
                static_cast<void>(lookup(
                        dictionary.data, 0, nullptr, sizeof(key), JOYEER_DICTIONARY_KEY_INT)),
                "invalid dictionary lookup");
        EXPECT_DEATH(
                static_cast<void>(lookup(
                        dictionary.data, 0, &key, sizeof(bool), JOYEER_DICTIONARY_KEY_INT)),
                "dictionary key type mismatch");
        EXPECT_DEATH(
                static_cast<void>(lookup(
                        dictionary.data, 0, &key, sizeof(key), JOYEER_DICTIONARY_KEY_BOOL)),
                "dictionary key type mismatch");
    }
    joyeer_dictionary_destroy_abi(&dictionary);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, DictionaryConstructionMatchesInsertionForDuplicateKeys) {
    const std::array<IntEntry, 5> entries {
    IntEntry { 1, 10 }, IntEntry { 2, 30 }, IntEntry { 1, 20 },
    IntEntry { 3, 40 }, IntEntry { 2, 50 },
    };
    auto constructed = createDictionary(
        entries.data(), entries.size(), sizeof(int64_t), sizeof(int64_t),
        sizeof(IntEntry), offsetof(IntEntry, value), JOYEER_DICTIONARY_KEY_INT);
    auto inserted = createDictionary(
        nullptr, 0, sizeof(int64_t), sizeof(int64_t), sizeof(IntEntry),
        offsetof(IntEntry, value), JOYEER_DICTIONARY_KEY_INT);
    for (const auto& entry : entries) {
    auto key = entry.key;
    joyeer_dictionary_set_owned_abi(&inserted, &key, &entry.value);
    }
    EXPECT_EQ(constructed.count, 3);
    EXPECT_EQ(constructed.count, inserted.count);
    EXPECT_GE(constructed.capacity, constructed.count);
    for (const auto& expected : { IntEntry { 1, 20 }, IntEntry { 2, 50 }, IntEntry { 3, 40 } }) {
    const auto* constructedValue = static_cast<const int64_t*>(dictionaryAt(
        constructed, &expected.key, sizeof(expected.key), JOYEER_DICTIONARY_KEY_INT));
    const auto* insertedValue = static_cast<const int64_t*>(dictionaryAt(
        inserted, &expected.key, sizeof(expected.key), JOYEER_DICTIONARY_KEY_INT));
    EXPECT_EQ(*constructedValue, expected.value);
    EXPECT_EQ(*constructedValue, *insertedValue);
    }
    EXPECT_EQ(entries[0].value, 10);
    joyeer_dictionary_destroy_abi(&constructed);
    joyeer_dictionary_destroy_abi(&inserted);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, DestroysDuplicateOwnedDictionaryEntriesExactlyOnce) {
    cloneCount = 0;
    destroyCount = 0;
    const std::array<OwnedStringEntry, 4> entries {
    OwnedStringEntry { owned("key"), owned("first") },
    OwnedStringEntry { owned("key"), owned("second") },
        OwnedStringEntry { owned("other"), owned("kept") },
    OwnedStringEntry { owned("key"), owned("final") },
    };
    ASSERT_NE(entries[0].key.data, entries[1].key.data);
    JoyeerDictionary dictionary {};
    joyeer_dictionary_create_owned_abi(
        &dictionary, entries.data(), entries.size(), sizeof(JoyeerString), sizeof(JoyeerString),
        sizeof(OwnedStringEntry), offsetof(OwnedStringEntry, value), JOYEER_DICTIONARY_KEY_STRING,
        cloneString, destroyString, cloneString, destroyString);
    EXPECT_EQ(dictionary.count, 2);
    EXPECT_EQ(cloneCount, 0);
    EXPECT_EQ(destroyCount, 4);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 5);

    JoyeerDictionary clone {};
    joyeer_dictionary_clone_abi(&clone, dictionary.data, dictionary.count);
    EXPECT_EQ(clone.count, 2);
    EXPECT_EQ(cloneCount, 4);
    const std::string keyText = "key";
    const auto key = view(keyText);
    const auto* value = static_cast<const JoyeerString*>(dictionaryAt(
        dictionary, &key, sizeof(key), JOYEER_DICTIONARY_KEY_STRING));
    const auto* cloneValue = static_cast<const JoyeerString*>(dictionaryAt(
        clone, &key, sizeof(key), JOYEER_DICTIONARY_KEY_STRING));
    EXPECT_TRUE(stringsEqual(*value, view(std::string("final"))));
    EXPECT_NE(value->data, cloneValue->data);

    joyeer_dictionary_destroy_abi(&dictionary);
    EXPECT_EQ(destroyCount, 8);
    EXPECT_TRUE(stringsEqual(*cloneValue, view(std::string("final"))));
    const std::string otherText = "other";
    const auto otherKey = view(otherText);
    const auto* otherValue = static_cast<const JoyeerString*>(dictionaryAt(
        clone, &otherKey, sizeof(otherKey), JOYEER_DICTIONARY_KEY_STRING));
    EXPECT_TRUE(stringsEqual(*otherValue, view(std::string("kept"))));
    joyeer_dictionary_destroy_abi(&clone);
    EXPECT_EQ(destroyCount, 12);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeTest, LooksUpDictionaryValuesByStringKey) {
    const std::string name = "answer";
    const StringEntry entry { view(name), 42 };
    auto dictionary = createDictionary(
            &entry,
            1,
            sizeof(JoyeerString),
            sizeof(int64_t),
            sizeof(StringEntry),
            offsetof(StringEntry, value),
            JOYEER_DICTIONARY_KEY_STRING);

    const auto key = view(name);
    const auto* value = static_cast<const int64_t*>(dictionaryAt(
            dictionary,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
    joyeer_dictionary_destroy_abi(&dictionary);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

    TEST(RuntimeTest, InsertsUpdatesAndGrowsDictionaryEntries) {
        JoyeerDictionary dictionary {};
        joyeer_dictionary_create_owned_abi(
            &dictionary,
            nullptr,
            0,
            sizeof(int64_t),
            sizeof(int64_t),
            sizeof(IntEntry),
            offsetof(IntEntry, value),
            JOYEER_DICTIONARY_KEY_INT,
            nullptr,
            nullptr,
            nullptr,
            nullptr);

        for (int64_t key = 0; key < 17; ++key) {
        const int64_t value = key * 10;
        joyeer_dictionary_set_owned_abi(&dictionary, &key, &value);
        }
        int64_t updatedKey = 5;
        const int64_t updatedValue = 999;
        joyeer_dictionary_set_owned_abi(&dictionary, &updatedKey, &updatedValue);

        EXPECT_EQ(dictionary.count, 17);
        EXPECT_GE(dictionary.capacity, dictionary.count);
        EXPECT_EQ(
            *static_cast<const int64_t*>(dictionaryAt(
                dictionary,
                &updatedKey,
                sizeof(updatedKey),
                JOYEER_DICTIONARY_KEY_INT)),
            updatedValue);
        joyeer_dictionary_destroy_abi(&dictionary);
        EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
    }

    TEST(RuntimeTest, TakesAndDestroysOwnedDictionaryInsertionsAndUpdates) {
        cloneCount = 0;
        destroyCount = 0;
        JoyeerDictionary dictionary {};
        joyeer_dictionary_create_owned_abi(
            &dictionary,
            nullptr,
            0,
            sizeof(JoyeerString),
            sizeof(JoyeerString),
            sizeof(OwnedStringEntry),
            offsetof(OwnedStringEntry, value),
            JOYEER_DICTIONARY_KEY_STRING,
            cloneString,
            destroyString,
            cloneString,
            destroyString);
        auto firstKey = owned(std::string("key"));
        auto firstValue = owned(std::string("first"));
        joyeer_dictionary_set_owned_abi(&dictionary, &firstKey, &firstValue);
        auto replacementKey = owned(std::string("key"));
        auto replacementValue = owned(std::string("second"));
        joyeer_dictionary_set_owned_abi(
            &dictionary,
            &replacementKey,
            &replacementValue);

        EXPECT_EQ(dictionary.count, 1);
        EXPECT_EQ(cloneCount, 0);
        EXPECT_EQ(destroyCount, 2);
        const std::string lookupText = "key";
        const auto lookupKey = view(lookupText);
        const auto* stored = static_cast<const JoyeerString*>(dictionaryAt(
            dictionary,
            &lookupKey,
            sizeof(lookupKey),
            JOYEER_DICTIONARY_KEY_STRING));
        EXPECT_TRUE(stringsEqual(*stored, view(std::string("second"))));

        joyeer_dictionary_destroy_abi(&dictionary);
        EXPECT_EQ(destroyCount, 4);
        EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
    }

TEST(RuntimeTest, DeepClonesAndRecursivelyDestroysDictionaryEntries) {
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
    const auto* firstValue = static_cast<const JoyeerString*>(dictionaryAt(
            first,
            &key,
            sizeof(key),
            JOYEER_DICTIONARY_KEY_STRING));
    const auto* secondValue = static_cast<const JoyeerString*>(dictionaryAt(
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

// Black-box coverage of the exported ABI. These tests name only functions that
// runtime.h declares (through the helpers above), so they hold for any provider
// of the runtime: the C implementation today, Joyeer objects later (see
// docs/plan/runtime-in-joyeer.md).

std::string capturedStdout(void (*produce)()) {
    std::fflush(stdout);
    testing::internal::CaptureStdout();
    produce();
    std::fflush(stdout);
    auto text = testing::internal::GetCapturedStdout();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

TEST(RuntimeBlackBoxTest, PrintsIntegersBooleansAndBytesOnePerLine) {
    EXPECT_EQ(capturedStdout([] { joyeer_print_int(-42); }), "-42\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_int(std::numeric_limits<int64_t>::min()); }),
              "-9223372036854775808\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_uint(std::numeric_limits<uint64_t>::max()); }),
              "18446744073709551615\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_bool(true); }), "true\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_bool(false); }), "false\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_byte(255); }), "255\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_byte(0); }), "0\n");
}

TEST(RuntimeBlackBoxTest, PrintsDecimalAtEveryDigitCountBoundary) {
    EXPECT_EQ(capturedStdout([] { joyeer_print_int(0); }), "0\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_int(-1); }), "-1\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_int(std::numeric_limits<int64_t>::max()); }),
              "9223372036854775807\n");
    // capturedStdout takes a plain function, so the value travels in a static.
    static uint64_t printed;
    printed = 1;
    for (int digits = 1; digits <= 20; ++digits) {
        EXPECT_EQ(capturedStdout([] { joyeer_print_uint(printed); }), std::to_string(printed) + "\n");
        if (printed > 1) {
            --printed;
            EXPECT_EQ(capturedStdout([] { joyeer_print_uint(printed); }), std::to_string(printed) + "\n");
            ++printed;
        }
        if (printed > std::numeric_limits<uint64_t>::max() / 10) break;
        printed *= 10;
    }
}

TEST(RuntimeBlackBoxTest, PrintsFloatingPointWithNineAndSeventeenSignificantDigits) {
    EXPECT_EQ(capturedStdout([] { joyeer_print_float(1.5f); }), "1.5\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_float(0.1f); }), "0.100000001\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_double(0.1); }), "0.10000000000000001\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_double(1e300); }), "1.0000000000000001e+300\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_double(-0.0); }), "-0\n");
    EXPECT_EQ(capturedStdout([] { joyeer_print_double(100.0); }), "100\n");
}

TEST(RuntimeBlackBoxTest, PrintsStringBytesUntransformedWithOneNewline) {
    const std::string text("a\0b\xff", 4);
    std::fflush(stdout);
    testing::internal::CaptureStdout();
    joyeer_print_string_abi(reinterpret_cast<const uint8_t*>(text.data()), 4);
    joyeer_print_string_abi(nullptr, 0);
    std::fflush(stdout);
    auto captured = testing::internal::GetCapturedStdout();
    captured.erase(std::remove(captured.begin(), captured.end(), '\r'), captured.end());
    EXPECT_EQ(captured, text + "\n" + "\n");
}

TEST(RuntimeBlackBoxTest, ComparesStringsAsUnsignedBytesWithExactResults) {
    const auto compare = [](const std::string& left, const std::string& right) {
        return joyeer_string_compare_abi(
                reinterpret_cast<const uint8_t*>(left.data()), static_cast<int64_t>(left.size()),
                reinterpret_cast<const uint8_t*>(right.data()), static_cast<int64_t>(right.size()));
    };
    EXPECT_EQ(compare("", ""), 0);
    EXPECT_EQ(compare("", "a"), -1);
    EXPECT_EQ(compare("a", ""), 1);
    EXPECT_EQ(compare("abc", "abc"), 0);
    EXPECT_EQ(compare("ab", "abc"), -1);
    EXPECT_EQ(compare("abc", "ab"), 1);
    EXPECT_EQ(compare("abc", "abd"), -1);
    EXPECT_EQ(compare("b", "abc"), 1);
    EXPECT_EQ(compare(std::string("a\0b", 3), std::string("a\0c", 3)), -1);
    EXPECT_EQ(compare(std::string("a\0", 2), "a"), 1);
    EXPECT_EQ(compare("\x7f", "\x80"), -1) << "bytes above 0x7f order after ASCII";
    EXPECT_EQ(compare("\xff", "\x01"), 1);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeBlackBoxTest, ComparesStringsForEqualityByLengthAndContent) {
    const auto equal = [](const std::string& left, const std::string& right) {
        return joyeer_string_equal_abi(
                reinterpret_cast<const uint8_t*>(left.data()), static_cast<int64_t>(left.size()),
                reinterpret_cast<const uint8_t*>(right.data()), static_cast<int64_t>(right.size()));
    };
    EXPECT_TRUE(equal("", ""));
    EXPECT_TRUE(equal("same", "same"));
    EXPECT_FALSE(equal("same", "sam"));
    EXPECT_FALSE(equal("same", "samf"));
    EXPECT_TRUE(equal(std::string("a\0b", 3), std::string("a\0b", 3)));
    EXPECT_FALSE(equal(std::string("a\0b", 3), std::string("a\0c", 3)));
    EXPECT_FALSE(equal("\xff", "\x7f"));
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeBlackBoxTest, ComparisonAllocatesNothingAndConcatenationAllocatesOnce) {
    const auto baseline = joyeer_runtime_active_allocations();
    const std::string left = "left";
    const std::string right = "right";
    (void)stringsEqual(view(left), view(right));
    (void)compareStrings(view(left), view(right));
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);

    auto joined = concatStrings(view(left), view(right));
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
    EXPECT_EQ(joined.count, 9);
    joyeer_string_destroy_abi(&joined);

    auto empty = concatStrings(view(std::string()), view(std::string()));
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1) << "an empty result still allocates once";
    EXPECT_EQ(empty.count, 0);
    EXPECT_NE(empty.data, nullptr);
    joyeer_string_destroy_abi(&empty);

    JoyeerString cloned {};
    joyeer_string_clone_abi(&cloned, nullptr, 0);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
    joyeer_string_destroy_abi(&cloned);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST(RuntimeBlackBoxTest, ConcatenationPreservesEveryByteAndIgnoresSourceLifetime) {
    std::string left("a\0\xff", 3);
    std::string right("\x80z", 2);
    auto joined = concatStrings(view(left), view(right));
    left.assign("xxx");
    right.assign("yy");
    ASSERT_EQ(joined.count, 5);
    EXPECT_EQ(std::memcmp(joined.data, "a\0\xff\x80z", 5), 0);
    joyeer_string_destroy_abi(&joined);
    EXPECT_EQ(joined.data, nullptr);
    EXPECT_EQ(joined.count, 0);
}

TEST(RuntimeBlackBoxTest, ConvertsByteValuesToOneByteStrings) {
    for (int value = 0; value < 256; value += 51) {
        JoyeerString text {};
        joyeer_byte_to_string_abi(&text, static_cast<uint8_t>(value));
        ASSERT_EQ(text.count, 1);
        EXPECT_EQ(text.data[0], static_cast<uint8_t>(value));
        joyeer_string_destroy_abi(&text);
    }
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeBlackBoxDeathTest, PanicPrintsTheMessageAndExitsWithFailure) {
    EXPECT_EXIT(joyeer_panic("boom"), testing::ExitedWithCode(1), "Joyeer runtime error: boom");
    EXPECT_EXIT(joyeer_panic(nullptr), testing::ExitedWithCode(1),
                "Joyeer runtime error: unknown error");
    EXPECT_EXIT(joyeer_panic(""), testing::ExitedWithCode(1), "Joyeer runtime error: \n");
}

TEST(RuntimeBlackBoxTest, AllocatorCountsBlocksAndKeepsTheirContents) {
    const auto baseline = joyeer_runtime_active_allocations();
    auto* first = static_cast<uint8_t*>(joyeer_alloc(16));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
    for (int index = 0; index < 16; ++index) first[index] = static_cast<uint8_t>(index);

    // A zero-byte request is one block with an address of its own.
    void* empty = joyeer_alloc(0);
    void* other = joyeer_alloc(0);
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(empty, other);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 3);

    // Growing and shrinking keep the leading bytes and do not change the count.
    first = static_cast<uint8_t*>(joyeer_realloc(first, 16, 4096));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 3);
    for (int index = 0; index < 16; ++index) EXPECT_EQ(first[index], index);
    first = static_cast<uint8_t*>(joyeer_realloc(first, 4096, 8));
    for (int index = 0; index < 8; ++index) EXPECT_EQ(first[index], index);

    // Reallocating nothing allocates; freeing nothing is a no-op.
    void* fresh = joyeer_realloc(nullptr, 0, 32);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 4);
    joyeer_free(nullptr, 0);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 4);

    joyeer_free(first, 8);
    joyeer_free(empty, 0);
    joyeer_free(other, 0);
    joyeer_free(fresh, 32);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST(RuntimeBlackBoxDeathTest, AllocatorRejectsNegativeSizesAndCounterUnderflow) {
    EXPECT_EXIT(joyeer_alloc(static_cast<size_t>(-1)), testing::ExitedWithCode(1),
                "Joyeer runtime error: negative allocation size");
    EXPECT_EXIT(joyeer_realloc(nullptr, 0, static_cast<size_t>(-1)), testing::ExitedWithCode(1),
                "Joyeer runtime error: negative allocation size");
    // Freeing a block the counter never saw leaves it below zero.
    EXPECT_EXIT({
        void* block = std::malloc(8);
        joyeer_free(block, 8);
    }, testing::ExitedWithCode(1), "Joyeer runtime error: runtime allocation counter underflow");
}

TEST(RuntimeBlackBoxTest, GrowsArraysFromFourThenDoubles) {
    JoyeerArray array {};
    joyeer_array_create_owned_abi(&array, nullptr, 0, sizeof(int64_t), nullptr, nullptr);
    EXPECT_EQ(array.capacity, 0);
    const std::array<std::pair<int64_t, int64_t>, 5> steps {{
        {1, 4}, {4, 4}, {5, 8}, {9, 16}, {17, 32},
    }};
    int64_t appended = 0;
    for (const auto& [count, capacity] : steps) {
        while (appended < count) {
            joyeer_array_append_owned_abi(&array, &appended);
            ++appended;
        }
        EXPECT_EQ(array.count, count);
        EXPECT_EQ(array.capacity, capacity) << "after " << count << " elements";
    }
    joyeer_array_destroy_abi(&array);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeBlackBoxTest, ClonesAnEmptyArrayAndKeepsItIndependent) {
    JoyeerArray source {};
    joyeer_array_create_owned_abi(&source, nullptr, 0, sizeof(int64_t), nullptr, nullptr);
    JoyeerArray copy {};
    joyeer_array_clone_abi(&copy, source.data, source.count);
    const int64_t value = 7;
    joyeer_array_append_owned_abi(&copy, &value);
    EXPECT_EQ(source.count, 0);
    EXPECT_EQ(copy.count, 1);
    joyeer_array_destroy_abi(&copy);
    joyeer_array_destroy_abi(&source);
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}

TEST(RuntimeBlackBoxTest, ExposesTheCRuntimeStateThatNativeBindingsCannotNameDirectly) {
    errno = ERANGE;
    EXPECT_EQ(joyeer_native_errno(), ERANGE);
    errno = 0;
    EXPECT_EQ(joyeer_native_errno(), 0);

    EXPECT_EQ(joyeer_native_stdio(0), static_cast<int64_t>(reinterpret_cast<intptr_t>(stdin)));
    EXPECT_EQ(joyeer_native_stdio(1), static_cast<int64_t>(reinterpret_cast<intptr_t>(stdout)));
    EXPECT_EQ(joyeer_native_stdio(2), static_cast<int64_t>(reinterpret_cast<intptr_t>(stderr)));
    EXPECT_EQ(joyeer_native_stdio(3), 0);
    EXPECT_EQ(joyeer_native_stdio(-1), 0);

#if defined(_WIN32)
    EXPECT_EQ(joyeer_native_environment(), 0);
#else
    EXPECT_NE(joyeer_native_environment(), 0);
#endif
}

} // namespace
