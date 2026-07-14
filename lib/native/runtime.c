#include "joyeer/native/runtime.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

typedef struct ArrayHeader {
    int64_t elementSize;
} ArrayHeader;

typedef struct DictionaryHeader {
    int64_t keySize;
    int64_t valueSize;
    int64_t entrySize;
    int64_t valueOffset;
    int32_t keyKind;
} DictionaryHeader;

static void* checkedAllocate(size_t size) {
    void* value = malloc(size == 0 ? 1 : size);
    if (value == NULL) joyeer_panic("out of memory");
    return value;
}

static size_t checkedByteCount(int64_t count, int64_t stride) {
    if (count < 0 || stride < 0) joyeer_panic("negative allocation size");
    if (count != 0 && (uint64_t)stride > SIZE_MAX / (uint64_t)count) {
        joyeer_panic("allocation size overflow");
    }
    return (size_t)count * (size_t)stride;
}

_Noreturn void joyeer_panic(const char* message) {
    fputs("Joyeer runtime error: ", stderr);
    fputs(message == NULL ? "unknown error" : message, stderr);
    fputc('\n', stderr);
    abort();
}

int64_t joyeer_checked_add_int(int64_t left, int64_t right) {
    int64_t result;
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
    if (__builtin_add_overflow(left, right, &result)) joyeer_panic("integer addition overflow");
    return result;
#endif
#endif
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        joyeer_panic("integer addition overflow");
    }
    return left + right;
}

int64_t joyeer_checked_sub_int(int64_t left, int64_t right) {
    int64_t result;
#if defined(__has_builtin)
#if __has_builtin(__builtin_sub_overflow)
    if (__builtin_sub_overflow(left, right, &result)) joyeer_panic("integer subtraction overflow");
    return result;
#endif
#endif
    if ((right < 0 && left > INT64_MAX + right) ||
        (right > 0 && left < INT64_MIN + right)) {
        joyeer_panic("integer subtraction overflow");
    }
    return left - right;
}

int64_t joyeer_checked_mul_int(int64_t left, int64_t right) {
    int64_t result;
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
    if (__builtin_mul_overflow(left, right, &result)) joyeer_panic("integer multiplication overflow");
    return result;
#endif
#endif
    if (left == 0 || right == 0) return 0;
    if ((left == -1 && right == INT64_MIN) ||
        (right == -1 && left == INT64_MIN)) {
        joyeer_panic("integer multiplication overflow");
    }
    if (left > 0) {
        if ((right > 0 && left > INT64_MAX / right) ||
            (right < 0 && right < INT64_MIN / left)) {
            joyeer_panic("integer multiplication overflow");
        }
    } else {
        if ((right > 0 && left < INT64_MIN / right) ||
            (right < 0 && left < INT64_MAX / right)) {
            joyeer_panic("integer multiplication overflow");
        }
    }
    return left * right;
}

void joyeer_print_int(int64_t value) {
    printf("%" PRId64 "\n", value);
}

void joyeer_print_bool(bool value) {
    puts(value ? "true" : "false");
}

void joyeer_print_byte(uint8_t value) {
    printf("%u\n", (unsigned int)value);
}

void joyeer_print_string(JoyeerString value) {
    if (value.count < 0 || (value.count != 0 && value.data == NULL)) {
        joyeer_panic("invalid string");
    }
    if (value.count != 0) fwrite(value.data, 1, (size_t)value.count, stdout);
    fputc('\n', stdout);
}

JoyeerString joyeer_string_concat(JoyeerString left, JoyeerString right) {
    if (left.count < 0 || right.count < 0 ||
        (left.count != 0 && left.data == NULL) ||
        (right.count != 0 && right.data == NULL) ||
        left.count > INT64_MAX - right.count) {
        joyeer_panic("invalid string concatenation");
    }
    const int64_t count = left.count + right.count;
    uint8_t* data = (uint8_t*)checkedAllocate((size_t)(count == 0 ? 1 : count));
    if (left.count != 0) memcpy(data, left.data, (size_t)left.count);
    if (right.count != 0) memcpy(data + left.count, right.data, (size_t)right.count);
    JoyeerString result = { data, count };
    return result;
}

bool joyeer_string_equal(JoyeerString left, JoyeerString right) {
    return left.count == right.count &&
           (left.count == 0 || memcmp(left.data, right.data, (size_t)left.count) == 0);
}

int64_t joyeer_string_compare(JoyeerString left, JoyeerString right) {
    const int64_t common = left.count < right.count ? left.count : right.count;
    const int comparison = common == 0 ? 0 : memcmp(left.data, right.data, (size_t)common);
    if (comparison < 0) return -1;
    if (comparison > 0) return 1;
    return (left.count > right.count) - (left.count < right.count);
}

