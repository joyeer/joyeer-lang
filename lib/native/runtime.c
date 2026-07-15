#include "joyeer/native/runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

typedef struct ArrayHeader {
    int64_t elementSize;
    JoyeerCloneValueFn cloneElement;
    JoyeerDestroyValueFn destroyElement;
} ArrayHeader;

typedef struct DictionaryHeader {
    int64_t keySize;
    int64_t valueSize;
    int64_t entrySize;
    int64_t valueOffset;
    int32_t keyKind;
    JoyeerCloneValueFn cloneKey;
    JoyeerDestroyValueFn destroyKey;
    JoyeerCloneValueFn cloneValue;
    JoyeerDestroyValueFn destroyValue;
} DictionaryHeader;

static _Atomic int64_t activeAllocations = 0;

static void* checkedAllocate(size_t size) {
    void* value = malloc(size == 0 ? 1 : size);
    if (value == NULL) joyeer_panic("out of memory");
    atomic_fetch_add_explicit(&activeAllocations, 1, memory_order_relaxed);
    return value;
}

static void checkedFree(void* value) {
    if (value == NULL) return;
    free(value);
    const int64_t previous = atomic_fetch_sub_explicit(
            &activeAllocations,
            1,
            memory_order_relaxed);
    if (previous <= 0) joyeer_panic("runtime allocation counter underflow");
}

static void* checkedReallocate(void* value, size_t size) {
    if (value == NULL) return checkedAllocate(size);
    void* result = realloc(value, size == 0 ? 1 : size);
    if (result == NULL) joyeer_panic("out of memory");
    return result;
}

int64_t joyeer_runtime_active_allocations(void) {
    return atomic_load_explicit(&activeAllocations, memory_order_relaxed);
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
    checkedFree((void*)value->data);
    value->data = NULL;
    value->count = 0;
}

void joyeer_string_clone_abi(
        JoyeerString* result,
        const uint8_t* data,
        int64_t count) {
    if (result == NULL || count < 0 || (count != 0 && data == NULL)) {
        joyeer_panic("invalid string clone");
    }
    uint8_t* copy = (uint8_t*)checkedAllocate((size_t)(count == 0 ? 1 : count));
    if (count != 0) memcpy(copy, data, (size_t)count);
    result->data = copy;
    result->count = count;
}

void joyeer_string_destroy_abi(JoyeerString* value) {
    joyeer_string_destroy(value);
}

void joyeer_byte_to_string_abi(JoyeerString* result, uint8_t value) {
    if (result == NULL) joyeer_panic("byteToString result is null");
    uint8_t* data = (uint8_t*)checkedAllocate(1);
    data[0] = value;
    result->data = data;
    result->count = 1;
}

static void setReadFileError(
        int32_t* resultTag,
        void* resultPayload,
        int32_t errorTag,
        int errorCode) {
    const int64_t value = errorCode == 0 ? EIO : errorCode;
    *resultTag = errorTag;
    memset(resultPayload, 0, sizeof(JoyeerString));
    memcpy(resultPayload, &value, sizeof(value));
}

static FILE* openBinaryFile(const char* path, int* errorCode) {
#if defined(_MSC_VER)
    FILE* file = NULL;
    const errno_t status = fopen_s(&file, path, "rb");
    *errorCode = (int)status;
    return file;
#else
    errno = 0;
    FILE* file = fopen(path, "rb");
    *errorCode = errno;
    return file;
#endif
}

