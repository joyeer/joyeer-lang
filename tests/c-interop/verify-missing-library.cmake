execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}"
        --config "${BUILD_CONFIG}" --target CInteropMissingLibrary
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(status EQUAL 0)
    message(FATAL_ERROR "Linking without the native provider unexpectedly succeeded")
endif()
if(NOT "${output}${errors}" MATCHES "c_fixture_identity")
    message(FATAL_ERROR "Missing-provider test failed for an unrelated reason:\n${output}\n${errors}")
endif()
