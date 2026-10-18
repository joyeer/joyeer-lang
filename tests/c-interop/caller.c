#include <stdint.h>
#include <stdio.h>

int64_t joyeer_fixture_identity(int64_t value);
int64_t joyeer_fixture_second(int64_t value);
int64_t joyeer_fixture_zero(void);
void joyeer_fixture_store(int64_t value);
int64_t joyeer_fixture_load(void);
int64_t joyeer_fixture_arguments(
        int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f,
        int64_t g, int64_t h, int64_t i, int64_t j, int64_t k, int64_t l);

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "C interop check failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    const int64_t values[] = {0, 1, -1, INT64_MIN, INT64_MAX};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        const int64_t value = values[index];
        CHECK(joyeer_fixture_identity(value) == value);
        CHECK(joyeer_fixture_second(value) == value);
        joyeer_fixture_store(value);
        CHECK(joyeer_fixture_load() == value);
    }
    CHECK(joyeer_fixture_zero() == -17);
    CHECK(joyeer_fixture_arguments(-1, 2, -3, 4, -5, 6, -7, 8, -9, 10, -11, 12) == 1);
    return 0;
}
