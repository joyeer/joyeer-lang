if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED EXPECTED_OUTPUT)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, and EXPECTED_OUTPUT are required")
endif()

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
if(NOT compiler_result EQUAL 0)
    message(FATAL_ERROR
            "Native compilation failed (${compiler_result}):\n${compiler_output}${compiler_error}")
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

if(NOT program_result EQUAL 0)
    message(FATAL_ERROR
            "Native program failed (${program_result}):\n${program_output}${program_error}")
endif()

string(REPLACE "\r\n" "\n" normalized_output "${program_output}")
if(NOT normalized_output STREQUAL EXPECTED_OUTPUT)
    message(FATAL_ERROR
            "Native output mismatch:\nExpected:\n${EXPECTED_OUTPUT}\nActual:\n${normalized_output}")
endif()

message(STATUS "Native executable produced expected output")
