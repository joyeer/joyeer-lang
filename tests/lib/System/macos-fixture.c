#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

// The Joyeer bindings use these offsets and buffers instead of C struct types.
_Static_assert(offsetof(struct stat, st_mode) == 4 && sizeof(mode_t) == 2, "lstat mode layout");
_Static_assert(sizeof(struct stat) <= 256 && _Alignof(struct stat) <= 8, "lstat buffer");
_Static_assert(offsetof(struct dirent, d_name) == 21, "readdir_r name layout");
_Static_assert(sizeof(struct dirent) <= 2048 && _Alignof(struct dirent) <= 8, "readdir_r buffer");
_Static_assert(sizeof(struct dirent *) == 8, "readdir_r output pointer");
_Static_assert(sizeof(posix_spawn_file_actions_t) <= 256 &&
               _Alignof(posix_spawn_file_actions_t) <= 8, "spawn actions buffer");
_Static_assert(sizeof(pid_t) == 4 && sizeof(int) == 4, "spawn PID and wait status buffers");

static const char payload[] = "retry-data";
enum { payload_size = sizeof(payload) - 1 };
static int read_calls, read_offset, write_calls, write_offset, wait_calls;
static FILE *reader, *writer;

static int read_stream(void *cookie, char *bytes, int count) {
    (void)cookie;
    ++read_calls;
    if (read_calls == 1 || read_calls == 3) {
        errno = EINTR;
        return -1;
    }
    if (ferror(reader)) {
        errno = EIO;
        return -1;
    }
    if (read_calls == 2 && count > 3) count = 3;
    if (count > payload_size - read_offset) count = payload_size - read_offset;
    memcpy(bytes, payload + read_offset, (size_t)count);
    read_offset += count;
    return count;
}

static int write_stream(void *cookie, const char *bytes, int count) {
    (void)cookie;
    ++write_calls;
    if (write_calls == 1 || write_calls == 3) {
        errno = EINTR;
        return -1;
    }
    if (write_calls == 2 && count > 3) count = 3;
    if (ferror(writer) || count > payload_size - write_offset ||
            memcmp(bytes, payload + write_offset, (size_t)count) != 0) {
        errno = EIO;
        return -1;
    }
    write_offset += count;
    return count;
}

// Only this test executable substitutes fopen and waitpid. The FILE operations
// themselves are libSystem, so ferror and clearerr see a real stream.
FILE *fopen(const char *restrict path, const char *restrict mode) {
    if (strcmp(path, "interrupted-read") == 0 && strcmp(mode, "rb") == 0) {
        reader = funopen(NULL, read_stream, NULL, NULL, NULL);
        return reader;
    }
    if (strcmp(path, "interrupted-write") == 0 && strcmp(mode, "wbx") == 0) {
        writer = funopen(NULL, NULL, write_stream, NULL, NULL);
        if (writer != NULL && setvbuf(writer, NULL, _IONBF, 0) != 0) {
            fclose(writer);
            errno = EIO;
            return NULL;
        }
        return writer;
    }
    errno = ENOENT;
    return NULL;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    if (wait_calls++ == 0) {
        errno = EINTR;
        return -1;
    }
    return wait4(pid, status, options, NULL);
}

int32_t system_macos_retries_observed(void) {
    return read_calls >= 4 && read_offset == payload_size &&
            write_calls >= 4 && write_offset == payload_size && wait_calls == 2;
}
