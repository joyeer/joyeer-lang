#ifndef JOYEER_NATIVE_H
#define JOYEER_NATIVE_H

#include "runtime.h"

#define JOYEER_NATIVE_BUFFER_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// Borrowed String/[UInt8] are (const uint8_t*, int64_t). Borrowed
// [String]/[[UInt8]] are (const JoyeerString*/const JoyeerArray*, int64_t).
// Neither descriptors nor their storage may be modified, retained or freed.
//
// Initializing outputs are pointers to caller-owned, already empty handles.
// Assign copies through the runtime allocator. Never install malloc/miniz
// storage in these handles. An error must leave a valid, destroyable value.
void joyeer_native_bytes_init(JoyeerArray* result);
void joyeer_native_strings_init(JoyeerArray* result);
void joyeer_native_bytes_assign(JoyeerArray* result, const uint8_t* data, int64_t count);
void joyeer_native_string_assign(JoyeerString* result, const uint8_t* data, int64_t count);
void joyeer_native_strings_assign(JoyeerArray* result, const JoyeerString* data, int64_t count);

#ifdef __cplusplus
}
#endif

#endif
