#ifndef __joyeer_native_runtime_h__
#define __joyeer_native_runtime_h__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define JOYEER_NORETURN [[noreturn]]
#else
#define JOYEER_NORETURN _Noreturn
#endif

typedef struct JoyeerString {
    const uint8_t* data;
    int64_t count;
} JoyeerString;

typedef struct JoyeerArray {
    void* data;
    int64_t count;
    int64_t capacity;
} JoyeerArray;

typedef struct JoyeerDictionary {
    void* data;
    int64_t count;
    int64_t capacity;
} JoyeerDictionary;

typedef void (*JoyeerCloneValueFn)(void* destination, const void* source);
typedef void (*JoyeerDestroyValueFn)(void* value);

enum JoyeerDictionaryKeyKind {
    JOYEER_DICTIONARY_KEY_INT = 1,
    JOYEER_DICTIONARY_KEY_BOOL = 2,
    JOYEER_DICTIONARY_KEY_STRING = 3,
    JOYEER_DICTIONARY_KEY_BYTE = 4,
};

JOYEER_NORETURN void joyeer_panic(const char* message);
int64_t joyeer_runtime_active_allocations(void);

int64_t joyeer_checked_add_int(int64_t left, int64_t right);
int64_t joyeer_checked_sub_int(int64_t left, int64_t right);
int64_t joyeer_checked_mul_int(int64_t left, int64_t right);

void joyeer_print_int(int64_t value);
void joyeer_print_bool(bool value);
void joyeer_print_byte(uint8_t value);
void joyeer_print_string(JoyeerString value);

JoyeerString joyeer_string_concat(JoyeerString left, JoyeerString right);
bool joyeer_string_equal(JoyeerString left, JoyeerString right);
int64_t joyeer_string_compare(JoyeerString left, JoyeerString right);
uint8_t joyeer_string_byte_at(JoyeerString value, int64_t index);
void joyeer_string_destroy(JoyeerString* value);
void joyeer_string_clone_abi(
    JoyeerString* result,
    const uint8_t* data,
    int64_t count);
void joyeer_string_destroy_abi(JoyeerString* value);

JoyeerArray joyeer_array_create(const void* values, int64_t count, int64_t elementSize);
void* joyeer_array_at(JoyeerArray array, int64_t index);
void joyeer_array_destroy(JoyeerArray* array);
void joyeer_array_create_owned_abi(
    JoyeerArray* result,
    const void* values,
    int64_t count,
    int64_t elementSize,
    JoyeerCloneValueFn cloneElement,
    JoyeerDestroyValueFn destroyElement);
void joyeer_array_clone_abi(
    JoyeerArray* result,
    const void* data,
    int64_t count);
void joyeer_array_destroy_abi(JoyeerArray* array);

JoyeerDictionary joyeer_dictionary_create(
        const void* entries,
        int64_t count,
        int64_t keySize,
        int64_t valueSize,
        int64_t entrySize,
        int64_t valueOffset,
        int32_t keyKind);
void* joyeer_dictionary_at(
        JoyeerDictionary dictionary,
        const void* key,
        int64_t keySize,
        int32_t keyKind);
void joyeer_dictionary_destroy(JoyeerDictionary* dictionary);
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
    JoyeerDestroyValueFn destroyValue);
void joyeer_dictionary_clone_abi(
    JoyeerDictionary* result,
    const void* data,
    int64_t count);
void joyeer_dictionary_destroy_abi(JoyeerDictionary* dictionary);

// Stable compiler ABI. These functions deliberately avoid passing or
// returning C structs by value, whose ABI differs across targets.
void joyeer_print_string_abi(const uint8_t* data, int64_t count);
void joyeer_string_concat_abi(
    JoyeerString* result,
    const uint8_t* leftData,
    int64_t leftCount,
    const uint8_t* rightData,
    int64_t rightCount);
bool joyeer_string_equal_abi(
    const uint8_t* leftData,
    int64_t leftCount,
    const uint8_t* rightData,
    int64_t rightCount);
int64_t joyeer_string_compare_abi(
    const uint8_t* leftData,
    int64_t leftCount,
    const uint8_t* rightData,
    int64_t rightCount);
uint8_t joyeer_string_byte_at_abi(const uint8_t* data, int64_t count, int64_t index);

void joyeer_array_create_abi(
    JoyeerArray* result,
    const void* values,
    int64_t count,
    int64_t elementSize);
void* joyeer_array_at_abi(void* data, int64_t count, int64_t index);

void joyeer_dictionary_create_abi(
    JoyeerDictionary* result,
    const void* entries,
    int64_t count,
    int64_t keySize,
    int64_t valueSize,
    int64_t entrySize,
    int64_t valueOffset,
    int32_t keyKind);
void* joyeer_dictionary_at_abi(
    void* data,
    int64_t count,
    const void* key,
    int64_t keySize,
    int32_t keyKind);

#ifdef __cplusplus
}
#endif

#undef JOYEER_NORETURN

#endif
