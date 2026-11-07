#include <stdint.h>
#include <stdio.h>

int64_t cinterop_slot_increment(int64_t index, int64_t delta);
uint64_t cinterop_slot_read(int64_t index);

// Two Joyeer objects, compiled separately, must see one table of slots.
int main(void) {
    if (cinterop_slot_read(3) != 0) return 1;
    if (cinterop_slot_increment(3, 5) != 0) return 2;
    if (cinterop_slot_increment(3, 2) != 5) return 3;
    if (cinterop_slot_read(3) != 7) return 4;
    if (cinterop_slot_read(4) != 0) return 5;
    puts("shared static slots passed");
    return 0;
}
