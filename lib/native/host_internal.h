#ifndef JOYEER_NATIVE_HOST_INTERNAL_H
#define JOYEER_NATIVE_HOST_INTERNAL_H

#include "joyeer/native/runtime.h"

#if defined(_WIN32)
typedef wchar_t JoyeerHostChar;
#else
typedef char JoyeerHostChar;
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool joyeer_host_valid_utf8(const uint8_t* data, int64_t count);

// The caller frees the converted string with free(). Invalid host input
// returns null and its platform error code; allocation failure is a panic.
JoyeerHostChar* joyeer_host_string(
        const uint8_t* data, int64_t count, bool allowEmpty, int64_t* errorCode);

#ifdef __cplusplus
}
#endif

#endif
