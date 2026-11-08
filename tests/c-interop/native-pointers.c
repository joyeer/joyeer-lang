#include <stdint.h>

// Uppercases a NUL-terminated C string from a bare byte pointer into a mutable
// byte buffer. Returns the length, or -1 when the buffer is too small.
int64_t cinterop_pointer_upper(const uint8_t* text, uint8_t* output, int64_t capacity) {
    int64_t length = 0;
    while (text[length] != 0) {
        if (length >= capacity) return -1;
        const uint8_t value = text[length];
        output[length] = value >= 'a' && value <= 'z' ? (uint8_t)(value - 32) : value;
        ++length;
    }
    return length;
}

// Fills a mutable byte buffer, as a native call fills a structure.
int64_t cinterop_pointer_fill(uint8_t* buffer, int64_t count) {
    for (int64_t index = 0; index < count; ++index) buffer[index] = (uint8_t)(index * 3 + 1);
    return count;
}

// A bare pointer may sit between scalars; the count is an ordinary argument.
int64_t cinterop_pointer_sum(int64_t base, const uint8_t* bytes, int64_t count, int64_t extra) {
    int64_t total = base + extra;
    for (int64_t index = 0; index < count; ++index) total += bytes[index];
    return total;
}
