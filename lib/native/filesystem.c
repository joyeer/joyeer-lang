#include "joyeer/native/filesystem.h"
#include "host_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static void* fsAllocate(size_t bytes) {
    void* result = malloc(bytes == 0 ? 1 : bytes);
    if (result == NULL) joyeer_panic("out of memory");
    return result;
}

static void* fsReallocate(void* value, size_t bytes) {
    void* result = realloc(value, bytes);
    if (result == NULL) joyeer_panic("out of memory");
    return result;
}

static int32_t fsError(int64_t* errorCode, int64_t code) {
    *errorCode = code;
#ifdef _WIN32
    switch (code) {
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NO_UNICODE_TRANSLATION:
    case ERROR_FILENAME_EXCED_RANGE:
        return JOYEER_FS_ERROR_INVALID_PATH;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_NETPATH:
        return JOYEER_FS_ERROR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return JOYEER_FS_ERROR_PERMISSION_DENIED;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return JOYEER_FS_ERROR_ALREADY_EXISTS;
    case ERROR_DIRECTORY:
        return JOYEER_FS_ERROR_NOT_DIRECTORY;
    default:
        return JOYEER_FS_ERROR_OTHER;
    }
#else
    switch (code) {
    case EINVAL:
    case EILSEQ:
    case ENAMETOOLONG:
        return JOYEER_FS_ERROR_INVALID_PATH;
    case ENOENT:
        return JOYEER_FS_ERROR_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
        return JOYEER_FS_ERROR_PERMISSION_DENIED;
    case EEXIST:
        return JOYEER_FS_ERROR_ALREADY_EXISTS;
    case ENOTDIR:
        return JOYEER_FS_ERROR_NOT_DIRECTORY;
    case EISDIR:
        return JOYEER_FS_ERROR_IS_DIRECTORY;
    default:
        return JOYEER_FS_ERROR_OTHER;
    }
#endif
}

static void fsCheck(int64_t* errorCode) {
    if (errorCode == NULL) joyeer_panic("filesystem error output is null");
    *errorCode = 0;
}

static int32_t fsPath(
        const uint8_t* data, int64_t count,
        JoyeerHostChar** result, int64_t* errorCode) {
    *result = joyeer_host_string(data, count, false, errorCode);
    if (*result == NULL) return fsError(errorCode, *errorCode);
    return JOYEER_FS_ERROR_NONE;
}

static void fsCloneString(void* destination, const void* source) {
    const JoyeerString* value = (const JoyeerString*)source;
    joyeer_string_clone_abi((JoyeerString*)destination, value->data, value->count);
}

static void fsDestroyString(void* value) {
    joyeer_string_destroy_abi((JoyeerString*)value);
}

#ifdef _WIN32
static int64_t fsLastError(void) { return (int64_t)GetLastError(); }

static bool fsDriveLetter(uint32_t value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

static bool fsDirectoryAttributes(const wchar_t* path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool fsWideName(
        const wchar_t* name, JoyeerString* result, int64_t* errorCode) {
    int length = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, NULL, 0, NULL, NULL);
    if (length == 0) {
        *errorCode = fsLastError();
        return false;
    }
    char* bytes = (char*)fsAllocate((size_t)length);
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, bytes, length, NULL, NULL) == 0) {
        *errorCode = fsLastError();
        free(bytes);
        return false;
    }
    joyeer_string_clone_abi(result, (const uint8_t*)bytes, length - 1);
    free(bytes);
    return true;
}
#endif

