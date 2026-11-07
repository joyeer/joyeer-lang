#include <stdint.h>

// Two callbacks with the shapes of the runtime's generated element functions:
// destroy takes one address, clone takes a destination and a source. Joyeer
// receives their addresses as Int and calls them through the indirect-call
// intrinsics (spec 19.4.5).
static int64_t destroyed = 0;

static void destroy_one(void* value) {
    *(int64_t*)value = -1;
    ++destroyed;
}

static void clone_one(void* destination, const void* source) {
    *(int64_t*)destination = *(const int64_t*)source * 2;
}

int64_t cinterop_destroy_address(void) { return (int64_t)(intptr_t)&destroy_one; }
int64_t cinterop_clone_address(void) { return (int64_t)(intptr_t)&clone_one; }
int64_t cinterop_destroy_count(void) { return destroyed; }
