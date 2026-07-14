if(NOT DEFINED GENERATOR OR NOT DEFINED CLANG OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GENERATOR, CLANG, and OUTPUT_FILE are required")
endif()

set(object_file "${OUTPUT_FILE}.obj")

execute_process(
        COMMAND "${GENERATOR}" "${OUTPUT_FILE}"
        RESULT_VARIABLE generator_result
        OUTPUT_VARIABLE generator_output
        ERROR_VARIABLE generator_error
)
if(NOT generator_result EQUAL 0)
    message(FATAL_ERROR
            "LLVM fixture generation failed (${generator_result}):\n${generator_output}${generator_error}")
endif()

execute_process(
        COMMAND "${CLANG}" -Wno-override-module -x ir -c "${OUTPUT_FILE}" -o "${object_file}"
        RESULT_VARIABLE clang_result
        OUTPUT_VARIABLE clang_output
        ERROR_VARIABLE clang_error
)
file(REMOVE "${OUTPUT_FILE}")

if(NOT clang_result EQUAL 0)
    file(REMOVE "${object_file}")
    message(FATAL_ERROR
            "Clang rejected generated LLVM IR (${clang_result}):\n${clang_output}${clang_error}")
endif()

if(NOT EXISTS "${object_file}")
    message(FATAL_ERROR "Clang succeeded without producing ${object_file}")
endif()

file(SIZE "${object_file}" object_size)
file(REMOVE "${object_file}")
if(object_size EQUAL 0)
    message(FATAL_ERROR "Clang produced an empty object file")
endif()

message(STATUS "Clang accepted generated LLVM IR and emitted ${object_size} bytes")
