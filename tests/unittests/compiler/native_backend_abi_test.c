#include "joyeer/backend/native_backend.h"

#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#include <stdio.h>
#include <wchar.h>
#define NOMINMAX
#include <windows.h>
#endif

static int diagnosticSeen = 0;

static void collectDiagnostic(
        void* context,
        JoyeerNativeBackendStatus status,
        const char* message,
        size_t messageSize) {
    (void)context;
    if (status == JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT &&
        message != NULL && messageSize != 0) {
        diagnosticSeen = 1;
    }
}

#if defined(_WIN32)
static void printDiagnostic(
        void* context,
        JoyeerNativeBackendStatus status,
        const char* message,
        size_t messageSize) {
    (void)context;
    fprintf(stderr, "native backend status %u: ", (unsigned)status);
    if (message != NULL) fwrite(message, 1, messageSize, stderr);
    fputc('\n', stderr);
}

static int isNonemptyFile(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) &&
            !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            (attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0);
}

static int checkUnicodePaths(void) {
    enum { pathCapacity = 1024, utf8Capacity = pathCapacity * 4 };
    wchar_t temporary[pathCapacity];
    wchar_t directory[pathCapacity];
    wchar_t objectPath[pathCapacity] = { 0 };
    wchar_t executablePath[pathCapacity] = { 0 };
    wchar_t pdbPath[pathCapacity] = { 0 };
    char objectUtf8[utf8Capacity];
    char outputArgument[utf8Capacity + 5] = "/OUT:";
    char pdbArgument[utf8Capacity + 5] = "/PDB:";
    int result = 6;
    const DWORD length = GetTempPathW(pathCapacity, temporary);
    if (length == 0 || length >= pathCapacity) return result;
    /* ASCII source spells Chinese and supplementary characters without a code-page dependency. */
    const int directoryLength = swprintf(
            directory, pathCapacity,
            L"%lsjoyeer backend \u4E2D\xD83D\xDE80-%lu-%llu",
            temporary, (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64());
    if (directoryLength < 0 || directoryLength >= pathCapacity ||
        !CreateDirectoryW(directory, NULL)) {
        return result;
    }
    const int objectLength = swprintf(
            objectPath, pathCapacity, L"%ls\\object \u4E2D\xD83D\xDE80.obj", directory);
    const int executableLength = swprintf(
            executablePath, pathCapacity, L"%ls\\program \u4E2D\xD83D\xDE80.exe", directory);
    const int pdbLength = swprintf(
            pdbPath, pathCapacity, L"%ls\\program \u4E2D\xD83D\xDE80.pdb", directory);
    if (objectLength < 0 || objectLength >= pathCapacity ||
        executableLength < 0 || executableLength >= pathCapacity ||
        pdbLength < 0 || pdbLength >= pathCapacity) {
        goto cleanup;
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, objectPath, -1,
                            objectUtf8, utf8Capacity, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, executablePath, -1,
                            outputArgument + 5, utf8Capacity, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pdbPath, -1,
                            pdbArgument + 5, utf8Capacity, NULL, NULL)) {
        goto cleanup;
    }

    /* Match the emitter's byte-escaped UTF-8 source/debug identities. */
    static const char llvmIR[] =
            "source_filename = \"source \\E4\\B8\\AD\\F0\\9F\\9A\\80.joyeer\"\n"
            "define void @unicode_entry() !dbg !4 {\n"
            "entry:\n"
            "  ret void, !dbg !5\n"
            "}\n"
            "!llvm.dbg.cu = !{!0}\n"
            "!llvm.module.flags = !{!6, !7}\n"
            "!0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, "
                "producer: \"Joyeer\", isOptimized: false, runtimeVersion: 0, "
                "emissionKind: LineTablesOnly)\n"
            "!1 = !DIFile(filename: \"source \\E4\\B8\\AD\\F0\\9F\\9A\\80.joyeer\", "
                "directory: \"C:/source \\E4\\B8\\AD\\F0\\9F\\9A\\80\")\n"
            "!2 = !DISubroutineType(types: !3)\n"
            "!3 = !{}\n"
            "!4 = distinct !DISubprogram(name: \"unicode_entry\", "
                "linkageName: \"unicode_entry\", scope: !1, file: !1, line: 1, "
                "type: !2, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)\n"
            "!5 = !DILocation(line: 1, column: 1, scope: !4)\n"
            "!6 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
            "!7 = !{i32 2, !\"CodeView\", i32 1}\n";
    const JoyeerNativeBackendObjectOptions objectOptions = {
        JOYEER_NATIVE_BACKEND_ABI_VERSION,
        sizeof(JoyeerNativeBackendObjectOptions),
        llvmIR,
        sizeof(llvmIR) - 1,
        objectUtf8,
        JOYEER_NATIVE_BACKEND_O0,
    };
    result = 7;
    if (joyeer_native_backend_emit_object(&objectOptions, printDiagnostic, NULL) !=
        JOYEER_NATIVE_BACKEND_SUCCESS || !isNonemptyFile(objectPath)) {
        goto cleanup;
    }

    const char* arguments[] = {
        "lld-link", "/NOLOGO", "/NODEFAULTLIB", "/SUBSYSTEM:CONSOLE",
        "/ENTRY:unicode_entry", "/DEBUG:FULL", "/INCREMENTAL:NO",
        outputArgument, pdbArgument, objectUtf8,
    };
    const JoyeerNativeBackendLinkOptions linkOptions = {
        JOYEER_NATIVE_BACKEND_ABI_VERSION,
        sizeof(JoyeerNativeBackendLinkOptions),
        arguments,
        sizeof(arguments) / sizeof(arguments[0]),
    };
    result = 8;
    if (joyeer_native_backend_link_coff(&linkOptions, printDiagnostic, NULL) !=
        JOYEER_NATIVE_BACKEND_SUCCESS || !isNonemptyFile(executablePath) ||
        !isNonemptyFile(pdbPath)) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (objectPath[0] != L'\0') DeleteFileW(objectPath);
    if (executablePath[0] != L'\0') DeleteFileW(executablePath);
    if (pdbPath[0] != L'\0') DeleteFileW(pdbPath);
    if (!RemoveDirectoryW(directory) && result == 0) result = 9;
    if (result != 0) fprintf(stderr, "Unicode native backend regression failed at stage %d\n", result);
    return result;
}
#endif

