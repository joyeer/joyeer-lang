#include <stdio.h>

#if defined(_MSC_VER)
__declspec(noreturn) void cinterop_finish(int status);
#else
_Noreturn void cinterop_finish(int status);
#endif

// The export must not return: if it did, this program would exit with status 1.
int main(void) {
    puts("never export before");
    fflush(stdout);
    cinterop_finish(0);
    return 1;
}
