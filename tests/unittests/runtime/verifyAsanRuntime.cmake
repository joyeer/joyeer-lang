execute_process(COMMAND "${PROBE}" "${MODE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
if(MODE STREQUAL "valid")
    if(NOT status STREQUAL "0" OR NOT output STREQUAL "" OR NOT error STREQUAL "")
        message(FATAL_ERROR "Sanitized runtime probe failed: ${status}\n${output}${error}")
    endif()
elseif(status STREQUAL "0" OR NOT error MATCHES "AddressSanitizer: heap-buffer-overflow"
        OR NOT error MATCHES "READ of size 1 at")
    message(FATAL_ERROR "Expected a checked access in the Joyeer runtime: ${status}\n${output}${error}")
endif()
