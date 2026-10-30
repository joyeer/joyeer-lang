#ifndef __joyeer_backend_h__
#define __joyeer_backend_h__

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(JOYEER_BACKEND_BUILD)
#define JOYEER_BACKEND_API __declspec(dllexport)
#else
#define JOYEER_BACKEND_API __declspec(dllimport)
#endif
#else
#define JOYEER_BACKEND_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JOYEER_BACKEND_ABI_VERSION = 1,
};

/* Backend-owned LLVM context/module. Destroy only through the backend API;
 * clients must not interpret the handle or share allocators across this ABI. */
typedef struct JoyeerBackendModule JoyeerBackendModule;

typedef uint32_t JoyeerBackendStatus;
enum {
    JOYEER_BACKEND_SUCCESS = 0,
    JOYEER_BACKEND_INVALID_ARGUMENT = 1,
    JOYEER_BACKEND_INVALID_IR = 2,
    JOYEER_BACKEND_CODE_GENERATION_FAILED = 3,
    JOYEER_BACKEND_LINK_FAILED = 4,
    JOYEER_BACKEND_FILE_ERROR = 5,
    JOYEER_BACKEND_INTERNAL_ERROR = 6,
};

typedef uint32_t JoyeerBackendOptimizationLevel;
enum {
    JOYEER_BACKEND_O0 = 0,
    JOYEER_BACKEND_O1 = 1,
    JOYEER_BACKEND_O2 = 2,
    JOYEER_BACKEND_O3 = 3,
};

typedef void (*JoyeerBackendDiagnosticCallback)(
        void* context,
        JoyeerBackendStatus status,
        const char* message,
        size_t messageSize);

typedef struct JoyeerBackendObjectOptions {
    uint32_t abiVersion;
    uint32_t structSize;
    const char* llvmIR;
    size_t llvmIRSize;
    /* NUL-terminated UTF-8 filesystem path, including on Windows. */
    const char* outputPath;
    JoyeerBackendOptimizationLevel optimizationLevel;
    /* When set, compile this module directly; llvmIR/llvmIRSize are ignored. */
    const JoyeerBackendModule* module;
} JoyeerBackendObjectOptions;

typedef struct JoyeerBackendModuleOptions {
    uint32_t abiVersion;
    uint32_t structSize;
    uint32_t irVersion;
    const char* joyeerIR;
    size_t joyeerIRSize;
    JoyeerBackendOptimizationLevel optimizationLevel;
    uint32_t emitLineTables;
    uint32_t emitVariables;
    uint32_t codeView;
} JoyeerBackendModuleOptions;

typedef struct JoyeerBackendIRDiagnostic {
    uint32_t id;
    uint32_t sourceId;
    uint32_t offset;
    uint32_t length;
    /* UINT32_MAX denotes absence. Message bytes are borrowed during callback. */
    uint32_t function;
    uint32_t block;
    const char* message;
    size_t messageSize;
} JoyeerBackendIRDiagnostic;

typedef void (*JoyeerBackendIRDiagnosticCallback)(
        void* context, const JoyeerBackendIRDiagnostic* diagnostic);
typedef void (*JoyeerBackendTextCallback)(
        void* context, const char* text, size_t textSize);

JOYEER_BACKEND_API JoyeerBackendStatus joyeer_backend_build_module(
        const JoyeerBackendModuleOptions* options,
        JoyeerBackendModule** result,
        JoyeerBackendIRDiagnosticCallback diagnosticCallback,
        void* diagnosticContext);
JOYEER_BACKEND_API void joyeer_backend_destroy_module(JoyeerBackendModule* module);
JOYEER_BACKEND_API int joyeer_backend_module_has_entry(const JoyeerBackendModule* module);
JOYEER_BACKEND_API JoyeerBackendStatus joyeer_backend_print_module(
        const JoyeerBackendModule* module,
        JoyeerBackendTextCallback callback, void* context);

typedef struct JoyeerBackendLinkOptions {
    uint32_t abiVersion;
    uint32_t structSize;
    /* NUL-terminated UTF-8 argv entries; pass each argument separately, unquoted. */
    const char* const* arguments;
    size_t argumentCount;
} JoyeerBackendLinkOptions;

JOYEER_BACKEND_API uint32_t joyeer_backend_abi_version(void);
JOYEER_BACKEND_API const char* joyeer_backend_llvm_version(void);
JOYEER_BACKEND_API int joyeer_backend_has_coff_linker(void);
JOYEER_BACKEND_API int joyeer_backend_has_macho_linker(void);
JOYEER_BACKEND_API int joyeer_backend_has_elf_linker(void);
JOYEER_BACKEND_API JoyeerBackendStatus
joyeer_backend_emit_object(
        const JoyeerBackendObjectOptions* options,
        JoyeerBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext);
JOYEER_BACKEND_API JoyeerBackendStatus
joyeer_backend_link_coff(
        const JoyeerBackendLinkOptions* options,
        JoyeerBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext);
JOYEER_BACKEND_API JoyeerBackendStatus
joyeer_backend_link_macho(
    const JoyeerBackendLinkOptions* options,
    JoyeerBackendDiagnosticCallback diagnosticCallback,
    void* diagnosticContext);
JOYEER_BACKEND_API JoyeerBackendStatus
joyeer_backend_link_elf(
    const JoyeerBackendLinkOptions* options,
    JoyeerBackendDiagnosticCallback diagnosticCallback,
    void* diagnosticContext);

#ifdef __cplusplus
}
#endif

#endif