void joyeer_read_file_abi(
        int32_t* resultTag,
        void* resultPayload,
        int32_t okTag,
        int32_t errorTag,
        const uint8_t* pathData,
        int64_t pathCount) {
    if (resultTag == NULL || resultPayload == NULL) {
        joyeer_panic("invalid readFile result storage");
    }
    memset(resultPayload, 0, sizeof(JoyeerString));
    if (pathCount < 0 || (pathCount != 0 && pathData == NULL) ||
        (uint64_t)pathCount >= SIZE_MAX) {
        setReadFileError(resultTag, resultPayload, errorTag, EINVAL);
        return;
    }

    char* path = (char*)checkedAllocate((size_t)pathCount + 1);
    if (pathCount != 0) memcpy(path, pathData, (size_t)pathCount);
    path[pathCount] = '\0';
    if (memchr(path, '\0', (size_t)pathCount) != NULL) {
        checkedFree(path);
        setReadFileError(resultTag, resultPayload, errorTag, EINVAL);
        return;
    }

    int openError = 0;
    FILE* file = openBinaryFile(path, &openError);
    checkedFree(path);
    if (file == NULL) {
        setReadFileError(resultTag, resultPayload, errorTag, openError);
        return;
    }

    size_t capacity = 4096;
    size_t count = 0;
    uint8_t* data = (uint8_t*)checkedAllocate(capacity);
    for (;;) {
        const size_t available = capacity - count;
        errno = 0;
        const size_t amount = fread(data + count, 1, available, file);
        count += amount;
        if (amount != available) {
            if (ferror(file)) {
                const int readError = errno;
                fclose(file);
                checkedFree(data);
                setReadFileError(resultTag, resultPayload, errorTag, readError);
                return;
            }
            break;
        }

        if (capacity > SIZE_MAX / 2 || (uint64_t)capacity * 2 > INT64_MAX) {
            fclose(file);
            checkedFree(data);
            setReadFileError(resultTag, resultPayload, errorTag, EFBIG);
            return;
        }
        capacity *= 2;
        data = (uint8_t*)checkedReallocate(data, capacity);
    }

    errno = 0;
    if (fclose(file) != 0) {
        const int closeError = errno;
        checkedFree(data);
        setReadFileError(resultTag, resultPayload, errorTag, closeError);
        return;
    }

    const JoyeerString value = { data, (int64_t)count };
    *resultTag = okTag;
    memcpy(resultPayload, &value, sizeof(value));
}

static JoyeerArray arrayCreateOwned(
        const void* values,
        int64_t count,
        int64_t elementSize,
        JoyeerCloneValueFn cloneElement,
        JoyeerDestroyValueFn destroyElement,
        bool cloneValues) {
    const size_t bytes = checkedByteCount(count, elementSize);
    if (bytes > SIZE_MAX - sizeof(ArrayHeader)) {
        joyeer_panic("array allocation size overflow");
    }
    ArrayHeader* storage = (ArrayHeader*)checkedAllocate(sizeof(ArrayHeader) + bytes);
    storage->elementSize = elementSize;
    storage->cloneElement = cloneElement;
    storage->destroyElement = destroyElement;
    void* data = storage + 1;
    if (bytes != 0) {
        if (values == NULL) joyeer_panic("array initializer is null");
        if (cloneValues && cloneElement != NULL) {
            for (int64_t index = 0; index < count; ++index) {
                cloneElement(
                        (uint8_t*)data + checkedByteCount(index, elementSize),
                        (const uint8_t*)values + checkedByteCount(index, elementSize));
            }
        } else {
            memcpy(data, values, bytes);
        }
    }
    JoyeerArray result = { data, count, count };
    return result;
}

JoyeerArray joyeer_array_create(const void* values, int64_t count, int64_t elementSize) {
    return arrayCreateOwned(values, count, elementSize, NULL, NULL, false);
}

void joyeer_array_create_owned_abi(
        JoyeerArray* result,
        const void* values,
        int64_t count,
        int64_t elementSize,
        JoyeerCloneValueFn cloneElement,
        JoyeerDestroyValueFn destroyElement) {
    if (result == NULL) joyeer_panic("array result is null");
    *result = arrayCreateOwned(
            values,
            count,
            elementSize,
            cloneElement,
            destroyElement,
            false);
}

void joyeer_array_clone_abi(
        JoyeerArray* result,
        const void* data,
        int64_t count) {
    if (result == NULL || data == NULL) joyeer_panic("invalid array clone");
    const ArrayHeader* source = ((const ArrayHeader*)data) - 1;
    *result = arrayCreateOwned(
            data,
            count,
            source->elementSize,
            source->cloneElement,
            source->destroyElement,
            true);
}

