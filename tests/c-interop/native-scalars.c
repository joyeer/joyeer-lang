#include "scalar-fixture.h"

#include <float.h>
#include <limits.h>

#if defined(_WIN32)
_Static_assert(sizeof(BOOL) == sizeof(int32_t), "Windows BOOL must be a signed 32-bit integer");
#endif

_Static_assert(CHAR_BIT == 8 && sizeof(short) == 2 && sizeof(int) == 4 &&
        sizeof(long long) == 8, "unsupported C integer representation");
_Static_assert((CHAR_MIN < 0) == CINTEROP_CHAR_SIGNED, "configured plain-char signedness is stale");
_Static_assert(sizeof(void*) == 8 && sizeof(size_t) == 8 && sizeof(ptrdiff_t) == 8,
        "C scalar fixtures require a 64-bit target");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 &&
        DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024, "unsupported C floating representation");
#if defined(_WIN32)
_Static_assert(sizeof(long) == 4, "Windows fixtures require LLP64");
#else
_Static_assert(sizeof(long) == 8, "Unix fixtures require LP64");
#endif

#define CINTEROP_DEFINE(type, name) \
    CINTEROP_SCALAR_API type c_scalar_##name(type value) { return value; }
CINTEROP_SCALARS(CINTEROP_DEFINE)
#undef CINTEROP_DEFINE

CINTEROP_SCALAR_API bool c_scalar_truth(int32_t value) {
    return value != 0;
}

CINTEROP_SCALAR_API bool c_scalar_mixed(CINTEROP_MIXED_PARAMETERS) {
    return a == INT8_MIN && b == UINT8_MAX && c == INT16_MIN && d == UINT16_MAX &&
            e == INT32_MIN && f == UINT32_MAX && g == INT64_MIN && h == UINT64_MAX &&
            i == INT64_MAX && j == UINT64_MAX && flag &&
            f0 == 1.25f && d0 == -2.5 && f1 == 3.75f && d1 == -4.25 &&
            f2 == 5.5f && d2 == -6.75 && f3 == 7.25f && d3 == -8.5 &&
            f4 == 9.75f && d4 == -10.25;
}

CINTEROP_SCALAR_API bool c_scalar_platform(long a, unsigned long b, char c, size_t d, ptrdiff_t e) {
    return a == LONG_MIN && b == ULONG_MAX && c == CHAR_MIN && d == SIZE_MAX && e == PTRDIFF_MIN;
}

CINTEROP_SCALAR_API CInteropPlatformBool c_scalar_platform_bool(CInteropPlatformBool value) {
    return value;
}