static int32_t fsReadFileWithLimit(
        JoyeerString* result, int64_t* errorCode,
        const uint8_t* pathData, int64_t pathCount,
        int64_t maximumBytes, bool bounded) {
    if (result == NULL) joyeer_panic("filesystem result is null");
    *result = (JoyeerString) { 0 };
    fsCheck(errorCode);
    if (bounded && maximumBytes < 0) {
#ifdef _WIN32
        *errorCode = ERROR_INVALID_PARAMETER;
#else
        *errorCode = EINVAL;
#endif
        // This is an invalid limit, not an invalid path; keep the existing tags.
        return JOYEER_FS_ERROR_OTHER;
    }
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
#ifdef _WIN32
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int64_t code = file == INVALID_HANDLE_VALUE ? fsLastError() : 0;
    if (code == ERROR_ACCESS_DENIED && fsDirectoryAttributes(path)) {
        free(path);
        *errorCode = code;
        return JOYEER_FS_ERROR_IS_DIRECTORY;
    }
#else
    int file = open(path, O_RDONLY);
    int64_t code = file < 0 ? errno : 0;
#endif
    free(path);
    if (code != 0) return fsError(errorCode, code);
    size_t count = 0;
    size_t capacity = 0;
    JoyeerString buffer = { 0 };
    // Start with one tracked String allocation. Reallocating this same libc
    // block preserves its allocation balance and avoids a second prefix copy.
    if (bounded) joyeer_string_clone_abi(&buffer, NULL, 0);
    uint8_t* bytes = (uint8_t*)buffer.data;
    for (;;) {
        // Reaching the limit is success; never read a sentinel byte implicitly.
        if (bounded && (uint64_t)count == (uint64_t)maximumBytes) break;
        if (count == capacity) {
            if (capacity >= (size_t)INT64_MAX) {
#ifdef _WIN32
                code = ERROR_FILE_TOO_LARGE;
#else
                code = EFBIG;
#endif
                break;
            }
            size_t next = capacity < 4096 ? 4096 : capacity > (size_t)INT64_MAX / 2
                    ? (size_t)INT64_MAX : capacity * 2;
            if (bounded && (uint64_t)next > (uint64_t)maximumBytes)
                next = (size_t)maximumBytes;
            bytes = (uint8_t*)fsReallocate(bytes, next);
            capacity = next;
        }
#ifdef _WIN32
        DWORD got = 0;
        DWORD wanted = (DWORD)((capacity - count) > MAXDWORD
                ? MAXDWORD : (capacity - count));
        if (!ReadFile(file, bytes + count, wanted, &got, NULL)) {
            code = fsLastError();
            break;
        }
        if (got == 0) break;
        count += got;
#else
        size_t wanted = capacity - count;
        if (wanted > (size_t)SSIZE_MAX) wanted = (size_t)SSIZE_MAX;
        ssize_t got = read(file, bytes + count, wanted);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { code = errno; break; }
        if (got == 0) break;
        count += (size_t)got;
#endif
    }
#ifdef _WIN32
    if (!CloseHandle(file) && code == 0) code = fsLastError();
#else
    if (close(file) != 0 && code == 0) code = errno;
#endif
    if (bounded) {
        buffer.data = bytes;
        buffer.count = (int64_t)count;
        if (code == 0) *result = buffer;
        else joyeer_string_destroy_abi(&buffer);
    } else {
        // Preserve the existing whole-file reader's allocation/copy behavior.
        if (code == 0) joyeer_string_clone_abi(result, bytes, (int64_t)count);
        free(bytes);
    }
    return code == 0 ? JOYEER_FS_ERROR_NONE : fsError(errorCode, code);
}

int32_t joyeer_fs_read_file_abi(
        JoyeerString* result, int64_t* errorCode,
        const uint8_t* pathData, int64_t pathCount) {
    return fsReadFileWithLimit(result, errorCode, pathData, pathCount, 0, false);
}

int32_t joyeer_fs_read_file_prefix_abi(
        JoyeerString* result, int64_t* errorCode,
        const uint8_t* pathData, int64_t pathCount, int64_t maximumBytes) {
    return fsReadFileWithLimit(
            result, errorCode, pathData, pathCount, maximumBytes, true);
}

int32_t joyeer_fs_write_file_new_abi(
        int64_t* errorCode, const uint8_t* pathData, int64_t pathCount,
        const uint8_t* contentsData, int64_t contentsCount) {
    fsCheck(errorCode);
    if (contentsCount < 0 || (contentsCount != 0 && contentsData == NULL)) {
        joyeer_panic("invalid file contents");
    }
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
#ifdef _WIN32
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
    int64_t code = file == INVALID_HANDLE_VALUE ? fsLastError() : 0;
#else
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    int64_t code = file < 0 ? errno : 0;
#endif
    free(path);
    if (code != 0) return fsError(errorCode, code);
    int64_t offset = 0;
    while (offset < contentsCount) {
#ifdef _WIN32
        DWORD wanted = (uint64_t)(contentsCount - offset) > MAXDWORD
                ? MAXDWORD : (DWORD)(contentsCount - offset);
        DWORD written = 0;
        if (!WriteFile(file, contentsData + offset, wanted, &written, NULL)) {
            code = fsLastError();
            break;
        }
        if (written == 0) { code = ERROR_WRITE_FAULT; break; }
#else
        size_t wanted = (uint64_t)(contentsCount - offset) > (size_t)SSIZE_MAX
                ? (size_t)SSIZE_MAX : (size_t)(contentsCount - offset);
        ssize_t written = write(file, contentsData + offset, wanted);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) { code = errno; break; }
        if (written == 0) { code = EIO; break; }
#endif
        offset += written;
    }
