# Compiles a Joyeer program and checks what the executable imports
# (decision 0011): a program that never calls the C library starts at its own entry
# point and imports only the operating system's functions, so the exact list is known;
# one that does keeps the C startup and imports far more.
#
# Inputs: JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, READOBJ (Windows) or NM (Linux),
# COMPILER_ARGS (list), and
# one of EXPECTED_IMPORTS (the exact list), ALLOWED_IMPORTS (a superset), FORBIDDEN_IMPORTS
# (names that must be absent) or
# MINIMUM_COUNT.
foreach(variable IN ITEMS JOYEER_EXECUTABLE INPUT_FILE OUTPUT_FILE)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
if(NOT DEFINED READOBJ AND NOT DEFINED NM)
    message(FATAL_ERROR "READOBJ (Windows) or NM (Linux) is required")
endif()
file(REMOVE "${OUTPUT_FILE}")
execute_process(COMMAND "${JOYEER_EXECUTABLE}" ${COMPILER_ARGS} -o "${OUTPUT_FILE}" -- "${INPUT_FILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT EXISTS "${OUTPUT_FILE}")
    message(FATAL_ERROR "Compiling ${INPUT_FILE} failed (${status}):\n${output}${error}")
endif()
if(DEFINED NM)
    # Linux: the dynamic symbols the executable needs, without their version suffix.
    execute_process(COMMAND "${NM}" -D --undefined-only "${OUTPUT_FILE}"
        RESULT_VARIABLE status OUTPUT_VARIABLE text ERROR_VARIABLE error)
    set(tool "${NM}")
else()
    execute_process(COMMAND "${READOBJ}" --coff-imports "${OUTPUT_FILE}"
        RESULT_VARIABLE status OUTPUT_VARIABLE text ERROR_VARIABLE error)
    set(tool "${READOBJ}")
endif()
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${tool} failed: ${error}")
endif()
string(REPLACE "\r" "" text "${text}")
string(REPLACE "\n" ";" lines "${text}")
set(imports)
foreach(line IN LISTS lines)
    if(DEFINED NM AND line MATCHES "^ *[Uuvw]? *([A-Za-z0-9_.]+)")
        list(APPEND imports "${CMAKE_MATCH_1}")
    elseif(NOT DEFINED NM AND line MATCHES "^ *Symbol: ([A-Za-z0-9_@?$.]+)")
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
elseif(DEFINED ALLOWED_IMPORTS)
    foreach(name IN LISTS imports)
        if(NOT name IN_LIST ALLOWED_IMPORTS)
            message(FATAL_ERROR "The program needs ${name}, which is not approved:\n  ${imports}\nAllowed: ${ALLOWED_IMPORTS}")
        endif()
    endforeach()
elseif(DEFINED FORBIDDEN_IMPORTS)
    foreach(name IN LISTS FORBIDDEN_IMPORTS)
        if(name IN_LIST imports)
            message(FATAL_ERROR "The program imports ${name}, which only the C runtime needs:\n  ${imports}")
        endif()
    endforeach()
elseif(DEFINED MINIMUM_COUNT)
    if(count LESS MINIMUM_COUNT)
        message(FATAL_ERROR "The program imports ${count} functions (${imports}); expected at least ${MINIMUM_COUNT}")
    endif()
else()
    message(FATAL_ERROR "EXPECTED_IMPORTS, FORBIDDEN_IMPORTS or MINIMUM_COUNT is required")
endif()
message(STATUS "program imports (${count}): ${imports}")
