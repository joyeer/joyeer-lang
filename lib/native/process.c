#include "joyeer/native/process.h"
#include "host_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void* processAllocate(size_t bytes) {
    void* result = malloc(bytes ? bytes : 1);
    if (!result) joyeer_panic("out of memory");
    return result;
}

static int64_t invalidInputCode(void) {
#ifdef _WIN32
    return ERROR_INVALID_PARAMETER;
#else
    return EINVAL;
#endif
}

static int32_t processError(int64_t code) {
#ifdef _WIN32
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_BAD_PATHNAME || code == ERROR_INVALID_DRIVE ||
        code == ERROR_DIRECTORY) {
        return JOYEER_PROCESS_ERROR_NOT_FOUND;
    }
    if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD ||
        code == ERROR_SHARING_VIOLATION) {
        return JOYEER_PROCESS_ERROR_PERMISSION_DENIED;
    }
#else
    if (code == ENOENT || code == ENOTDIR) return JOYEER_PROCESS_ERROR_NOT_FOUND;
    if (code == EACCES || code == EPERM) return JOYEER_PROCESS_ERROR_PERMISSION_DENIED;
#endif
    return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
}

#ifdef _WIN32
static wchar_t* absolutePath(const wchar_t* path, int64_t* errorCode) {
    DWORD length = GetFullPathNameW(path, 0, NULL, NULL);
    if (!length) {
        *errorCode = GetLastError();
        return NULL;
    }
    wchar_t* result = processAllocate((size_t)length * sizeof(wchar_t));
    DWORD actual = GetFullPathNameW(path, length, result, NULL);
    if (!actual || actual >= length) {
        *errorCode = actual ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        free(result);
        return NULL;
    }
    return result;
}

static bool isBatchFile(const wchar_t* path) {
    const wchar_t* suffix = wcsrchr(path, L'.');
    return suffix && (_wcsicmp(suffix, L".bat") == 0 ||
                      _wcsicmp(suffix, L".cmd") == 0);
}

static size_t quotedLength(const wchar_t* value) {
    size_t length = 2;
    size_t slashes = 0;
    for (const wchar_t* cursor = value; *cursor; ++cursor) {
        if (*cursor == L'\\') {
            ++slashes;
        } else {
            length += slashes * (*cursor == L'"' ? 2 : 1) +
                      (*cursor == L'"' ? 2 : 1);
            slashes = 0;
        }
    }
    return length + slashes * 2;
}

static wchar_t* appendQuoted(wchar_t* output, const wchar_t* value) {
    *output++ = L'"';
    size_t slashes = 0;
    for (const wchar_t* cursor = value; *cursor; ++cursor) {
        if (*cursor == L'\\') {
            ++slashes;
            continue;
        }
        size_t escapes = slashes * (*cursor == L'"' ? 2 : 1);
        while (escapes--) *output++ = L'\\';
        slashes = 0;
        if (*cursor == L'"') *output++ = L'\\';
        *output++ = *cursor;
    }
    while (slashes--) {
        *output++ = L'\\';
        *output++ = L'\\';
    }
    *output++ = L'"';
    return output;
}

