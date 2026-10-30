#include "runtime/native.h"

int64_t cinterop_buffers(const uint8_t* input, int64_t count,
        JoyeerArray* bytes, JoyeerString* text, JoyeerArray* names) {
    if (count != 3 || input[0] != 0 || input[1] != 255 || input[2] != 65) return 9;
    joyeer_native_bytes_assign(bytes, input, count);
    joyeer_native_string_assign(text, input, count);
    const JoyeerString values[] = {{input, count}, {NULL, 0}};
    joyeer_native_strings_assign(names, values, 2);
    return 0;
}

int64_t cinterop_buffers_error(JoyeerArray* bytes, JoyeerString* text, JoyeerArray* names) {
    (void)bytes;
    (void)text;
    (void)names;
    return 7;
}
