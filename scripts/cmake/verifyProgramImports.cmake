# Compiles a Joyeer program and checks what the Windows executable imports
# (decision 0011): a program that never calls the C library starts at its own entry
# point and imports only the operating system's functions, so the exact list is known;
# one that does keeps the C startup and imports far more.
#
# Inputs: JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, READOBJ, COMPILER_ARGS (list), and
# either EXPECTED_IMPORTS (the exact list, sorted) or MINIMUM_COUNT.
foreach(variable IN ITEMS JOYEER_EXECUTABLE INPUT_FILE OUTPUT_FILE READOBJ)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
file(REMOVE "${OUTPUT_FILE}")
execute_process(COMMAND "${JOYEER_EXECUTABLE}" ${COMPILER_ARGS} -o "${OUTPUT_FILE}" -- "${INPUT_FILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "Compiling ${INPUT_FILE} failed (${status}):\n${output}${error}")
endif()
execute_process(COMMAND "${READOBJ}" --coff-imports "${OUTPUT_FILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE text ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${READOBJ} failed: ${error}")
endif()
string(REPLACE "\r" "" text "${text}")
string(REPLACE "\n" ";" lines "${text}")
set(imports)
foreach(line IN LISTS lines)
    if(line MATCHES "^ *Symbol: ([A-Za-z0-9_@?$.]+)")
        list(APPEND imports "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(SORT imports)
list(LENGTH imports count)
if(DEFINED EXPECTED_IMPORTS)
    list(SORT EXPECTED_IMPORTS)
    if(NOT "${imports}" STREQUAL "${EXPECTED_IMPORTS}")
        message(FATAL_ERROR "The program imports:\n  ${imports}\nbut only these are expected:\n  ${EXPECTED_IMPORTS}")
    endif()
elseif(DEFINED MINIMUM_COUNT)
    if(count LESS MINIMUM_COUNT)
        message(FATAL_ERROR "The program imports ${count} functions (${imports}); expected at least ${MINIMUM_COUNT}")
    endif()
else()
    message(FATAL_ERROR "EXPECTED_IMPORTS or MINIMUM_COUNT is required")
endif()
message(STATUS "program imports (${count}): ${imports}")
