#ifndef __joyeer_runtime_h__
#define __joyeer_runtime_h__

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

// Writes `Joyeer runtime error: <message>` to standard error and exits with
// status 1; a null message reads "unknown error". Written in Joyeer
// (src/runtime/joyeer/panic.joyeer) and exported with this C ABI.
JOYEER_NORETURN void joyeer_panic(const char* message);

// The runtime allocator and its leak counter, written in Joyeer
// (src/runtime/joyeer/allocator.joyeer). A size is the byte count the block was
// last requested with (a zero-byte request is served as one byte), passed back to
// `joyeer_realloc` and `joyeer_free`. All three panic rather than return failure.
void* joyeer_alloc(size_t size);
void* joyeer_realloc(void* address, size_t oldSize, size_t newSize);
void joyeer_free(void* address, size_t size);
// Blocks allocated and not yet freed.
int64_t joyeer_runtime_active_allocations(void);
// The calling thread's C-runtime errno. A Joyeer binding cannot dereference
// the pointer that errno expands to, so native bindings read it through this.
int32_t joyeer_native_errno(void);
// The address of the process environment block (`environ`), or 0 on Windows.
// POSIX process spawning passes it as `envp`; a Joyeer binding cannot name the
// C global.
int64_t joyeer_native_environment(void);
// The C `FILE*` for standard input (0), output (1) or error (2) as an integer,
// or 0 for any other value. A Joyeer binding cannot name the C globals, and the
// stdio functions it calls need the stream.
int64_t joyeer_native_stdio(int32_t which);
// True when the bytes are strict UTF-8: no overlong sequence, surrogate,
// truncated sequence or code point beyond U+10FFFF. Null data is valid only for
// a count of zero; a negative count is invalid. Written in Joyeer
// (src/runtime/joyeer/utf8.joyeer) and exported with this C ABI.
bool joyeer_host_valid_utf8(const uint8_t* data, int64_t count);

void joyeer_print_int(int64_t value);
void joyeer_print_uint(uint64_t value);
void joyeer_print_float(float value);
void joyeer_print_double(double value);
void joyeer_print_bool(bool value);
void joyeer_print_byte(uint8_t value);

void joyeer_string_clone_abi(
    JoyeerString* result,
    const uint8_t* data,
    int64_t count);
void joyeer_string_destroy_abi(JoyeerString* value);

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
void joyeer_array_append_owned_abi(
    JoyeerArray* array,
    const void* element);
void joyeer_array_destroy_abi(JoyeerArray* array);

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
void joyeer_dictionary_set_owned_abi(
    JoyeerDictionary* dictionary,
    void* key,
    const void* value);
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
void joyeer_byte_to_string_abi(JoyeerString* result, uint8_t value);
void* joyeer_dictionary_at_abi(
    void* data,
    int64_t count,
    const void* key,
    int64_t keySize,
    int32_t keyKind);
// Returns borrowed value storage, or null for a missing key. A present
// zero-sized value still returns non-null; invalid lookups still panic.
void* joyeer_dictionary_find_abi(
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
