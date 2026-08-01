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
    if (!joyeer_native_backend_has_coff_linker()) {
        return 3;
    }

    JoyeerNativeBackendObjectOptions invalidOptions = { 0 };
    const JoyeerNativeBackendStatus status = joyeer_native_backend_emit_object(
            &invalidOptions,
            collectDiagnostic,
            NULL);
    if (status != JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT || !diagnosticSeen) {
        return 4;
    }
    return 0;
}