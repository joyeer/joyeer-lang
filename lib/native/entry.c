#include "joyeer/native/runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef wchar_t JoyeerHostChar;
#else
typedef char JoyeerHostChar;
#endif

int64_t joyeer_main(const JoyeerArray* arguments);
int joyeer_main_uses_arguments(void);

static void cloneArgument(void* destination, const void* source) {
    const JoyeerString* string = (const JoyeerString*)source;
    joyeer_string_clone_abi((JoyeerString*)destination, string->data, string->count);
}

static void destroyArgument(void* value) {
    joyeer_string_destroy_abi((JoyeerString*)value);
}

#if defined(_WIN32)
static void copyArgument(JoyeerString* destination, const JoyeerHostChar* argument) {
    if (argument == NULL) joyeer_panic("invalid command line argument");
    const int count = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, NULL, 0, NULL, NULL);
    if (count == 0) joyeer_panic("invalid Windows command line argument encoding");
    char* bytes = (char*)malloc((size_t)count);
    if (bytes == NULL) joyeer_panic("out of memory");
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, bytes, count, NULL, NULL) != count) {
        joyeer_panic("invalid Windows command line argument encoding");
    }
    joyeer_string_clone_abi(destination, (const uint8_t*)bytes, (int64_t)count - 1);
    free(bytes);
}
#else
static void copyArgument(JoyeerString* destination, const JoyeerHostChar* argument) {
    if (argument == NULL) joyeer_panic("invalid command line argument");
    const size_t count = strlen(argument);
    if (count > INT64_MAX) joyeer_panic("command line argument is too large");
    joyeer_string_clone_abi(destination, (const uint8_t*)argument, (int64_t)count);
}
#endif

static void createArguments(
        JoyeerArray* result, int argc, JoyeerHostChar* const argv[]) {
    if (argc < 1 || argv == NULL) joyeer_panic("invalid command line arguments");
    const size_t count = (size_t)(argc - 1);
    if (count > SIZE_MAX / sizeof(JoyeerString)) {
        joyeer_panic("command line argument count is too large");
    }
    JoyeerString* strings = count == 0
            ? NULL
            : (JoyeerString*)malloc(count * sizeof(JoyeerString));
    if (count != 0 && strings == NULL) joyeer_panic("out of memory");
    for (size_t index = 0; index < count; ++index) {
        copyArgument(&strings[index], argv[index + 1]);
    }
    joyeer_array_create_owned_abi(
            result, strings, (int64_t)count, sizeof(JoyeerString),
            cloneArgument, destroyArgument);
    free(strings);
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    JoyeerArray arguments = { 0 };
    if (joyeer_main_uses_arguments()) createArguments(&arguments, argc, argv);
    const int64_t status = joyeer_main(&arguments);
    joyeer_array_destroy_abi(&arguments);
    const int64_t allocations = joyeer_runtime_active_allocations();
    if (allocations != 0) {
        fprintf(stderr, "Joyeer runtime error: %" PRId64 " leaked allocation(s)\n", allocations);
        return EXIT_FAILURE;
    }
    if (status < 0 || status > 255) {
        fprintf(stderr,
                "Joyeer runtime error: entry returned exit code %" PRId64
                " outside 0..255\n",
                status);
        return EXIT_FAILURE;
    }
    return (int)status;
}
