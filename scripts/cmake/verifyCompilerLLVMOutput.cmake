if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED CLANG OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, CLANG, INPUT_FILE, and OUTPUT_FILE are required")
endif()

set(object_file "${OUTPUT_FILE}.obj")

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 --emit-llvm "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
if(NOT compiler_result EQUAL 0)
    message(FATAL_ERROR
            "Joyeer LLVM emission failed (${compiler_result}):\n${compiler_output}${compiler_error}")
endif()
if(NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "Joyeer succeeded without writing ${OUTPUT_FILE}")
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
            "Clang rejected compiler LLVM IR (${clang_result}):\n${clang_output}${clang_error}")
endif()
if(NOT EXISTS "${object_file}")
    message(FATAL_ERROR "Clang succeeded without producing ${object_file}")
endif()

file(SIZE "${object_file}" object_size)
file(REMOVE "${object_file}")
if(object_size EQUAL 0)
    message(FATAL_ERROR "Clang produced an empty object file")
endif()

message(STATUS "Joyeer emitted LLVM IR and Clang emitted ${object_size} object bytes")
