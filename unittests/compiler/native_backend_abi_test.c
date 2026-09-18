#include "joyeer/backend/native_backend.h"

#include <stddef.h>
#include <string.h>

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
    return 0;
}