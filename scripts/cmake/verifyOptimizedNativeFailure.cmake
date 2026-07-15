if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED EXPECTED_PATTERN)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, and EXPECTED_PATTERN are required")
endif()

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 -O2 -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
if(NOT compiler_result EQUAL 0)
    message(FATAL_ERROR
            "Optimized native compilation failed (${compiler_result}):\n${compiler_output}${compiler_error}")
endif()
if(NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "Joyeer succeeded without producing ${OUTPUT_FILE}")
endif()

execute_process(
        COMMAND "${OUTPUT_FILE}"
        RESULT_VARIABLE program_result
        OUTPUT_VARIABLE program_output
        ERROR_VARIABLE program_error
)
file(REMOVE "${OUTPUT_FILE}")

if(program_result STREQUAL "0")
    message(FATAL_ERROR
            "Optimized native program unexpectedly succeeded:\n${program_output}${program_error}")
endif()

set(combined_output "${program_output}${program_error}")
if(NOT combined_output MATCHES "${EXPECTED_PATTERN}")
    message(FATAL_ERROR
            "Optimized native failure did not match '${EXPECTED_PATTERN}':\n${combined_output}")
endif()

message(STATUS "Optimized native executable preserved the expected safety trap")
