if(NOT DEFINED GENERATOR OR NOT DEFINED CLANG OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GENERATOR, CLANG, and OUTPUT_FILE are required")
endif()

set(object_file "${OUTPUT_FILE}.obj")

set(generator_command "${GENERATOR}" "${OUTPUT_FILE}")
if(DEFINED GENERATOR_ARGS)
    list(APPEND generator_command ${GENERATOR_ARGS})
endif()

execute_process(
    COMMAND ${generator_command}
        RESULT_VARIABLE generator_result
        OUTPUT_VARIABLE generator_output
        ERROR_VARIABLE generator_error
)
if(NOT generator_result EQUAL 0)
    message(FATAL_ERROR
            "LLVM fixture generation failed (${generator_result}):\n${generator_output}${generator_error}")
endif()

if(DEFINED EXPECTED_IR_PATTERN)
    file(READ "${OUTPUT_FILE}" emitted_ir)
    string(FIND "${emitted_ir}" "${EXPECTED_IR_PATTERN}" pattern_at)
    if(pattern_at EQUAL -1)
        file(REMOVE "${OUTPUT_FILE}")
        message(FATAL_ERROR
                "Generated LLVM IR is missing '${EXPECTED_IR_PATTERN}'")
    endif()
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
if("${clang_output}${clang_error}" MATCHES "ignoring .*debug info|invalid debug info|debug info with an invalid version")
    file(REMOVE "${object_file}")
    message(FATAL_ERROR
            "Clang discarded generated debug metadata:\n${clang_output}${clang_error}")
endif()

if(NOT EXISTS "${object_file}")
    message(FATAL_ERROR "Clang succeeded without producing ${object_file}")
endif()

file(SIZE "${object_file}" object_size)
if(LLVM_READOBJ AND EXPECTED_OBJECT_PATTERNS)
    set(readobj_arguments --sections)
    if(READOBJ_CODEVIEW)
        list(APPEND readobj_arguments --codeview)
    endif()
    execute_process(
        COMMAND "${LLVM_READOBJ}" ${readobj_arguments} "${object_file}"
        RESULT_VARIABLE readobj_result
        OUTPUT_VARIABLE readobj_output
        ERROR_VARIABLE readobj_error
    )
    set(readobj_combined "${readobj_output}${readobj_error}")
    if(NOT readobj_result EQUAL 0)
        file(REMOVE "${object_file}")
        message(FATAL_ERROR
                "Cannot inspect object debug information:\n${readobj_output}${readobj_error}")
    endif()
    foreach(expected_pattern IN LISTS EXPECTED_OBJECT_PATTERNS)
        string(FIND "${readobj_combined}" "${expected_pattern}" object_pattern_at)
        if(object_pattern_at EQUAL -1)
            file(REMOVE "${object_file}")
            message(FATAL_ERROR
                    "Object debug information is missing '${expected_pattern}':\n${readobj_output}${readobj_error}")
        endif()
    endforeach()
endif()
file(REMOVE "${object_file}")
if(object_size EQUAL 0)
    message(FATAL_ERROR "Clang produced an empty object file")
endif()

message(STATUS "Clang accepted generated LLVM IR and emitted ${object_size} bytes")
