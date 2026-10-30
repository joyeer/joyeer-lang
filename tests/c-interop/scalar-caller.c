#include "scalar-fixture.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "C scalar check failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define CHECK_VALUES(type, name, ...) do { \
    const type values[] = {__VA_ARGS__}; \
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) { \
        CHECK(joyeer_scalar_##name(values[index]) == values[index]); \
    } \
} while (0)

int cinterop_check_scalars(void) {
    CHECK_VALUES(bool, boolean, false, true, 7, -1);
    CHECK_VALUES(int8_t, s8, 0, 1, -1, INT8_MIN, INT8_MAX);
    CHECK_VALUES(uint8_t, u8, 0, 1, 128, UINT8_MAX);
    CHECK_VALUES(int16_t, s16, 0, 1, -1, INT16_MIN, INT16_MAX);
    CHECK_VALUES(uint16_t, u16, 0, 1, 32768, UINT16_MAX);
    CHECK_VALUES(int32_t, s32, 0, 1, -1, INT32_MIN, INT32_MAX);
    CHECK_VALUES(uint32_t, u32, 0, 1, UINT32_C(2147483648), UINT32_MAX);
    CHECK_VALUES(int64_t, s64, 0, 1, -1, INT64_MIN, INT64_MAX);
    CHECK_VALUES(uint64_t, u64, 0, 1, UINT64_C(9223372036854775808), UINT64_MAX);
    CHECK_VALUES(int64_t, word, 0, 1, -1, INT64_MIN, INT64_MAX);
    CHECK_VALUES(uint64_t, uword, 0, 1, UINT64_C(9223372036854775808), UINT64_MAX);
    CHECK_VALUES(float, f32, 0.0f, -0.0f, 1.25f, -2.5f, FLT_MIN, FLT_MAX, FLT_TRUE_MIN,
            INFINITY, -INFINITY);
    CHECK_VALUES(double, f64, 0.0, -0.0, 1.25, -2.5, DBL_MIN, DBL_MAX, DBL_TRUE_MIN,
            INFINITY, -INFINITY);
    CHECK(!signbit(joyeer_scalar_f32(0.0f)));
    CHECK(signbit(joyeer_scalar_f32(-0.0f)));
    CHECK(!signbit(joyeer_scalar_f64(0.0)));
    CHECK(signbit(joyeer_scalar_f64(-0.0)));
    CHECK(isnan(joyeer_scalar_f32(NAN)));
    CHECK(isnan(joyeer_scalar_f64(NAN)));
    CHECK(!joyeer_scalar_truth(0));
    CHECK(joyeer_scalar_truth(1));
    CHECK(joyeer_scalar_truth(INT32_MIN));
    CHECK(joyeer_scalar_truth(INT32_MAX));
    CHECK(joyeer_scalar_mixed(INT8_MIN, 1.25f, UINT8_MAX, -2.5, INT16_MIN, 3.75f,
            UINT16_MAX, -4.25, INT32_MIN, 5.5f, UINT32_MAX, -6.75, INT64_MIN, 7.25f,
            UINT64_MAX, -8.5, INT64_MAX, 9.75f, UINT64_MAX, -10.25, true));
    CHECK(joyeer_scalar_platform(LONG_MIN, ULONG_MAX, CHAR_MIN, SIZE_MAX, PTRDIFF_MIN));
    CHECK(joyeer_scalar_platform_bool(-123) == -123);
    CHECK(joyeer_scalar_minimum() == INT64_MIN);
    CHECK(joyeer_scalar_maximum() == UINT64_MAX);
    CHECK(signbit(joyeer_scalar_negative_zero()));
    CHECK(joyeer_scalar_float_literal() == 1.0f + FLT_EPSILON);
    CHECK(joyeer_scalar_double_literal() == 1.0 + DBL_EPSILON);
    CHECK(signbit(joyeer_scalar_negate_float(0.0f)));
    CHECK(!signbit(joyeer_scalar_negate_float(-0.0f)));
    CHECK(joyeer_scalar_negate_float(1.25f) == -1.25f);
    CHECK(isinf(joyeer_scalar_divide_double(1.0, 0.0)));
    CHECK(signbit(joyeer_scalar_divide_double(1.0, -0.0)));
    CHECK(joyeer_scalar_is_nan(NAN));
    CHECK(!joyeer_scalar_is_nan(INFINITY));
    CHECK(!joyeer_scalar_is_nan(1.0));
    CHECK(joyeer_scalar_choose_unsigned(true) == UINT64_MAX);
    CHECK(joyeer_scalar_choose_unsigned(false) == 0);
    return 0;
}
