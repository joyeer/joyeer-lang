#include "joyeer/runtime.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    uint8_t* bytes = malloc(1);
    if (bytes == NULL) return 3;
    bytes[0] = 'a';
    const int64_t count = strcmp(argv[1], "valid") == 0 ? 1 : 2;
    const bool valid = joyeer_host_valid_utf8(bytes, count);
    free(bytes);
    return valid ? 0 : 4;
}