static int32_t runPlatform(
        int32_t* statusKind, int64_t* statusCode, int64_t* errorCode,
        JoyeerHostChar* executable, JoyeerHostChar** args,
        int64_t argumentCount, JoyeerHostChar* directory) {
    wchar_t* fullExecutable = absolutePath(executable, errorCode);
    if (!fullExecutable) return processError(*errorCode);
    wchar_t* fullDirectory = absolutePath(directory, errorCode);
    if (!fullDirectory) {
        free(fullExecutable);
        return processError(*errorCode);
    }
    if (isBatchFile(fullExecutable)) {
        free(fullExecutable);
        free(fullDirectory);
        *errorCode = ERROR_BAD_EXE_FORMAT;
        return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
    }
    size_t length = quotedLength(fullExecutable);
    for (int64_t i = 0; i < argumentCount; ++i) {
        size_t next = quotedLength(args[i]);
        if (length > SIZE_MAX - 2 || next > SIZE_MAX - length - 2) {
            joyeer_panic("process command line overflow");
        }
        length += next + 1;
    }
    if (length >= SIZE_MAX / sizeof(wchar_t)) joyeer_panic("process command line overflow");
    wchar_t* command = processAllocate((length + 1) * sizeof(wchar_t));
    wchar_t* end = appendQuoted(command, fullExecutable);
    for (int64_t i = 0; i < argumentCount; ++i) {
        *end++ = L' ';
        end = appendQuoted(end, args[i]);
    }
    *end = L'\0';
    STARTUPINFOW startup = { 0 };
    startup.cb = sizeof(startup);
    HANDLE standard[3] = {
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE)
    };
    HANDLE inherited[3] = { NULL, NULL, NULL };
    for (int i = 0; i < 3; ++i) {
        if (!standard[i] || standard[i] == INVALID_HANDLE_VALUE) continue;
        if (!DuplicateHandle(GetCurrentProcess(), standard[i],
                             GetCurrentProcess(), &inherited[i],
                             0, TRUE, DUPLICATE_SAME_ACCESS)) {
            *errorCode = GetLastError();
            for (int j = 0; j < i; ++j) {
                if (inherited[j]) CloseHandle(inherited[j]);
            }
            free(command);
            free(fullDirectory);
            free(fullExecutable);
            return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
        }
    }
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inherited[0];
    startup.hStdOutput = inherited[1];
    startup.hStdError = inherited[2];
    PROCESS_INFORMATION child = { 0 };
    BOOL launched = CreateProcessW(fullExecutable, command, NULL, NULL, TRUE, 0,
                                   NULL, fullDirectory, &startup, &child);
    int64_t launchError = launched ? 0 : GetLastError();
    for (int i = 0; i < 3; ++i) {
        if (inherited[i]) CloseHandle(inherited[i]);
    }
    free(command);
    free(fullDirectory);
    free(fullExecutable);
    if (!launched) {
        *errorCode = launchError;
        return processError(launchError);
    }
    CloseHandle(child.hThread);
    DWORD waitResult;
    do {
        waitResult = WaitForSingleObject(child.hProcess, INFINITE);
    } while (waitResult == WAIT_FAILED && GetLastError() == ERROR_IO_PENDING);
    if (waitResult != WAIT_OBJECT_0) {
        *errorCode = GetLastError();
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, INFINITE);
        CloseHandle(child.hProcess);
        return JOYEER_PROCESS_ERROR_WAIT_FAILED;
    }
    DWORD exitCode;
    if (!GetExitCodeProcess(child.hProcess, &exitCode)) {
        *errorCode = GetLastError();
        CloseHandle(child.hProcess);
        return JOYEER_PROCESS_ERROR_WAIT_FAILED;
    }
    CloseHandle(child.hProcess);
    *statusKind = JOYEER_PROCESS_STATUS_EXITED;
    *statusCode = (int64_t)exitCode;
    return JOYEER_PROCESS_ERROR_NONE;
}
#else
static char* absolutePath(const char* path, int64_t* errorCode) {
    if (path[0] == '/') {
        size_t bytes = strlen(path) + 1;
        char* result = processAllocate(bytes);
        memcpy(result, path, bytes);
        return result;
    }
    size_t capacity = 256;
    char* cwd = processAllocate(capacity);
    while (!getcwd(cwd, capacity)) {
        if (errno != ERANGE) {
            *errorCode = errno;
            free(cwd);
            return NULL;
        }
        if (capacity > SIZE_MAX / 2) joyeer_panic("process path overflow");
        capacity *= 2;
        char* bigger = realloc(cwd, capacity);
        if (!bigger) joyeer_panic("out of memory");
        cwd = bigger;
    }
    size_t base = strlen(cwd);
    size_t tail = strlen(path);
    if (tail > SIZE_MAX - base - 2) joyeer_panic("process path overflow");
    char* result = processAllocate(base + tail + 2);
    memcpy(result, cwd, base);
    result[base] = '/';
    memcpy(result + base + 1, path, tail + 1);
    free(cwd);
    return result;
}

typedef struct ChildFailure {
    int32_t operation;
    int32_t code;
} ChildFailure;

