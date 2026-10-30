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

static uint64_t getLE64(const uint8_t* source) {
    uint64_t value = 0;
    for (int byte = 7; byte >= 0; --byte) value = (value << 8) | source[byte];
    return value;
}

// Prints "name<TAB>directory<TAB>size<TAB>compressed<TAB>crc<TAB>supported" per entry.
static void emitEntries(int64_t status, const JoyeerArray* names, const JoyeerArray* table) {
    printf("%lld\n", (long long)status);
    if (status != 0) return;
    const JoyeerString* labels = (const JoyeerString*)names->data;
    const uint8_t* records = (const uint8_t*)table->data;
    for (int64_t index = 0; index < names->count; ++index) {
        const uint8_t* record = records + index * 56;
        fwrite(labels[index].data, 1, (size_t)labels[index].count, stdout);
        const uint64_t attributes = getLE64(record + 48);
        printf("\t%d\t%llu\t%llu\t%llu\t%d\n", (int)(attributes & 1),
                (unsigned long long)getLE64(record + 16), (unsigned long long)getLE64(record + 8),
                (unsigned long long)getLE64(record + 24), (int)((attributes >> 1) & 1));
    }
}

// Mirrors readZipEntry: look the entry up once, then read it from its local header.
static int64_t readByIndex(const uint8_t* input, int64_t count, int64_t index,
        int64_t maximumBytes, JoyeerArray* output) {
    JoyeerArray names = {0};
    JoyeerArray table = {0};
    joyeer_native_strings_init(&names);
    joyeer_native_bytes_init(&table);
    int64_t status = joyeer_compression_entries_zip(input, count, 1000, 100000, &names, &table);
    if (status == 0) {
        if (index < 0 || index >= names.count) {
            status = 5;
        } else {
            const uint8_t* record = (const uint8_t*)table.data + index * 56;
            const uint64_t attributes = getLE64(record + 48);
            if ((attributes & 1) != 0 || (attributes & 2) == 0) {
                status = 3;
            } else {
                status = joyeer_compression_read_entry(input, count,
                        (int64_t)getLE64(record), (int64_t)getLE64(record + 32),
                        (int64_t)getLE64(record + 40), (int64_t)getLE64(record + 8),
                        (int64_t)getLE64(record + 16), (int64_t)getLE64(record + 24),
                        maximumBytes, output);
            }
        }
    }
    joyeer_array_destroy_abi(&names);
    joyeer_array_destroy_abi(&table);
    return status;
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
        } else if (strcmp(argv[1], "entries") == 0) {
            JoyeerArray names = {0};
            JoyeerArray table = {0};
            joyeer_native_strings_init(&names);
            joyeer_native_bytes_init(&table);
            emitEntries(joyeer_compression_entries_zip(
                    input, (int64_t)count, first, second, &names, &table), &names, &table);
            joyeer_array_destroy_abi(&names);
            joyeer_array_destroy_abi(&table);
        } else if (strcmp(argv[1], "read") == 0) {
            joyeer_native_bytes_init(&output);
            emit(readByIndex(input, (int64_t)count, first, second, &output), &output, false);
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
