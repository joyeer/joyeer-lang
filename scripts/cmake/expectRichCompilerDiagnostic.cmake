if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED EXPECTED_HEADER OR NOT DEFINED EXPECTED_SOURCE)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, EXPECTED_HEADER, and EXPECTED_SOURCE are required")
endif()

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 "${INPUT_FILE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
)
if(result EQUAL 0)
    message(FATAL_ERROR "Compiler validation unexpectedly succeeded")
endif()

set(combined "${output}${error}")
foreach(expected IN ITEMS "${EXPECTED_HEADER}" "${EXPECTED_SOURCE}" "^")
    string(FIND "${combined}" "${expected}" location)
    if(location EQUAL -1)
        message(FATAL_ERROR
                "Compiler diagnostic is missing '${expected}':\n${combined}")
    endif()
endforeach()

foreach(optional_name IN ITEMS EXPECTED_HELP EXPECTED_FIX_IT)
    if(DEFINED ${optional_name})
        string(FIND "${combined}" "${${optional_name}}" location)
        if(location EQUAL -1)
            message(FATAL_ERROR
                    "Compiler diagnostic is missing '${${optional_name}}':\n${combined}")
        endif()
    endif()
endforeach()

message(STATUS "Observed rich source diagnostic: ${EXPECTED_HEADER}")
