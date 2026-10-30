#include <stdint.h>

#if defined(_WIN32) && defined(CINTEROP_SHARED)
#define CINTEROP_API __declspec(dllexport)
#else
#define CINTEROP_API
#endif

static int64_t stored_value;

CINTEROP_API int64_t c_fixture_identity(int64_t value) {
    return value;
}

CINTEROP_API int64_t c_fixture_zero(void) {
    return -17;
}

CINTEROP_API void c_fixture_store(int64_t value) {
    stored_value = value;
}

CINTEROP_API int64_t c_fixture_load(void) {
    return stored_value;
}

CINTEROP_API int64_t c_fixture_arguments(
        int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f,
        int64_t g, int64_t h, int64_t i, int64_t j, int64_t k, int64_t l) {
    return a == -1 && b == 2 && c == -3 && d == 4 && e == -5 && f == 6 &&
           g == -7 && h == 8 && i == -9 && j == 10 && k == -11 && l == 12;
}
