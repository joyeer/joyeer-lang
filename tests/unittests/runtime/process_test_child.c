#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#endif

static void writeText(FILE* file, const char* text) {
    uint32_t size = (uint32_t)strlen(text);
    unsigned char length[4] = {
        (unsigned char)size, (unsigned char)(size >> 8),
        (unsigned char)(size >> 16), (unsigned char)(size >> 24)
    };
    if (fwrite(length, 1, 4, file) != 4 ||
        fwrite(text, 1, size, file) != size) exit(120);
}

#ifdef _WIN32
static char* utf8(const wchar_t* value) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                   NULL, 0, NULL, NULL);
    if (!size) exit(121);
    char* text = malloc((size_t)size);
    if (!text) exit(121);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                             text, size, NULL, NULL)) exit(121);
    return text;
}

static int hasStream(DWORD which) {
    HANDLE handle = GetStdHandle(which);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) return 0;
    SetLastError(NO_ERROR);
    DWORD type = GetFileType(handle);
    return type != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 122;
    if (wcscmp(argv[1], L"exit7") == 0) return 7;
    if (wcscmp(argv[1], L"exitWide") == 0) return (int)0xF1234567U;
    if (wcscmp(argv[1], L"streams") == 0) {
        if (fputs("joyeer-child-stdout", stdout) == EOF ||
            fputs("joyeer-child-stderr", stderr) == EOF ||
            fflush(stdout) != 0 || fflush(stderr) != 0) return 125;
        return 0;
    }
    if (argc < 3) return 122;
    if (wcscmp(argv[1], L"exit") == 0) return (int)wcstoul(argv[2], NULL, 10);
    if (wcscmp(argv[1], L"signal") == 0) return 122;
    FILE* file = _wfopen(argv[2], L"wb");
    if (!file) return 123;
    DWORD cwdSize = GetCurrentDirectoryW(0, NULL);
    wchar_t* cwd = malloc((size_t)cwdSize * sizeof(wchar_t));
    if (!cwd || !GetCurrentDirectoryW(cwdSize, cwd)) return 124;
    char* cwdUtf8 = utf8(cwd);
    writeText(file, cwdUtf8);
    free(cwdUtf8);
    free(cwd);
    const wchar_t* env = _wgetenv(L"JOYEER_PROCESS_TEST_ENV");
    char* envUtf8 = utf8(env ? env : L"");
    writeText(file, envUtf8);
    free(envUtf8);
    char streams[] = {
        hasStream(STD_INPUT_HANDLE) ? '1' : '0',
        hasStream(STD_OUTPUT_HANDLE) ? '1' : '0',
        hasStream(STD_ERROR_HANDLE) ? '1' : '0', 0
    };
    writeText(file, streams);
    for (int i = 3; i < argc; ++i) {
        char* text = utf8(argv[i]);
        writeText(file, text);
        free(text);
    }
    return fclose(file) == 0 ? 0 : 125;
}
#else
int main(int argc, char** argv) {
    if (argc < 2) return 122;
    if (strcmp(argv[1], "exit7") == 0) return 7;
    if (strcmp(argv[1], "exitWide") == 0) return 73;
    if (strcmp(argv[1], "streams") == 0) {
        if (fputs("joyeer-child-stdout", stdout) == EOF ||
            fputs("joyeer-child-stderr", stderr) == EOF ||
            fflush(stdout) != 0 || fflush(stderr) != 0) return 125;
        return 0;
    }
    if (argc < 3) return 122;
    if (strcmp(argv[1], "exit") == 0) return atoi(argv[2]);
    if (strcmp(argv[1], "signal") == 0) {
        raise(SIGTERM);
        return 126;
    }
    FILE* file = fopen(argv[2], "wb");
    if (!file) return 123;
    size_t capacity = 256;
    char* cwd = malloc(capacity);
    if (!cwd) return 124;
    while (!getcwd(cwd, capacity)) {
        if (errno != ERANGE || capacity > SIZE_MAX / 2) return 124;
        capacity *= 2;
        char* bigger = realloc(cwd, capacity);
        if (!bigger) return 124;
        cwd = bigger;
    }
    writeText(file, cwd);
    free(cwd);
    const char* env = getenv("JOYEER_PROCESS_TEST_ENV");
    writeText(file, env ? env : "");
    char streams[] = {
        fcntl(0, F_GETFD) != -1 ? '1' : '0',
        fcntl(1, F_GETFD) != -1 ? '1' : '0',
        fcntl(2, F_GETFD) != -1 ? '1' : '0', 0
    };
    writeText(file, streams);
    for (int i = 3; i < argc; ++i) writeText(file, argv[i]);
    return fclose(file) == 0 ? 0 : 125;
}
#endif
