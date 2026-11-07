#include "host_internal.h"

bool joyeer_host_valid_utf8(const uint8_t* data, int64_t count) {
    if (count < 0 || (count != 0 && data == NULL)) return false;
    int64_t index = 0;
    while (index < count) {
        const uint8_t first = data[index++];
        if (first <= 0x7f) continue;
        int remaining;
        uint8_t lower = 0x80;
        uint8_t upper = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            if (first == 0xe0) lower = 0xa0;
            if (first == 0xed) upper = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            if (first == 0xf0) lower = 0x90;
            if (first == 0xf4) upper = 0x8f;
        } else {
            return false;
        }
        if (count - index < remaining) return false;
        if (data[index] < lower || data[index] > upper) return false;
        ++index;
        for (int continuation = 1; continuation < remaining; ++continuation) {
            if (data[index] < 0x80 || data[index] > 0xbf) return false;
            ++index;
        }
    }
    return true;
}
