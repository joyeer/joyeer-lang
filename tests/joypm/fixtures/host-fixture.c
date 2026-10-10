#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <sddl.h>
#include <wchar.h>
typedef wchar_t HostChar;
#define HOST_TEXT(value) L##value
#define hostCompare wcscmp
#define hostOpen _wfopen
#define hostMain wmain
#else
#include <sys/stat.h>
#include <unistd.h>
typedef char HostChar;
#define HOST_TEXT(value) value
#define hostCompare strcmp
#define hostOpen fopen
#define hostMain main
#endif

static int failure(const char* message) {
    fprintf(stderr, "joypm host fixture: %s (errno %d)\n", message, errno);
    return 125;
}

static int wideStatus(void) {
#ifdef _WIN32
    return (int)0xF1234567U;
#else
    return 255;
#endif
}

static int terminated(void) {
#ifndef _WIN32
    if (raise(SIGTERM) != 0) return failure("cannot raise SIGTERM");
#endif
    return failure("signal termination is unavailable");
}

#ifdef _WIN32
static int allowSubdirectories(const HostChar* path, int allowed) {
    const wchar_t* sddl = allowed
        ? L"D:P(A;;FA;;;OW)"
        : L"D:P(D;;0x00000004;;;OW)(A;;FA;;;OW)";
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, NULL)) {
        fprintf(stderr, "joypm host fixture: cannot create DACL (Windows code %lu)\n",
                GetLastError());
        return 125;
    }
    const BOOL changed = SetFileSecurityW(path,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    const DWORD error = changed ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!changed) {
        fprintf(stderr, "joypm host fixture: cannot set directory DACL (Windows code %lu)\n",
                error);
        return 125;
    }
    return 0;
}
#endif

static int copyArtifact(const HostChar* source, const HostChar* output, int executable) {
    FILE* input = hostOpen(source, HOST_TEXT("rb"));
    if (!input) return failure("cannot open fixture executable");
    FILE* artifact = hostOpen(output, HOST_TEXT("wb"));
    if (!artifact) {
        fclose(input);
        return failure("cannot create artifact");
    }
    unsigned char buffer[4096];
    size_t count;
    int failed = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        if (fwrite(buffer, 1, count, artifact) != count) {
            failed = 1;
            break;
        }
    }
    if (ferror(input)) failed = 1;
    if (fclose(input) != 0) failed = 1;
    if (fclose(artifact) != 0) failed = 1;
    if (failed) return failure("cannot copy artifact");
#ifndef _WIN32
    if (executable && chmod(output, 0755) != 0) return failure("cannot make artifact executable");
#else
    (void)executable;
#endif
    return 0;
}

static int compile(int argc, HostChar** argv) {
    FILE* control = fopen("host-action.txt", "rb");
    if (!control) return failure("missing compiler action");
    char action[64] = {0};
    const size_t count = fread(action, 1, sizeof(action) - 1, control);
    const int readFailed = ferror(control);
    const int closeFailed = fclose(control) != 0;
    if (readFailed || closeFailed || count == 0) return failure("cannot read compiler action");
    if (strcmp(action, "compiler-wide") == 0) return wideStatus();
    if (strcmp(action, "compiler-signal") == 0) return terminated();

    const HostChar* output = NULL;
    for (int index = 1; index + 1 < argc; ++index) {
        if (hostCompare(argv[index], HOST_TEXT("-o")) == 0) output = argv[index + 1];
    }
    if (!output) return failure("compiler fixture requires native output");
    if (strcmp(action, "artifact-directory") == 0) {
#ifdef _WIN32
        if (_wmkdir(output) != 0) return failure("cannot create directory artifact");
#else
        if (mkdir(output, 0700) != 0) return failure("cannot create directory artifact");
#endif
        return 0;
    }
    if (strcmp(action, "artifact-link") == 0) {
#ifdef _WIN32
        if (!CreateSymbolicLinkW(output, argv[0], SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            fprintf(stderr, "joypm host fixture: cannot create artifact link (Windows code %lu)\n",
                    GetLastError());
            return 125;
        }
#else
        if (symlink(argv[0], output) != 0) return failure("cannot create artifact link");
#endif
        return 0;
    }
    if (strcmp(action, "artifact-empty") == 0) {
        FILE* artifact = hostOpen(output, HOST_TEXT("wb"));
        if (!artifact) return failure("cannot create empty artifact");
        if (fclose(artifact) != 0) return failure("cannot close empty artifact");
        return 0;
    }
    if (strcmp(action, "artifact-copy") == 0) return copyArtifact(argv[0], output, 1);
    if (strcmp(action, "artifact-not-executable") == 0) return copyArtifact(argv[0], output, 0);
    return failure("unknown compiler action");
}

int hostMain(int argc, HostChar** argv) {
    if (argc < 2) return failure("missing action");
    if (hostCompare(argv[1], HOST_TEXT("--module-name")) == 0) return compile(argc, argv);
    if (hostCompare(argv[1], HOST_TEXT("exit-wide")) == 0) return wideStatus();
    if (hostCompare(argv[1], HOST_TEXT("signal")) == 0) return terminated();
    if (argc != 3) return failure("action requires a path");
#ifdef _WIN32
    if (hostCompare(argv[1], HOST_TEXT("block-subdirectories")) == 0)
        return allowSubdirectories(argv[2], 0);
    if (hostCompare(argv[1], HOST_TEXT("allow-subdirectories")) == 0)
        return allowSubdirectories(argv[2], 1);
#else
    if (hostCompare(argv[1], HOST_TEXT("fifo")) == 0) {
        if (mkfifo(argv[2], 0600) != 0) return failure("cannot create FIFO");
        return 0;
    }
#endif
    return failure("unknown action");
}
