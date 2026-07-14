#include "joyeer/native/runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

void joyeer_main(void);

int main(void) {
    joyeer_main();
    const int64_t allocations = joyeer_runtime_active_allocations();
    if (allocations != 0) {
        fprintf(stderr, "Joyeer runtime error: %" PRId64 " leaked allocation(s)\n", allocations);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
