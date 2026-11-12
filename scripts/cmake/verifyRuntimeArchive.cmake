# Checks that the runtime archive holds exactly the objects the Joyeer compiler
# produced from lib/Runtime/core/ and nothing else (docs/plan/0008-runtime-in-joyeer.md,
# stage 9): no object compiled from C, no stray member.
#
# Inputs: AR, ARCHIVE, EXPECTED_MEMBERS (semicolon list of file names).
foreach(variable IN ITEMS AR ARCHIVE EXPECTED_MEMBERS)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Missing runtime archive: ${ARCHIVE}")
endif()
execute_process(COMMAND "${AR}" t "${ARCHIVE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE text ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${AR} t failed: ${error}")
endif()
string(REPLACE "\r" "" text "${text}")
string(REPLACE "\n" ";" lines "${text}")
set(members)
foreach(line IN LISTS lines)
    if(NOT line STREQUAL "")
        get_filename_component(name "${line}" NAME)
        list(APPEND members "${name}")
    endif()
endforeach()
list(SORT members)
set(expected "${EXPECTED_MEMBERS}")
list(SORT expected)
if(NOT "${members}" STREQUAL "${expected}")
    message(FATAL_ERROR
        "The runtime archive holds:\n  ${members}\nbut only these Joyeer objects are allowed:\n  ${expected}")
endif()
message(STATUS "runtime archive members: ${members}")