uint8_t joyeer_string_byte_at(JoyeerString value, int64_t index) {
    if (index < 0 || index >= value.count) joyeer_panic("string index out of bounds");
    return value.data[index];
}

void joyeer_string_destroy(JoyeerString* value) {
    if (value == NULL) return;
    free((void*)value->data);
    value->data = NULL;
    value->count = 0;
}

JoyeerArray joyeer_array_create(const void* values, int64_t count, int64_t elementSize) {
    const size_t bytes = checkedByteCount(count, elementSize);
    ArrayHeader* storage = (ArrayHeader*)checkedAllocate(sizeof(ArrayHeader) + bytes);
    storage->elementSize = elementSize;
    void* data = storage + 1;
    if (bytes != 0) {
        if (values == NULL) joyeer_panic("array initializer is null");
        memcpy(data, values, bytes);
    }
    JoyeerArray result = { data, count, count };
    return result;
}

void* joyeer_array_at(JoyeerArray array, int64_t index) {
    if (array.data == NULL || index < 0 || index >= array.count) {
        joyeer_panic("array index out of bounds");
    }
    const ArrayHeader* header = ((const ArrayHeader*)array.data) - 1;
    return (uint8_t*)array.data + checkedByteCount(index, header->elementSize);
}

void joyeer_array_destroy(JoyeerArray* array) {
    if (array == NULL || array->data == NULL) return;
    free(((ArrayHeader*)array->data) - 1);
    array->data = NULL;
    array->count = 0;
    array->capacity = 0;
}

static bool dictionaryKeyEqual(
        const void* left,
        const void* right,
        int64_t keySize,
        int32_t keyKind) {
    switch (keyKind) {
        case JOYEER_DICTIONARY_KEY_INT:
            return *(const int64_t*)left == *(const int64_t*)right;
        case JOYEER_DICTIONARY_KEY_BOOL:
            return *(const bool*)left == *(const bool*)right;
        case JOYEER_DICTIONARY_KEY_STRING:
            return joyeer_string_equal(
                    *(const JoyeerString*)left,
                    *(const JoyeerString*)right);
        case JOYEER_DICTIONARY_KEY_BYTE:
            return *(const uint8_t*)left == *(const uint8_t*)right;
        default:
            return memcmp(left, right, (size_t)keySize) == 0;
    }
}

JoyeerDictionary joyeer_dictionary_create(
        const void* entries,
        int64_t count,
        int64_t keySize,
        int64_t valueSize,
        int64_t entrySize,
        int64_t valueOffset,
        int32_t keyKind) {
    if (keySize <= 0 || valueSize < 0 || entrySize < keySize ||
        valueOffset < keySize || valueOffset + valueSize > entrySize) {
        joyeer_panic("invalid dictionary layout");
    }
    const size_t bytes = checkedByteCount(count, entrySize);
    DictionaryHeader* storage =
            (DictionaryHeader*)checkedAllocate(sizeof(DictionaryHeader) + bytes);
    storage->keySize = keySize;
    storage->valueSize = valueSize;
    storage->entrySize = entrySize;
    storage->valueOffset = valueOffset;
    storage->keyKind = keyKind;
    void* data = storage + 1;
    if (bytes != 0) {
        if (entries == NULL) joyeer_panic("dictionary initializer is null");
        memcpy(data, entries, bytes);
    }
    JoyeerDictionary result = { data, count, count };
    return result;
}

void* joyeer_dictionary_at(
        JoyeerDictionary dictionary,
        const void* key,
        int64_t keySize,
        int32_t keyKind) {
    if (dictionary.data == NULL || key == NULL) joyeer_panic("invalid dictionary lookup");
    const DictionaryHeader* header = ((const DictionaryHeader*)dictionary.data) - 1;
    if (keySize != header->keySize || keyKind != header->keyKind) {
        joyeer_panic("dictionary key type mismatch");
    }
    for (int64_t index = 0; index < dictionary.count; ++index) {
        uint8_t* entry = (uint8_t*)dictionary.data +
                checkedByteCount(index, header->entrySize);
        if (dictionaryKeyEqual(entry, key, keySize, keyKind)) {
            return entry + header->valueOffset;
        }
    }
    joyeer_panic("dictionary key not found");
}

void joyeer_dictionary_destroy(JoyeerDictionary* dictionary) {
    if (dictionary == NULL || dictionary->data == NULL) return;
    free(((DictionaryHeader*)dictionary->data) - 1);
    dictionary->data = NULL;
    dictionary->count = 0;
    dictionary->capacity = 0;
}