static void childFailure(int fd, int32_t operation) {
    ChildFailure failure = { operation, errno };
    const char* bytes = (const char*)&failure;
    size_t remaining = sizeof(failure);
    while (remaining) {
        ssize_t written = write(fd, bytes, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        bytes += written;
        remaining -= (size_t)written;
    }
    _exit(127);
}

static int waitChild(pid_t child, int* status, int64_t* errorCode) {
    pid_t waited;
    do {
        waited = waitpid(child, status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        *errorCode = errno;
        return -1;
    }
    return 0;
}

static int32_t runPlatform(
        int32_t* statusKind, int64_t* statusCode, int64_t* errorCode,
        JoyeerHostChar* executable, JoyeerHostChar** args,
        int64_t argumentCount, JoyeerHostChar* directory) {
    char* fullExecutable = absolutePath(executable, errorCode);
    if (!fullExecutable) return processError(*errorCode);
    char** argv = processAllocate(((size_t)argumentCount + 2) * sizeof(char*));
    argv[0] = fullExecutable;
    for (int64_t i = 0; i < argumentCount; ++i) argv[i + 1] = args[i];
    argv[argumentCount + 1] = NULL;
    int pipeEnds[2];
    if (pipe(pipeEnds) != 0) {
        *errorCode = errno;
        free(argv);
        free(fullExecutable);
        return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
    }
    if (fcntl(pipeEnds[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipeEnds[1], F_SETFD, FD_CLOEXEC) < 0) {
        *errorCode = errno;
        close(pipeEnds[0]);
        close(pipeEnds[1]);
        free(argv);
        free(fullExecutable);
        return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
    }
    pid_t child = fork();
    if (child == 0) {
        close(pipeEnds[0]);
        if (chdir(directory) != 0) childFailure(pipeEnds[1], 1);
        execv(fullExecutable, argv);
        childFailure(pipeEnds[1], 2);
    }
    int64_t forkError = child < 0 ? errno : 0;
    close(pipeEnds[1]);
    free(argv);
    free(fullExecutable);
    if (child < 0) {
        close(pipeEnds[0]);
        *errorCode = forkError;
        return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
    }
    ChildFailure failure = { 0 };
    size_t received = 0;
    while (received < sizeof(failure)) {
        ssize_t bytes = read(pipeEnds[0], (char*)&failure + received,
                             sizeof(failure) - received);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) {
            *errorCode = errno;
            break;
        }
        if (bytes == 0) break;
        received += (size_t)bytes;
    }
    close(pipeEnds[0]);
    int childStatus = 0;
    int64_t waitError = 0;
    if (waitChild(child, &childStatus, &waitError) != 0) {
        *errorCode = waitError;
        return JOYEER_PROCESS_ERROR_WAIT_FAILED;
    }
    if (*errorCode) return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
    if (received) {
        if (received != sizeof(failure)) {
            *errorCode = EIO;
            return JOYEER_PROCESS_ERROR_LAUNCH_FAILED;
        }
        *errorCode = failure.code;
        return processError(failure.code);
    }
    if (WIFEXITED(childStatus)) {
        *statusKind = JOYEER_PROCESS_STATUS_EXITED;
        *statusCode = WEXITSTATUS(childStatus);
    } else if (WIFSIGNALED(childStatus)) {
        *statusKind = JOYEER_PROCESS_STATUS_SIGNALED;
        *statusCode = WTERMSIG(childStatus);
    } else {
        *errorCode = ECHILD;
        return JOYEER_PROCESS_ERROR_WAIT_FAILED;
    }
    return JOYEER_PROCESS_ERROR_NONE;
}
#endif

int32_t joyeer_run_process_abi(
        int32_t* statusKind, int64_t* statusCode, int64_t* errorCode,
        const uint8_t* executableData, int64_t executableCount,
        const JoyeerString* arguments, int64_t argumentCount,
        const uint8_t* workingDirectoryData, int64_t workingDirectoryCount) {
    if (!statusKind || !statusCode || !errorCode) {
        if (errorCode) *errorCode = invalidInputCode();
        return JOYEER_PROCESS_ERROR_INVALID_INPUT;
    }
    *statusKind = JOYEER_PROCESS_STATUS_EXITED;
    *statusCode = 0;
    *errorCode = 0;
    if (executableCount <= 0 || workingDirectoryCount <= 0 ||
        argumentCount < 0 || (argumentCount && !arguments) ||
        !executableData || !workingDirectoryData ||
        (uint64_t)argumentCount > SIZE_MAX / sizeof(JoyeerHostChar*) - 2) {
        *errorCode = invalidInputCode();
        return JOYEER_PROCESS_ERROR_INVALID_INPUT;
    }
    for (int64_t i = 0; i < argumentCount; ++i) {
        if (arguments[i].count < 0 ||
            (arguments[i].count && !arguments[i].data)) {
            *errorCode = invalidInputCode();
            return JOYEER_PROCESS_ERROR_INVALID_INPUT;
        }
    }
    int64_t conversionError = 0;
    JoyeerHostChar* executable = joyeer_host_string(
            executableData, executableCount, false, &conversionError);
    if (!executable) {
        *errorCode = conversionError;
        return JOYEER_PROCESS_ERROR_INVALID_INPUT;
    }
    JoyeerHostChar* directory = joyeer_host_string(
            workingDirectoryData, workingDirectoryCount, false, &conversionError);
    if (!directory) {
        free(executable);
        *errorCode = conversionError;
        return JOYEER_PROCESS_ERROR_INVALID_INPUT;
    }
    JoyeerHostChar** args = processAllocate(((size_t)argumentCount + 1) * sizeof(*args));
    int64_t converted = 0;
    for (; converted < argumentCount; ++converted) {
        args[converted] = joyeer_host_string(
                arguments[converted].data, arguments[converted].count,
                true, &conversionError);
        if (!args[converted]) break;
    }
    int32_t result;
    if (converted != argumentCount) {
        *errorCode = conversionError;
        result = JOYEER_PROCESS_ERROR_INVALID_INPUT;
    } else {
        result = runPlatform(statusKind, statusCode, errorCode,
                             executable, args, argumentCount, directory);
    }
    while (converted) free(args[--converted]);
    free(args);
    free(directory);
    free(executable);
    return result;
}
