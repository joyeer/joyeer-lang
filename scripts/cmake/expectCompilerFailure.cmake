if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED EXPECTED_PATTERN)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, and EXPECTED_PATTERN are required")
endif()

set(command "${JOYEER_EXECUTABLE}" --lang=v0.1)
if(DEFINED EXTRA_ARGS)
    list(APPEND command ${EXTRA_ARGS})
endif()
list(APPEND command "${INPUT_FILE}")

execute_process(
    COMMAND ${command}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
)

if(DEFINED OUTPUT_TO_REMOVE)
    file(REMOVE "${OUTPUT_TO_REMOVE}")
endif()

if(result EQUAL 0)
    message(FATAL_ERROR "Compiler validation unexpectedly succeeded")
endif()

set(combined "${output}${error}")
if(NOT combined MATCHES "${EXPECTED_PATTERN}")
    message(FATAL_ERROR
            "Compiler failed without the expected diagnostic '${EXPECTED_PATTERN}':\n${combined}")
endif()

message(STATUS "Observed expected compiler failure: ${EXPECTED_PATTERN}")