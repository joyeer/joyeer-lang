#ifndef JOYEER_SCALAR_FIXTURE_H
#define JOYEER_SCALAR_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
typedef BOOL CInteropPlatformBool;
#else
typedef int32_t CInteropPlatformBool;
#endif

#if defined(_WIN32) && defined(CINTEROP_SHARED)
#define CINTEROP_SCALAR_API __declspec(dllexport)
#else
#define CINTEROP_SCALAR_API
#endif

#define CINTEROP_SCALARS(X) \
    X(bool, boolean) \
    X(int8_t, s8) X(uint8_t, u8) \
    X(int16_t, s16) X(uint16_t, u16) \
    X(int32_t, s32) X(uint32_t, u32) \
    X(int64_t, s64) X(uint64_t, u64) \
    X(int64_t, word) X(uint64_t, uword) \
    X(float, f32) X(double, f64)

#define CINTEROP_DECLARE(type, name) \
    CINTEROP_SCALAR_API type c_scalar_##name(type value); \
    type joyeer_scalar_##name(type value);
CINTEROP_SCALARS(CINTEROP_DECLARE)
#undef CINTEROP_DECLARE

#define CINTEROP_MIXED_PARAMETERS \
    int8_t a, float f0, uint8_t b, double d0, \
    int16_t c, float f1, uint16_t d, double d1, int32_t e, float f2, \
    uint32_t f, double d2, int64_t g, float f3, uint64_t h, double d3, \
    int64_t i, float f4, uint64_t j, double d4, bool flag

CINTEROP_SCALAR_API bool c_scalar_mixed(CINTEROP_MIXED_PARAMETERS);
bool joyeer_scalar_mixed(CINTEROP_MIXED_PARAMETERS);
CINTEROP_SCALAR_API bool c_scalar_truth(int32_t value);
bool joyeer_scalar_truth(int32_t value);
CINTEROP_SCALAR_API bool c_scalar_platform(long a, unsigned long b, char c, size_t d, ptrdiff_t e);
bool joyeer_scalar_platform(long a, unsigned long b, char c, size_t d, ptrdiff_t e);
CINTEROP_SCALAR_API CInteropPlatformBool c_scalar_platform_bool(CInteropPlatformBool value);
CInteropPlatformBool joyeer_scalar_platform_bool(CInteropPlatformBool value);

int64_t joyeer_scalar_minimum(void);
uint64_t joyeer_scalar_maximum(void);
float joyeer_scalar_negative_zero(void);
float joyeer_scalar_float_literal(void);
double joyeer_scalar_double_literal(void);
float joyeer_scalar_negate_float(float value);
double joyeer_scalar_divide_double(double a, double b);
bool joyeer_scalar_is_nan(double value);
uint64_t joyeer_scalar_choose_unsigned(bool flag);
int cinterop_check_scalars(void);

#endif
