# Checks the symbols of the Joyeer runtime core object (docs/plan/runtime-in-joyeer.md).
# The object must define exactly the approved exports and may call out only to the
# approved imports, so a runtime function cannot silently gain a dependency (the
# runtime must not reach itself through code the compiler lowers to runtime calls).
#
# Inputs: NM, OBJECT, EXPECTED_DEFINED, ALLOWED_UNDEFINED (semicolon lists).
foreach(variable IN ITEMS NM OBJECT EXPECTED_DEFINED ALLOWED_UNDEFINED)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
if(NOT EXISTS "${OBJECT}")
    message(FATAL_ERROR "Missing runtime core object: ${OBJECT}")
endif()

function(joyeer_symbols option output)
    execute_process(COMMAND "${NM}" ${option} "${OBJECT}"
        RESULT_VARIABLE status OUTPUT_VARIABLE text ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${NM} ${option} failed: ${error}")
    endif()
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(names)
    foreach(line IN LISTS lines)
        if(line MATCHES "^[0-9a-fA-F ]*[A-Za-z] +([^ ]+)$")
            set(name "${CMAKE_MATCH_1}")
            if(APPLE)
                string(REGEX REPLACE "^_" "" name "${name}")
            endif()
            list(APPEND names "${name}")
        endif()
    endforeach()
    list(SORT names)
    set(${output} "${names}" PARENT_SCOPE)
endfunction()

joyeer_symbols("--extern-only;--defined-only" defined)
joyeer_symbols("--undefined-only" undefined)
list(SORT EXPECTED_DEFINED)

if(NOT "${defined}" STREQUAL "${EXPECTED_DEFINED}")
    message(FATAL_ERROR
        "The runtime core object defines:\n  ${defined}\nbut the approved exports are:\n  ${EXPECTED_DEFINED}")
endif()
foreach(name IN LISTS undefined)
    if(NOT name IN_LIST ALLOWED_UNDEFINED)
        message(FATAL_ERROR
            "The runtime core object imports '${name}', which is not approved.\n"
            "Allowed imports: ${ALLOWED_UNDEFINED}")
    endif()
endforeach()
message(STATUS "runtime core object: ${defined}; imports ${undefined}")