#ifdef _WIN32
    if (!CloseHandle(file) && code == 0) code = fsLastError();
#else
    if (close(file) != 0 && code == 0) code = errno;
#endif
    return code == 0 ? JOYEER_FS_ERROR_NONE : fsError(errorCode, code);
}

int32_t joyeer_fs_create_directory_abi(
        int64_t* errorCode, const uint8_t* pathData, int64_t pathCount) {
    fsCheck(errorCode);
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
#ifdef _WIN32
    BOOL ok = CreateDirectoryW(path, NULL);
    int64_t code = ok ? 0 : fsLastError();
#else
    int ok = mkdir(path, 0777);
    int64_t code = ok == 0 ? 0 : errno;
#endif
    free(path);
    return code == 0 ? JOYEER_FS_ERROR_NONE : fsError(errorCode, code);
}

int32_t joyeer_fs_file_kind_abi(
        int32_t* result, int64_t* errorCode,
        const uint8_t* pathData, int64_t pathCount) {
    if (result == NULL) joyeer_panic("filesystem result is null");
    *result = JOYEER_FS_KIND_OTHER;
    fsCheck(errorCode);
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path);
    int64_t code = attributes == INVALID_FILE_ATTRIBUTES ? fsLastError() : 0;
    if (code == 0) {
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            *result = JOYEER_FS_KIND_SYMLINK;
        } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            *result = JOYEER_FS_KIND_DIRECTORY;
        } else {
            // DOS devices such as NUL can advertise ordinary file attributes.
            // Query a metadata-only handle without reading or following links.
            HANDLE file = CreateFileW(path, 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (file == INVALID_HANDLE_VALUE) {
                code = fsLastError();
            } else {
                SetLastError(ERROR_SUCCESS);
                DWORD type = GetFileType(file);
                if (type == FILE_TYPE_UNKNOWN) {
                    code = fsLastError();
                } else if (type == FILE_TYPE_DISK) {
                    BY_HANDLE_FILE_INFORMATION info;
                    if (!GetFileInformationByHandle(file, &info)) {
                        code = fsLastError();
                    } else if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        *result = JOYEER_FS_KIND_SYMLINK;
                    } else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        *result = JOYEER_FS_KIND_DIRECTORY;
                    } else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)) {
                        *result = JOYEER_FS_KIND_FILE;
                    }
                }
                if (!CloseHandle(file) && code == 0) code = fsLastError();
            }
        }
    }
#else
    struct stat info;
    int ok = lstat(path, &info);
    int64_t code = ok == 0 ? 0 : errno;
    if (code == 0) {
        *result = S_ISLNK(info.st_mode) ? JOYEER_FS_KIND_SYMLINK
                : S_ISDIR(info.st_mode) ? JOYEER_FS_KIND_DIRECTORY
                : S_ISREG(info.st_mode) ? JOYEER_FS_KIND_FILE : JOYEER_FS_KIND_OTHER;
    }
#endif
    free(path);
    return code == 0 ? JOYEER_FS_ERROR_NONE : fsError(errorCode, code);
}

int32_t joyeer_fs_remove_file_abi(
        int64_t* errorCode, const uint8_t* pathData, int64_t pathCount) {
    fsCheck(errorCode);
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path);
    int64_t code = attributes == INVALID_FILE_ATTRIBUTES ? fsLastError() : 0;
    if (code == 0) {
        bool realDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        BOOL ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) && !realDirectory
                ? RemoveDirectoryW(path) : DeleteFileW(path);
        if (!ok) code = fsLastError();
        if (code != 0 && realDirectory) kind = JOYEER_FS_ERROR_IS_DIRECTORY;
    }
