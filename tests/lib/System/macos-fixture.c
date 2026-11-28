#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The Joyeer bindings use these offsets and buffers instead of C struct types.
_Static_assert(offsetof(struct stat, st_mode) == 4 && sizeof(mode_t) == 2, "lstat mode layout");
_Static_assert(offsetof(struct stat, st_dev) == 0 && sizeof(dev_t) == 4 &&
               offsetof(struct stat, st_ino) == 8 && sizeof(ino_t) == 8, "fstat identity layout");
_Static_assert(sizeof(struct stat) <= 256 && _Alignof(struct stat) <= 8, "lstat buffer");
_Static_assert(offsetof(struct dirent, d_name) == 21, "readdir_r name layout");
_Static_assert(sizeof(struct dirent) <= 2048 && _Alignof(struct dirent) <= 8, "readdir_r buffer");
_Static_assert(sizeof(struct dirent *) == 8, "readdir_r output pointer");
_Static_assert(sizeof(posix_spawn_file_actions_t) <= 256 &&
               _Alignof(posix_spawn_file_actions_t) <= 8, "spawn actions buffer");
_Static_assert(sizeof(pid_t) == 4 && sizeof(int) == 4, "spawn PID and wait status buffers");
_Static_assert(sizeof(struct timespec) == 16 && offsetof(struct timespec, tv_nsec) == 8,
               "64-bit clock seconds and nanoseconds");

static const char payload[] = "retry-data";
enum { payload_size = sizeof(payload) - 1 };
struct transfer {
    int calls;
    size_t offset;
};
static struct transfer reader, writer, owned_writer;
static struct transfer *active;
static int test_descriptor = -1;
static int read_open_calls, wait_calls;

// Make a real close release the descriptor and report EINTR. Retrying or not
// closing at all must be distinguishable from closing exactly once.
static int close_calls, failed_close_calls;
static int closing_descriptor = -1;

int openx_np(const char *path, int flags, filesec_t security) {
    if (strcmp(path, "interrupted-read") == 0) {
        if (flags != (O_CLOEXEC | O_RDONLY) || security != NULL) {
            errno = EINVAL;
            return -1;
        }
        if (read_open_calls++ == 0) {
            errno = EINTR;
            return -1;
        }
        active = &reader;
        test_descriptor = open("/dev/null", O_CLOEXEC | O_RDONLY);
        return test_descriptor;
    }

    mode_t mode = 0;
    if (flags != (O_CLOEXEC | O_CREAT | O_EXCL | O_WRONLY) ||
        security == NULL || filesec_get_property(security, FILESEC_MODE, &mode) != 0 || mode != 0666) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, "close-interrupted") == 0 || strcmp(path, "write-and-close-fail") == 0) {
        active = NULL;
        test_descriptor = open("/dev/null", O_CLOEXEC | O_WRONLY);
        closing_descriptor = test_descriptor;
        return test_descriptor;
    } else if (strcmp(path, "interrupted-write") == 0) {
        active = &writer;
    } else if (strcmp(path, "owned-interrupted-write") == 0) {
        active = &owned_writer;
    } else {
        errno = ENOENT;
        return -1;
    }
    test_descriptor = open("/dev/null", O_CLOEXEC | O_WRONLY);
    return test_descriptor;
}

// An interrupted call makes no progress; a short successful call does.
static ssize_t transfer_count(struct transfer *state, size_t count) {
    ++state->calls;
    if (state->calls == 1 || state->calls == 3) {
        errno = EINTR;
        return -1;
    }
    if (state->calls == 2 && count > 3) count = 3;
    return (ssize_t)count;
}

ssize_t read(int descriptor, void *bytes, size_t count) {
    if (descriptor != test_descriptor || active != &reader) {
        struct iovec part = { .iov_base = bytes, .iov_len = count };
        return readv(descriptor, &part, 1);
    }
    const size_t remaining = payload_size - reader.offset;
    const ssize_t taken = transfer_count(&reader, count < remaining ? count : remaining);
    if (taken >= 0) {
        memcpy(bytes, payload + reader.offset, (size_t)taken);
        reader.offset += (size_t)taken;
    }
    return taken;
}

ssize_t write(int descriptor, const void *bytes, size_t count) {
    if (descriptor == closing_descriptor && descriptor >= 0) {
        errno = ENOSPC;
        return -1;
    }
    if (descriptor != test_descriptor || (active != &writer && active != &owned_writer)) {
        struct iovec part = { .iov_base = (void *)bytes, .iov_len = count };
        return writev(descriptor, &part, 1);
    }
    const ssize_t taken = transfer_count(active, count);
    if (taken < 0) return taken;
    if ((size_t)taken > payload_size - active->offset ||
        memcmp(bytes, payload + active->offset, (size_t)taken) != 0) {
        errno = EIO;
        return -1;
    }
    active->offset += (size_t)taken;
    return taken;
}

int close(int descriptor) {
    int (*system_close)(int) = (int (*)(int))dlsym(RTLD_NEXT, "close");
    if (system_close == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (descriptor == closing_descriptor && descriptor >= 0) {
        ++failed_close_calls;
        if (system_close(descriptor) != 0) return -1;
        errno = EINTR;
        return -1;
    }
    if (descriptor == test_descriptor && descriptor >= 0) {
        ++close_calls;
        test_descriptor = -1;
    }
    return system_close(descriptor);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    if (wait_calls++ == 0) {
        errno = EINTR;
        return -1;
    }
    return wait4(pid, status, options, NULL);
}

int32_t system_macos_retries_observed(void) {
    return read_open_calls == 2 && reader.calls == 5 && reader.offset == payload_size &&
            writer.calls == 4 && writer.offset == payload_size &&
            owned_writer.calls == 4 && owned_writer.offset == payload_size && wait_calls == 2 &&
            close_calls == 3 && failed_close_calls == 2;
}

static int32_t clock_sample;

void system_macos_clock_sample(int32_t sample) {
    clock_sample = sample;
}

int clock_gettime(clockid_t clock, struct timespec *stamp) {
    if (clock != CLOCK_REALTIME || clock_sample == 0) {
        errno = EINVAL;
        return -1;
    }
    if (clock_sample == 1) {
        stamp->tv_sec = -1;
        stamp->tv_nsec = 999999999;
    } else {
        stamp->tv_sec = INT64_MAX;
        stamp->tv_nsec = 0;
    }
    return 0;
}
