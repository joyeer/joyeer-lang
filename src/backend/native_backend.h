#ifndef __joyeer_backend_native_backend_h__
#define __joyeer_backend_native_backend_h__

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(JOYEER_NATIVE_BACKEND_BUILD)
#define JOYEER_NATIVE_BACKEND_API __declspec(dllexport)
#else
#define JOYEER_NATIVE_BACKEND_API __declspec(dllimport)
#endif
#else
#define JOYEER_NATIVE_BACKEND_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JOYEER_NATIVE_BACKEND_ABI_VERSION = 1,
};

/* Backend-owned LLVM context/module. Destroy only through the backend API;
 * clients must not interpret the handle or share allocators across this ABI. */
typedef struct JoyeerNativeBackendModule JoyeerNativeBackendModule;

typedef uint32_t JoyeerNativeBackendStatus;
enum {
    JOYEER_NATIVE_BACKEND_SUCCESS = 0,
    JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT = 1,
    JOYEER_NATIVE_BACKEND_INVALID_IR = 2,
    JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED = 3,
    JOYEER_NATIVE_BACKEND_LINK_FAILED = 4,
    JOYEER_NATIVE_BACKEND_FILE_ERROR = 5,
    JOYEER_NATIVE_BACKEND_INTERNAL_ERROR = 6,
};

typedef uint32_t JoyeerNativeBackendOptimizationLevel;
enum {
    JOYEER_NATIVE_BACKEND_O0 = 0,
    JOYEER_NATIVE_BACKEND_O1 = 1,
    JOYEER_NATIVE_BACKEND_O2 = 2,
    JOYEER_NATIVE_BACKEND_O3 = 3,
};

typedef void (*JoyeerNativeBackendDiagnosticCallback)(
        void* context,
        JoyeerNativeBackendStatus status,
        const char* message,
        size_t messageSize);

typedef struct JoyeerNativeBackendObjectOptions {
    uint32_t abiVersion;
    uint32_t structSize;
    const char* llvmIR;
    size_t llvmIRSize;
    /* NUL-terminated UTF-8 filesystem path, including on Windows. */
    const char* outputPath;
    JoyeerNativeBackendOptimizationLevel optimizationLevel;
    /* When set, compile this module directly; llvmIR/llvmIRSize are ignored. */
    const JoyeerNativeBackendModule* module;
} JoyeerNativeBackendObjectOptions;

typedef struct JoyeerNativeBackendModuleOptions {
    uint32_t abiVersion;
    uint32_t structSize;
    uint32_t irVersion;
    const char* joyeerIR;
    size_t joyeerIRSize;
    JoyeerNativeBackendOptimizationLevel optimizationLevel;
    uint32_t emitLineTables;
    uint32_t emitVariables;
    uint32_t codeView;
} JoyeerNativeBackendModuleOptions;

typedef struct JoyeerNativeBackendIRDiagnostic {
    uint32_t id;
    uint32_t sourceId;
    uint32_t offset;
    uint32_t length;
    /* UINT32_MAX denotes absence. Message bytes are borrowed during callback. */
    uint32_t function;
    uint32_t block;
    const char* message;
    size_t messageSize;
} JoyeerNativeBackendIRDiagnostic;

typedef void (*JoyeerNativeBackendIRDiagnosticCallback)(
        void* context, const JoyeerNativeBackendIRDiagnostic* diagnostic);
typedef void (*JoyeerNativeBackendTextCallback)(
        void* context, const char* text, size_t textSize);

JOYEER_NATIVE_BACKEND_API JoyeerNativeBackendStatus joyeer_native_backend_build_module(
        const JoyeerNativeBackendModuleOptions* options,
        JoyeerNativeBackendModule** result,
        JoyeerNativeBackendIRDiagnosticCallback diagnosticCallback,
        void* diagnosticContext);
JOYEER_NATIVE_BACKEND_API void joyeer_native_backend_destroy_module(JoyeerNativeBackendModule* module);
JOYEER_NATIVE_BACKEND_API int joyeer_native_backend_module_has_entry(const JoyeerNativeBackendModule* module);
JOYEER_NATIVE_BACKEND_API JoyeerNativeBackendStatus joyeer_native_backend_print_module(
        const JoyeerNativeBackendModule* module,
        JoyeerNativeBackendTextCallback callback, void* context);

typedef struct JoyeerNativeBackendLinkOptions {
    uint32_t abiVersion;
    uint32_t structSize;
    /* NUL-terminated UTF-8 argv entries; pass each argument separately, unquoted. */
    const char* const* arguments;
    size_t argumentCount;
} JoyeerNativeBackendLinkOptions;

JOYEER_NATIVE_BACKEND_API uint32_t joyeer_native_backend_abi_version(void);
JOYEER_NATIVE_BACKEND_API const char* joyeer_native_backend_llvm_version(void);
JOYEER_NATIVE_BACKEND_API int joyeer_native_backend_has_coff_linker(void);
JOYEER_NATIVE_BACKEND_API int joyeer_native_backend_has_macho_linker(void);
JOYEER_NATIVE_BACKEND_API int joyeer_native_backend_has_elf_linker(void);
JOYEER_NATIVE_BACKEND_API JoyeerNativeBackendStatus
joyeer_native_backend_emit_object(
        const JoyeerNativeBackendObjectOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext);
JOYEER_NATIVE_BACKEND_API JoyeerNativeBackendStatus
joyeer_native_backend_link_coff(
        const JoyeerNativeBackendLinkOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext);
JOYEER_NATIVE_BACKEND_API JoyeerNativeBackendStatus
joyeer_native_backend_link_macho(
    const JoyeerNativeBackendLinkOptions* options,
    JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
    void* diagnosticContext);
JOYEER_NATIVE_BACKEND_API JoyeerNativeBackendStatus
joyeer_native_backend_link_elf(
    const JoyeerNativeBackendLinkOptions* options,
    JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
    void* diagnosticContext);

#ifdef __cplusplus
}
#endif

#endif