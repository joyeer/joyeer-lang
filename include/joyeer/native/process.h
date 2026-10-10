#ifndef __joyeer_native_process_h__
#define __joyeer_native_process_h__

#include "joyeer/native/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

enum JoyeerProcessErrorKind {
    JOYEER_PROCESS_ERROR_NONE = 0,
    JOYEER_PROCESS_ERROR_INVALID_INPUT = 1,
    JOYEER_PROCESS_ERROR_NOT_FOUND = 2,
    JOYEER_PROCESS_ERROR_PERMISSION_DENIED = 3,
    JOYEER_PROCESS_ERROR_LAUNCH_FAILED = 4,
    JOYEER_PROCESS_ERROR_WAIT_FAILED = 5,
};

enum JoyeerProcessStatusKind {
    JOYEER_PROCESS_STATUS_EXITED = 0,
    JOYEER_PROCESS_STATUS_SIGNALED = 1,
};

// Runs one explicit executable synchronously, without a shell or PATH lookup.
// Inherits the caller's environment and standard streams. The caller maps the
// status/error tags to Joyeer values; exit codes are not truncated to 8 bits.
// On POSIX, SIGINT/SIGTERM delivered to the child are reported as Signaled;
// this API does not forward signals or provide cancellation of its own.
int32_t joyeer_run_process_abi(
    int32_t* statusKind,
    int64_t* statusCode,
    int64_t* errorCode,
    const uint8_t* executableData,
    int64_t executableCount,
    const JoyeerString* arguments,
    int64_t argumentCount,
    const uint8_t* workingDirectoryData,
    int64_t workingDirectoryCount);

#ifdef __cplusplus
}
#endif

#endif
