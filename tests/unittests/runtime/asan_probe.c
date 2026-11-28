#include "joyeer/runtime.h"

#include <stdlib.h>
#include <string.h>

static int checkRead(int64_t count) {
    uint8_t* bytes = malloc(1);
    if (bytes == NULL) return 3;
    bytes[0] = 'a';
    const bool valid = joyeer_host_valid_utf8(bytes, count);
    free(bytes);
    return valid ? 0 : 4;
}

static int checkWrite(size_t capacity) {
    char* buffer = malloc(capacity);
    if (buffer == NULL) return 3;
    const int64_t count = joyeer_format_double_abi(buffer, 1, -1.0);
    const bool valid = count == 2 && capacity >= 2 && memcmp(buffer, "-1", 2) == 0;
    free(buffer);
    return valid ? 0 : 4;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (strcmp(argv[1], "invalid") == 0) return checkRead(2);
    if (strcmp(argv[1], "invalid-write") == 0) return checkWrite(1);
    if (strcmp(argv[1], "valid") != 0) return 2;
    const int status = checkRead(1);
    return status == 0 ? checkWrite(40) : status;
}
