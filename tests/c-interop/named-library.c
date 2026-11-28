#include <stdint.h>

#ifndef JOYEER_NAMED_VALUE
#define JOYEER_NAMED_VALUE 42
#endif

int32_t joyeer_named_library_value(void) {
    return JOYEER_NAMED_VALUE;
}