int main(void) {
    if (joyeer_native_backend_abi_version() !=
        JOYEER_NATIVE_BACKEND_ABI_VERSION) {
        return 1;
    }
    if (strcmp(joyeer_native_backend_llvm_version(), "22.1.8") != 0) {
        return 2;
    }
#if defined(_WIN32)
    if (!joyeer_native_backend_has_coff_linker() ||
        joyeer_native_backend_has_macho_linker() ||
        joyeer_native_backend_has_elf_linker()) {
        return 3;
    }
#elif defined(__APPLE__)
    if (joyeer_native_backend_has_coff_linker() ||
        !joyeer_native_backend_has_macho_linker() ||
        joyeer_native_backend_has_elf_linker()) {
        return 3;
    }
#else
    if (joyeer_native_backend_has_coff_linker() ||
        joyeer_native_backend_has_macho_linker() ||
        !joyeer_native_backend_has_elf_linker()) {
        return 3;
    }
#endif

    JoyeerNativeBackendObjectOptions invalidOptions = { 0 };
    const JoyeerNativeBackendStatus status = joyeer_native_backend_emit_object(
            &invalidOptions,
            collectDiagnostic,
            NULL);
    if (status != JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT || !diagnosticSeen) {
        return 4;
    }

    diagnosticSeen = 0;
    JoyeerNativeBackendLinkOptions invalidLinkOptions = { 0 };
#if defined(_WIN32)
    const JoyeerNativeBackendStatus linkStatus = joyeer_native_backend_link_coff(
#elif defined(__APPLE__)
    const JoyeerNativeBackendStatus linkStatus = joyeer_native_backend_link_macho(
#else
    const JoyeerNativeBackendStatus linkStatus = joyeer_native_backend_link_elf(
#endif
            &invalidLinkOptions,
            collectDiagnostic,
            NULL);
    if (linkStatus != JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT || !diagnosticSeen) {
        return 5;
    }
#if defined(_WIN32)
    return checkUnicodePaths();
#else
    return 0;
#endif
}