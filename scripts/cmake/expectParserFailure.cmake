if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED EXPECTED_PATTERN)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, and EXPECTED_PATTERN are required")
endif()

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 "${INPUT_FILE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
)

if(result EQUAL 0)
    message(FATAL_ERROR "Parser validation unexpectedly succeeded")
endif()

set(combined "${output}${error}")
if(NOT combined MATCHES "${EXPECTED_PATTERN}")
    message(FATAL_ERROR
            "Parser failed without the expected diagnostic '${EXPECTED_PATTERN}':\n${combined}")
endif()

message(STATUS "Observed expected parser failure: ${EXPECTED_PATTERN}")