#else
    int ok = unlink(path);
    int64_t code = ok == 0 ? 0 : errno;
    if (code == EPERM || code == EACCES) {
        struct stat info;
        if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
            kind = JOYEER_FS_ERROR_IS_DIRECTORY;
        }
    }
#endif
    free(path);
    if (code == 0) return JOYEER_FS_ERROR_NONE;
    int32_t mapped = fsError(errorCode, code);
#ifdef _WIN32
    return kind == JOYEER_FS_ERROR_IS_DIRECTORY ? kind : mapped;
#else
    return kind == JOYEER_FS_ERROR_IS_DIRECTORY ? kind : mapped;
#endif
}

int32_t joyeer_fs_remove_directory_abi(
        int64_t* errorCode, const uint8_t* pathData, int64_t pathCount) {
    fsCheck(errorCode);
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path);
    int64_t code = attributes == INVALID_FILE_ATTRIBUTES ? fsLastError() : 0;
    if (code == 0 && (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        code = ERROR_DIRECTORY;
    } else if (code == 0 && !RemoveDirectoryW(path)) {
        code = fsLastError();
    }
#else
    struct stat info;
    int ok = lstat(path, &info);
    int64_t code = ok == 0 ? 0 : errno;
    if (code == 0 && !S_ISDIR(info.st_mode)) code = ENOTDIR;
    else if (code == 0 && rmdir(path) != 0) code = errno;
#endif
    free(path);
    return code == 0 ? JOYEER_FS_ERROR_NONE : fsError(errorCode, code);
}

int32_t joyeer_fs_list_directory_abi(
        JoyeerArray* result, int64_t* errorCode,
        const uint8_t* pathData, int64_t pathCount) {
    if (result == NULL) joyeer_panic("filesystem result is null");
    *result = (JoyeerArray) { 0 };
    fsCheck(errorCode);
    JoyeerHostChar* path;
    int32_t kind = fsPath(pathData, pathCount, &path, errorCode);
    if (kind != JOYEER_FS_ERROR_NONE) return kind;
    int64_t code = 0;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) code = fsLastError();
    else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) code = ERROR_DIRECTORY;
    size_t length = wcslen(path);
    if (code == 0 && length > (SIZE_MAX / sizeof(wchar_t)) - 3)
        joyeer_panic("directory path overflow");
    wchar_t* pattern = NULL;
    if (code == 0) {
        pattern = (wchar_t*)fsAllocate((length + 3) * sizeof(wchar_t));
        memcpy(pattern, path, length * sizeof(wchar_t));
        const bool bareDrive = length == 2 && fsDriveLetter(path[0]) && path[1] == L':';
        if (!bareDrive && length != 0 &&
                path[length - 1] != L'/' && path[length - 1] != L'\\')
            pattern[length++] = L'\\';
        pattern[length++] = L'*';
        pattern[length] = 0;
    }
    WIN32_FIND_DATAW entry;
    HANDLE directory = code == 0 ? FindFirstFileW(pattern, &entry)
            : INVALID_HANDLE_VALUE;
    if (code == 0 && directory == INVALID_HANDLE_VALUE) {
        code = fsLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            DWORD remaining = GetFileAttributesW(path);
            if (remaining != INVALID_FILE_ATTRIBUTES &&
                    (remaining & FILE_ATTRIBUTE_DIRECTORY)) {
                code = 0;
            } else if (remaining == INVALID_FILE_ATTRIBUTES) {
                code = fsLastError();
            }
        }
    }
    free(path);
    free(pattern);
#else
    DIR* directory = opendir(path);
    if (directory == NULL) code = errno;
    free(path);
#endif
    if (code != 0) return fsError(errorCode, code);
    joyeer_array_create_owned_abi(
            result, NULL, 0, sizeof(JoyeerString), fsCloneString, fsDestroyString);
