#include "runtime.h"
#include "native.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

void joyeer_print_float(float value) {
    printf("%.9g\n", (double)value);
}

void joyeer_print_double(double value) {
    printf("%.17g\n", value);
}

