#ifndef JOYEER_NATIVE_HOST_INTERNAL_H
#define JOYEER_NATIVE_HOST_INTERNAL_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

bool joyeer_host_valid_utf8(const uint8_t* data, int64_t count);

#ifdef __cplusplus
}
#endif

#endif
