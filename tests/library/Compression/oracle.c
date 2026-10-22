#include "library/Compression/native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

static void emit(int64_t status, JoyeerArray* output, bool listing) {
    printf("%lld\n", (long long)status);
    if (status == 0) {
        if (listing) {
            const JoyeerString* names = (const JoyeerString*)output->data;
            for (int64_t index = 0; index < output->count; ++index) {
                fwrite(names[index].data, 1, (size_t)names[index].count, stdout);
                fputc('\n', stdout);
            }
        } else {
            fwrite(output->data, 1, (size_t)output->count, stdout);
        }
    }
    joyeer_array_destroy_abi(output);
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) return 80;
#endif
    if (argc < 3) return 81;
    JoyeerArray output = {0};
    if (strcmp(argv[1], "create") == 0) {
        uint8_t binary[4096];
        for (size_t index = 0; index < sizeof(binary); ++index) binary[index] = (uint8_t)index;
        const char* labels[] = {"binary.bin", "empty", "nested/\xE9\x9B\xAA.txt"};
        JoyeerString names[3];
        for (int index = 0; index < 3; ++index) {
            names[index].data = (const uint8_t*)labels[index];
            names[index].count = (int64_t)strlen(labels[index]);
        }
        const JoyeerArray contents[] = {
            {binary, sizeof(binary), sizeof(binary)}, {NULL, 0, 0}, {"payload", 7, 7}};
        joyeer_native_bytes_init(&output);
        const int64_t status = joyeer_compression_create_zip(
                names, 3, contents, 3, strtoll(argv[2], NULL, 10), 1048576, &output);
        emit(status, &output, false);
    } else {
        if (argc != 4) return 82;
        uint8_t* input = (uint8_t*)malloc(2097152);
        if (input == NULL) return 83;
        const size_t count = fread(input, 1, 2097152, stdin);
        if (ferror(stdin) || !feof(stdin)) { free(input); return 84; }
        const int64_t first = strtoll(argv[2], NULL, 10);
        const int64_t second = strtoll(argv[3], NULL, 10);
        if (strcmp(argv[1], "list") == 0) {
            joyeer_native_strings_init(&output);
            emit(joyeer_compression_list_zip(input, (int64_t)count, first, second, &output), &output, true);
        } else if (strcmp(argv[1], "extract") == 0) {
            joyeer_native_bytes_init(&output);
            emit(joyeer_compression_extract_zip(input, (int64_t)count, first, second, &output), &output, false);
        } else {
            free(input);
            return 85;
        }
        free(input);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) return 86;
    return joyeer_runtime_active_allocations() == 0 ? 0 : 87;
}
