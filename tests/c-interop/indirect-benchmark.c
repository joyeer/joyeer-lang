// Not a test. Compares destroying an array of elements through a function
// pointer in a C loop with the same loop written in Joyeer using the indirect
// call intrinsic. Build the CInteropIndirectBenchmark target in a Release tree.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void cinterop_destroy_all(int64_t function, int64_t base, int64_t count, int64_t stride);

static volatile int64_t sink;

static void destroy_element(void* value) { sink += *(int64_t*)value; }

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// Not inlined, and the callee comes from a volatile, so the call stays indirect.
static void (*volatile callee)(void*) = destroy_element;

NOINLINE static void c_destroy_all(void (*destroy)(void*), void* base, int64_t count, int64_t stride) {
    for (int64_t index = count; index > 0; --index) {
        destroy((uint8_t*)base + (index - 1) * stride);
    }
}

static double seconds(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(void) {
    const int64_t count = 1000;
    const int passes = 100000;
    int64_t* elements = calloc((size_t)count, sizeof(int64_t));
    if (elements == NULL) return 1;
    double best_c = 1e300;
    double best_joyeer = 1e300;
    for (int repetition = 0; repetition < 7; ++repetition) {
        double start = seconds();
        for (int pass = 0; pass < passes; ++pass) {
            c_destroy_all(callee, elements, count, sizeof(int64_t));
        }
        double elapsed = seconds() - start;
        if (elapsed < best_c) best_c = elapsed;

        start = seconds();
        for (int pass = 0; pass < passes; ++pass) {
            cinterop_destroy_all((int64_t)(intptr_t)callee, (int64_t)(intptr_t)elements,
                    count, sizeof(int64_t));
        }
        elapsed = seconds() - start;
        if (elapsed < best_joyeer) best_joyeer = elapsed;
    }
    const double operations = (double)count * (double)passes;
    printf("C loop, indirect call:      %6.2f ns per element\n", best_c / operations * 1e9);
    printf("Joyeer loop, intrinsic:     %6.2f ns per element\n", best_joyeer / operations * 1e9);
    free(elements);
    return 0;
}
