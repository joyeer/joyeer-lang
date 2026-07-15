if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED CLANG OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, CLANG, INPUT_FILE, and OUTPUT_FILE are required")
endif()

set(object_file "${OUTPUT_FILE}.obj")

set(compiler_command "${JOYEER_EXECUTABLE}" --lang=v0.1)
if(DEFINED COMPILER_ARGS)
    list(APPEND compiler_command ${COMPILER_ARGS})
endif()
list(APPEND compiler_command --emit-llvm "${OUTPUT_FILE}" "${INPUT_FILE}")

execute_process(
    COMMAND ${compiler_command}
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

file(READ "${OUTPUT_FILE}" emitted_ir)
foreach(expected_pattern IN LISTS EXPECTED_IR_PATTERNS)
    string(FIND "${emitted_ir}" "${expected_pattern}" pattern_at)
    if(pattern_at EQUAL -1)
        file(REMOVE "${OUTPUT_FILE}")
        message(FATAL_ERROR "Joyeer LLVM IR is missing '${expected_pattern}'")
    endif()
endforeach()
foreach(unexpected_pattern IN LISTS UNEXPECTED_IR_PATTERNS)
    string(FIND "${emitted_ir}" "${unexpected_pattern}" pattern_at)
    if(NOT pattern_at EQUAL -1)
        file(REMOVE "${OUTPUT_FILE}")
        message(FATAL_ERROR "Joyeer LLVM IR unexpectedly contains '${unexpected_pattern}'")
    endif()
endforeach()

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
if("${clang_output}${clang_error}" MATCHES "ignoring .*debug info|invalid debug info|debug info with an invalid version")
    file(REMOVE "${object_file}")
    message(FATAL_ERROR
            "Clang discarded compiler debug metadata:\n${clang_output}${clang_error}")
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
