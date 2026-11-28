cmake_minimum_required(VERSION 3.20)

execute_process(COMMAND mktemp -d "${TEST_DIRECTORY}/asan-scope-${MODE}.XXXXXXXX"
    RESULT_VARIABLE status OUTPUT_VARIABLE work ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Could not claim an ASan object test directory: ${error}")
endif()

function(emit option object authorization)
    execute_process(COMMAND "${JOYEER_EXECUTABLE}" "-${MODE}"
        --module-name asan_scope.root --runtime-core-module asan_scope.root
        "${authorization}" asan_scope.dependency
        --module-source "asan_scope.dependency=${CMAKE_CURRENT_LIST_DIR}/asan_scope_dependency.joyeer"
        "${option}" "${object}" -- "${CMAKE_CURRENT_LIST_DIR}/asan_scope_root.joyeer"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(status "${status}" PARENT_SCOPE)
    set(messages "${output}${error}" PARENT_SCOPE)
endfunction()

function(check_symbols object instrumented)
    execute_process(COMMAND "${LLVM_NM}" "${object}"
        RESULT_VARIABLE status OUTPUT_VARIABLE symbols ERROR_VARIABLE error)
    if(NOT status STREQUAL "0" OR NOT symbols MATCHES " T asan_scope_root_read"
       OR NOT symbols MATCHES " T asan_scope_dependency_read")
        message(FATAL_ERROR "Expected both definitions in ${object}:\n${symbols}${error}")
    endif()
    if(instrumented AND NOT symbols MATCHES " U __asan_report_load1")
        message(FATAL_ERROR "Expected a sanitized byte load in ${object}:\n${symbols}")
    elseif(NOT instrumented AND symbols MATCHES " __asan_")
        message(FATAL_ERROR "Ordinary object unexpectedly contains ASan symbols:\n${symbols}")
    endif()
endfunction()

set(plain "${work}/plain.o")
emit(--emit-runtime-object "${plain}" --runtime-module)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Ordinary mixed-module output failed: ${status}\n${messages}")
endif()
check_symbols("${plain}" FALSE)

set(core "${work}/core.o")
emit(--emit-asan-runtime-object "${core}" --runtime-core-module)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "All-core ASan object output failed: ${status}\n${messages}")
endif()
check_symbols("${core}" TRUE)

set(mixed "${work}/mixed.o")
emit(--emit-asan-runtime-object "${mixed}" --runtime-module)
if(NOT status STREQUAL "1" OR NOT messages MATCHES "linker.tool-failure"
   OR NOT messages MATCHES "sanitizer emission requires a verified runtime core object")
    message(FATAL_ERROR "Expected ASan object emission to reject a non-core definition: ${status}\n${messages}")
endif()
if(EXISTS "${mixed}" OR IS_SYMLINK "${mixed}")
    message(FATAL_ERROR "Rejected ASan object emission left an output file: ${mixed}")
endif()

file(REMOVE "${plain}" "${core}")
execute_process(COMMAND rmdir "${work}" COMMAND_ERROR_IS_FATAL ANY)
