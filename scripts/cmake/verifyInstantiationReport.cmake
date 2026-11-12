if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED EXPECTED_FILE)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE and EXPECTED_FILE are required")
endif()

get_filename_component(directory "${INPUT_FILE}" DIRECTORY)
get_filename_component(name "${INPUT_FILE}" NAME)
file(READ "${EXPECTED_FILE}" expected)
foreach(level IN ITEMS -O0 -O2)
    foreach(run RANGE 1 2)
        execute_process(
                COMMAND "${JOYEER_EXECUTABLE}" --no-stdlib --report-instantiations "${level}" "${name}"
                WORKING_DIRECTORY "${directory}"
                RESULT_VARIABLE result
                OUTPUT_VARIABLE output
                ERROR_VARIABLE error
        )
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Instantiation report failed:\n${output}${error}")
        endif()
        if(NOT output STREQUAL expected)
            message(FATAL_ERROR "Unexpected instantiation report (${level}, run ${run}):\n${output}\nExpected:\n${expected}")
        endif()
    endforeach()
endforeach()