void joyeer_array_append_owned_abi(
        JoyeerArray* array,
        const void* element) {
    if (array == NULL || array->data == NULL || array->count < 0 ||
        array->capacity < array->count) {
        joyeer_panic("invalid array append");
    }
    ArrayHeader* header = ((ArrayHeader*)array->data) - 1;
    if (header->elementSize < 0 ||
        (header->elementSize != 0 && element == NULL) ||
        array->count == INT64_MAX) {
        joyeer_panic("invalid array append");
    }

    if (array->count == array->capacity) {
        int64_t capacity = array->capacity < 4 ? 4 : array->capacity;
        if (capacity == array->capacity) {
            if (capacity > INT64_MAX / 2) joyeer_panic("array capacity overflow");
            capacity *= 2;
        }
        const size_t bytes = checkedByteCount(capacity, header->elementSize);
        if (bytes > SIZE_MAX - sizeof(ArrayHeader)) {
            joyeer_panic("array allocation size overflow");
        }
        header = (ArrayHeader*)checkedReallocate(
                header,
                sizeof(ArrayHeader) + bytes);
        array->data = header + 1;
        array->capacity = capacity;
    }

    uint8_t* destination = (uint8_t*)array->data +
            checkedByteCount(array->count, header->elementSize);
    if (header->elementSize != 0) {
        memcpy(destination, element, (size_t)header->elementSize);
    }
    ++array->count;
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
    ArrayHeader* header = ((ArrayHeader*)array->data) - 1;
    if (header->destroyElement != NULL) {
        for (int64_t index = array->count; index > 0; --index) {
            header->destroyElement(
                    (uint8_t*)array->data +
                    checkedByteCount(index - 1, header->elementSize));
        }
    }
    checkedFree(header);
    array->data = NULL;
    array->count = 0;
    array->capacity = 0;
}

