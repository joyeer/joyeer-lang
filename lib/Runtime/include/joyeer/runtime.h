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
// (lib/Runtime/core/panic.joyeer) and exported with this C ABI.
JOYEER_NORETURN void joyeer_panic(const char* message);

// The runtime allocator and its leak counter, written in Joyeer
// (lib/Runtime/core/allocator.joyeer). A size is the byte count the block was
// last requested with (a zero-byte request is served as one byte), passed back to
// `joyeer_realloc` and `joyeer_free`. All three panic rather than return failure.
void* joyeer_alloc(size_t size);
void* joyeer_realloc(void* address, size_t oldSize, size_t newSize);
void joyeer_free(void* address, size_t size);
// Blocks allocated and not yet freed.
int64_t joyeer_runtime_active_allocations(void);

// The process's standard streams, written in Joyeer straight over the operating system
// (lib/Runtime/core/streams.joyeer). Standard output is buffered and shared with `print`;
// standard error is not. Each returns 0 or an errno-style code (9, 13, 28, 32, 5).
int32_t joyeer_stdout_write(const uint8_t* data, int64_t count);
int32_t joyeer_stdout_flush(void);
int32_t joyeer_stderr_write(const uint8_t* data, int64_t count);
// The next byte of standard input (0 to 255), -1 at the end of the input, or a negated
// error code (-2 or below).
int32_t joyeer_stdin_read_byte(void);
// Drops the buffered, unread standard input (for a program that redirects its own).
void joyeer_stdin_discard(void);
// True when the bytes are strict UTF-8: no overlong sequence, surrogate,
// truncated sequence or code point beyond U+10FFFF. Null data is valid only for
// a count of zero; a negative count is invalid. Written in Joyeer
// (lib/Runtime/core/utf8.joyeer) and exported with this C ABI.
bool joyeer_host_valid_utf8(const uint8_t* data, int64_t count);

void joyeer_print_int(int64_t value);
void joyeer_print_uint(uint64_t value);
// `print` of a Float (9 significant digits) or Double (17), written in Joyeer
// (lib/Runtime/core/floats.joyeer, print.joyeer).
void joyeer_print_float(float value);
void joyeer_print_double(double value);
// Formats `value` like the C library's `%.<precision>g` (precision 1 to 17, NaN as
// `nan`) into `buffer`, which must hold 40 bytes, with no terminator. Returns the
// number of bytes written.
int64_t joyeer_format_double_abi(char* buffer, int32_t precision, double value);
int64_t joyeer_format_float_abi(char* buffer, int32_t precision, float value);
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
// Moves the last element into `element` and returns 1, or returns 0 when the array is empty.
int64_t joyeer_array_remove_last_abi(
    JoyeerArray* array,
    void* element);
// Ends an empty array, freeing its storage; traps with `array not empty` otherwise.
void joyeer_array_consume_empty_abi(JoyeerArray* array);
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

/* Threads, a blocking mutex and a condition variable (decision 0016). They live in a separate
 * object of the runtime archive, linked only into a program that references them. The runtime is
 * not thread safe yet: a function run on a thread started here must not allocate, print or panic.
 * Every result is >= 0 on success or a negative code: the negated operating-system error, or one
 * of the two codes below. */
#define JOYEER_THREAD_BYTES 128 /* zeroed, 8-byte-aligned storage for one thread */
#define JOYEER_LOCK_BYTES 64    /* zeroed, 8-byte-aligned storage for a mutex or a condition */
#define JOYEER_MAX_THREADS 256  /* unjoined threads at once */
#define JOYEER_THREAD_LIMIT ((int64_t)-1073741824)
#define JOYEER_THREAD_INVALID ((int64_t)-1073741823)

typedef void (*JoyeerThreadEntry)(void* argument);

/* Starts a thread running entry(argument). `thread` must stay valid until joined. */
int64_t joyeer_thread_start_abi(void* thread, JoyeerThreadEntry entry, void* argument);
/* Waits for the thread to finish and releases it; every started thread is joined once. */
int64_t joyeer_thread_join_abi(void* thread);

int64_t joyeer_mutex_init_abi(void* storage);
int64_t joyeer_mutex_lock_abi(void* storage);
int64_t joyeer_mutex_unlock_abi(void* storage);
int64_t joyeer_mutex_destroy_abi(void* storage);

int64_t joyeer_condition_init_abi(void* storage);
/* Waits with `mutex` held; a wait can wake without a signal, so wait in a loop. */
int64_t joyeer_condition_wait_abi(void* condition, void* mutex);
int64_t joyeer_condition_signal_abi(void* condition);
int64_t joyeer_condition_broadcast_abi(void* condition);
int64_t joyeer_condition_destroy_abi(void* condition);

#ifdef __cplusplus
}
#endif

#undef JOYEER_NORETURN

#endif
