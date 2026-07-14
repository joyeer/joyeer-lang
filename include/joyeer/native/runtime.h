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

enum JoyeerDictionaryKeyKind {
    JOYEER_DICTIONARY_KEY_INT = 1,
    JOYEER_DICTIONARY_KEY_BOOL = 2,
    JOYEER_DICTIONARY_KEY_STRING = 3,
    JOYEER_DICTIONARY_KEY_BYTE = 4,
};

JOYEER_NORETURN void joyeer_panic(const char* message);

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

JoyeerArray joyeer_array_create(const void* values, int64_t count, int64_t elementSize);
void* joyeer_array_at(JoyeerArray array, int64_t index);
void joyeer_array_destroy(JoyeerArray* array);

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

#ifdef __cplusplus
}
#endif

#undef JOYEER_NORETURN

#endif