void joyeer_array_destroy_abi(JoyeerArray* array) {
    joyeer_array_destroy(array);
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

static JoyeerDictionary dictionaryCreateOwned(
        const void* entries,
        int64_t count,
        int64_t keySize,
        int64_t valueSize,
        int64_t entrySize,
        int64_t valueOffset,
        int32_t keyKind,
        JoyeerCloneValueFn cloneKey,
        JoyeerDestroyValueFn destroyKey,
        JoyeerCloneValueFn cloneValue,
        JoyeerDestroyValueFn destroyValue,
        bool cloneEntries) {
    if (keySize <= 0 || valueSize < 0 || entrySize < keySize ||
        valueOffset < keySize || valueOffset + valueSize > entrySize) {
        joyeer_panic("invalid dictionary layout");
    }
    const size_t bytes = checkedByteCount(count, entrySize);
    if (bytes > SIZE_MAX - sizeof(DictionaryHeader)) {
        joyeer_panic("dictionary allocation size overflow");
    }
    DictionaryHeader* storage =
            (DictionaryHeader*)checkedAllocate(sizeof(DictionaryHeader) + bytes);
    storage->keySize = keySize;
    storage->valueSize = valueSize;
    storage->entrySize = entrySize;
    storage->valueOffset = valueOffset;
    storage->keyKind = keyKind;
    storage->cloneKey = cloneKey;
    storage->destroyKey = destroyKey;
    storage->cloneValue = cloneValue;
    storage->destroyValue = destroyValue;
    void* data = storage + 1;
    if (bytes != 0) {
        if (entries == NULL) joyeer_panic("dictionary initializer is null");
        if (cloneEntries && (cloneKey != NULL || cloneValue != NULL)) {
            memset(data, 0, bytes);
            for (int64_t index = 0; index < count; ++index) {
                const uint8_t* source = (const uint8_t*)entries +
                        checkedByteCount(index, entrySize);
                uint8_t* destination = (uint8_t*)data +
                        checkedByteCount(index, entrySize);
                if (cloneKey != NULL) cloneKey(destination, source);
                else memcpy(destination, source, (size_t)keySize);
                if (cloneValue != NULL) {
                    cloneValue(destination + valueOffset, source + valueOffset);
                } else {
                    memcpy(
                            destination + valueOffset,
                            source + valueOffset,
                            (size_t)valueSize);
                }
            }
        } else {
            memcpy(data, entries, bytes);
        }
    }
    JoyeerDictionary result = { data, count, count };
    return result;
}

JoyeerDictionary joyeer_dictionary_create(
        const void* entries,
        int64_t count,
        int64_t keySize,
        int64_t valueSize,
        int64_t entrySize,
        int64_t valueOffset,
        int32_t keyKind) {
    return dictionaryCreateOwned(
            entries,
            count,
            keySize,
            valueSize,
            entrySize,
            valueOffset,
            keyKind,
            NULL,
            NULL,
            NULL,
            NULL,
            false);
}

void joyeer_dictionary_create_owned_abi(
        JoyeerDictionary* result,
        const void* entries,
        int64_t count,
        int64_t keySize,
        int64_t valueSize,
        int64_t entrySize,
        int64_t valueOffset,
        int32_t keyKind,
        JoyeerCloneValueFn cloneKey,
        JoyeerDestroyValueFn destroyKey,
        JoyeerCloneValueFn cloneValue,
        JoyeerDestroyValueFn destroyValue) {
    if (result == NULL) joyeer_panic("dictionary result is null");
    *result = dictionaryCreateOwned(
            entries,
            count,
            keySize,
            valueSize,
            entrySize,
            valueOffset,
            keyKind,
            cloneKey,
            destroyKey,
            cloneValue,
            destroyValue,
            false);
}

void joyeer_dictionary_clone_abi(
        JoyeerDictionary* result,
        const void* data,
        int64_t count) {
    if (result == NULL || data == NULL) joyeer_panic("invalid dictionary clone");
    const DictionaryHeader* source = ((const DictionaryHeader*)data) - 1;
    *result = dictionaryCreateOwned(
            data,
            count,
            source->keySize,
            source->valueSize,
            source->entrySize,
            source->valueOffset,
            source->keyKind,
            source->cloneKey,
            source->destroyKey,
            source->cloneValue,
            source->destroyValue,
            true);
}

void joyeer_dictionary_set_owned_abi(
        JoyeerDictionary* dictionary,
    void* key,
        const void* value) {
    if (dictionary == NULL || dictionary->data == NULL || key == NULL ||
        dictionary->count < 0 || dictionary->capacity < dictionary->count) {
        joyeer_panic("invalid dictionary set");
    }
    DictionaryHeader* header = ((DictionaryHeader*)dictionary->data) - 1;
    if (header->keySize <= 0 || header->valueSize < 0 ||
        (header->valueSize != 0 && value == NULL) ||
        dictionary->count == INT64_MAX) {
        joyeer_panic("invalid dictionary set");
    }

    for (int64_t index = 0; index < dictionary->count; ++index) {
        uint8_t* entry = (uint8_t*)dictionary->data +
                checkedByteCount(index, header->entrySize);
        if (!dictionaryKeyEqual(entry, key, header->keySize, header->keyKind)) {
            continue;
        }
        if (header->destroyKey != NULL) {
            header->destroyKey(key);
        }
        if (header->destroyValue != NULL) {
            header->destroyValue(entry + header->valueOffset);
        }
        if (header->valueSize != 0) {
            memcpy(
                    entry + header->valueOffset,
                    value,
                    (size_t)header->valueSize);
        }
        return;
    }

    if (dictionary->count == dictionary->capacity) {
        int64_t capacity = dictionary->capacity < 4 ? 4 : dictionary->capacity;
        if (capacity == dictionary->capacity) {
            if (capacity > INT64_MAX / 2) {
                joyeer_panic("dictionary capacity overflow");
            }
            capacity *= 2;
        }
        const size_t bytes = checkedByteCount(capacity, header->entrySize);
        if (bytes > SIZE_MAX - sizeof(DictionaryHeader)) {
            joyeer_panic("dictionary allocation size overflow");
        }
        header = (DictionaryHeader*)checkedReallocate(
                header,
                sizeof(DictionaryHeader) + bytes);
        dictionary->data = header + 1;
        dictionary->capacity = capacity;
    }

    uint8_t* entry = (uint8_t*)dictionary->data +
            checkedByteCount(dictionary->count, header->entrySize);
    memset(entry, 0, (size_t)header->entrySize);
    memcpy(entry, key, (size_t)header->keySize);
    if (header->valueSize != 0) {
        memcpy(
                entry + header->valueOffset,
                value,
                (size_t)header->valueSize);
    }
    ++dictionary->count;
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
    DictionaryHeader* header = ((DictionaryHeader*)dictionary->data) - 1;
    for (int64_t index = dictionary->count; index > 0; --index) {
        uint8_t* entry = (uint8_t*)dictionary->data +
                checkedByteCount(index - 1, header->entrySize);
        if (header->destroyValue != NULL) {
            header->destroyValue(entry + header->valueOffset);
        }
        if (header->destroyKey != NULL) header->destroyKey(entry);
    }
    checkedFree(header);
    dictionary->data = NULL;
    dictionary->count = 0;
    dictionary->capacity = 0;
}

void joyeer_dictionary_destroy_abi(JoyeerDictionary* dictionary) {
    joyeer_dictionary_destroy(dictionary);
}

void joyeer_print_string_abi(const uint8_t* data, int64_t count) {
    joyeer_print_string((JoyeerString) { data, count });
}

void joyeer_string_concat_abi(
        JoyeerString* result,
        const uint8_t* leftData,
        int64_t leftCount,
        const uint8_t* rightData,
        int64_t rightCount) {
    if (result == NULL) joyeer_panic("string result is null");
    *result = joyeer_string_concat(
            (JoyeerString) { leftData, leftCount },
            (JoyeerString) { rightData, rightCount });
}

bool joyeer_string_equal_abi(
        const uint8_t* leftData,
        int64_t leftCount,
        const uint8_t* rightData,
        int64_t rightCount) {
    return joyeer_string_equal(
            (JoyeerString) { leftData, leftCount },
            (JoyeerString) { rightData, rightCount });
}

int64_t joyeer_string_compare_abi(
        const uint8_t* leftData,
        int64_t leftCount,
        const uint8_t* rightData,
        int64_t rightCount) {
    return joyeer_string_compare(
            (JoyeerString) { leftData, leftCount },
            (JoyeerString) { rightData, rightCount });
}

uint8_t joyeer_string_byte_at_abi(const uint8_t* data, int64_t count, int64_t index) {
    return joyeer_string_byte_at((JoyeerString) { data, count }, index);
}

void joyeer_array_create_abi(
        JoyeerArray* result,
        const void* values,
        int64_t count,
        int64_t elementSize) {
    if (result == NULL) joyeer_panic("array result is null");
    *result = joyeer_array_create(values, count, elementSize);
}

void* joyeer_array_at_abi(void* data, int64_t count, int64_t index) {
    return joyeer_array_at((JoyeerArray) { data, count, count }, index);
}

void joyeer_dictionary_create_abi(
        JoyeerDictionary* result,
        const void* entries,
        int64_t count,
        int64_t keySize,
        int64_t valueSize,
        int64_t entrySize,
        int64_t valueOffset,
        int32_t keyKind) {
    if (result == NULL) joyeer_panic("dictionary result is null");
    *result = joyeer_dictionary_create(
            entries,
            count,
            keySize,
            valueSize,
            entrySize,
            valueOffset,
            keyKind);
}

void* joyeer_dictionary_at_abi(
        void* data,
        int64_t count,
        const void* key,
        int64_t keySize,
        int32_t keyKind) {
    return joyeer_dictionary_at(
            (JoyeerDictionary) { data, count, count },
            key,
            keySize,
            keyKind);
}