#ifdef _WIN32
    if (directory != INVALID_HANDLE_VALUE) {
        BOOL hasEntry;
        do {
            if (wcscmp(entry.cFileName, L".") != 0 &&
                    wcscmp(entry.cFileName, L"..") != 0) {
                JoyeerString name = { 0 };
                if (!fsWideName(entry.cFileName, &name, &code)) break;
                joyeer_array_append_owned_abi(result, &name);
            }
            hasEntry = FindNextFileW(directory, &entry);
        } while (hasEntry);
        if (code == 0 && GetLastError() != ERROR_NO_MORE_FILES) code = fsLastError();
        if (!FindClose(directory) && code == 0) code = fsLastError();
    }
#else
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) code = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        size_t length = strlen(entry->d_name);
        if (!joyeer_host_valid_utf8((const uint8_t*)entry->d_name, (int64_t)length)) {
            code = EILSEQ;
            break;
        }
        JoyeerString name;
        joyeer_string_clone_abi(&name, (const uint8_t*)entry->d_name, (int64_t)length);
        joyeer_array_append_owned_abi(result, &name);
    }
    if (closedir(directory) != 0 && code == 0) code = errno;
#endif
    if (code == 0) return JOYEER_FS_ERROR_NONE;
    joyeer_array_destroy_abi(result);
    return fsError(errorCode, code);
}

int32_t joyeer_fs_join_path_abi(
        JoyeerString* result, int64_t* errorCode,
        const uint8_t* baseData, int64_t baseCount,
        const uint8_t* pathData, int64_t pathCount) {
    if (result == NULL) joyeer_panic("filesystem result is null");
    *result = (JoyeerString) { 0 };
    fsCheck(errorCode);
    if (baseCount <= 0 || pathCount <= 0 ||
            (baseCount != 0 && baseData == NULL) ||
            (pathCount != 0 && pathData == NULL)) {
#ifdef _WIN32
        return fsError(errorCode, ERROR_INVALID_PARAMETER);
#else
        return fsError(errorCode, EINVAL);
#endif
    }
    if (!joyeer_host_valid_utf8(baseData, baseCount) ||
            !joyeer_host_valid_utf8(pathData, pathCount)) {
#ifdef _WIN32
        return fsError(errorCode, ERROR_NO_UNICODE_TRANSLATION);
#else
        return fsError(errorCode, EILSEQ);
#endif
    }
    if ((baseCount > 0 && memchr(baseData, 0, (size_t)baseCount) != NULL) ||
            (pathCount > 0 && memchr(pathData, 0, (size_t)pathCount) != NULL)) {
#ifdef _WIN32
        return fsError(errorCode, ERROR_INVALID_NAME);
#else
        return fsError(errorCode, EINVAL);
#endif
    }
#ifdef _WIN32
    bool absolute = pathCount > 0 && (pathData[0] == '/' || pathData[0] == '\\');
    bool driveQualified = pathCount >= 2 && fsDriveLetter(pathData[0]) && pathData[1] == ':';
    if (driveQualified && pathCount >= 3 &&
            (pathData[2] == '/' || pathData[2] == '\\'))
        absolute = true;
    bool replaceBase = absolute || driveQualified;
    const uint8_t separator = '\\';
    bool bareDrive = baseCount == 2 && fsDriveLetter(baseData[0]) && baseData[1] == ':';
    bool trailing = baseCount > 0 &&
            (baseData[baseCount - 1] == '/' || baseData[baseCount - 1] == '\\');
    bool separate = !bareDrive && !trailing;
#else
    bool absolute = pathCount > 0 && pathData[0] == '/';
    bool replaceBase = absolute;
    const uint8_t separator = '/';
    bool separate = baseCount > 0 && baseData[baseCount - 1] != '/';
#endif
    if (replaceBase) {
        joyeer_string_clone_abi(result, pathData, pathCount);
        return JOYEER_FS_ERROR_NONE;
    }
    if (baseCount > INT64_MAX - pathCount - (separate ? 1 : 0))
        joyeer_panic("joined path size overflow");
    int64_t length = baseCount + pathCount + (separate ? 1 : 0);
    uint8_t* joined = (uint8_t*)fsAllocate((size_t)length);
    memcpy(joined, baseData, (size_t)baseCount);
    if (separate) joined[baseCount] = separator;
    memcpy(joined + baseCount + (separate ? 1 : 0), pathData, (size_t)pathCount);
    joyeer_string_clone_abi(result, joined, length);
    free(joined);
    return JOYEER_FS_ERROR_NONE;
}
