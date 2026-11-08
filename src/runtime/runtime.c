#include "runtime.h"
#include "native.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#elif !defined(_WIN32)
extern char** environ;
#endif

int32_t joyeer_native_errno(void) {
    return (int32_t)errno;
}

int64_t joyeer_native_environment(void) {
#if defined(_WIN32)
    return 0;
#elif defined(__APPLE__)
    return (int64_t)(intptr_t)*_NSGetEnviron();
#else
    return (int64_t)(intptr_t)environ;
#endif
}

int64_t joyeer_native_stdio(int32_t which) {
    switch (which) {
    case 0: return (int64_t)(intptr_t)stdin;
    case 1: return (int64_t)(intptr_t)stdout;
    case 2: return (int64_t)(intptr_t)stderr;
    default: return 0;
    }
}

void joyeer_print_float(float value) {
    printf("%.9g\n", (double)value);
}

void joyeer_print_double(double value) {
    printf("%.17g\n", value);
}

