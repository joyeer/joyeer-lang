if(NOT DEFINED BUILD_DIR OR NOT DEFINED STAGING_DIR OR
   NOT DEFINED INPUT_FILE OR NOT DEFINED EXPECTED_OUTPUT)
    message(FATAL_ERROR
            "BUILD_DIR, STAGING_DIR, INPUT_FILE, and EXPECTED_OUTPUT are required")
endif()

function(fail_installed_validation message_text)
    file(REMOVE_RECURSE "${STAGING_DIR}")
    message(FATAL_ERROR "${message_text}")
endfunction()

file(REMOVE_RECURSE "${STAGING_DIR}")
execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGING_DIR}"
        RESULT_VARIABLE install_result
        OUTPUT_VARIABLE install_output
        ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    fail_installed_validation(
            "Compiler installation failed (${install_result}):\n${install_output}${install_error}")
endif()

set(compiler "${STAGING_DIR}/joyeer${EXECUTABLE_SUFFIX}")
set(output "${STAGING_DIR}/installed-native${EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${compiler}")
    fail_installed_validation("Installed compiler does not exist: ${compiler}")
endif()

execute_process(
        COMMAND "${compiler}" -o "${output}" "${INPUT_FILE}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
if(NOT compiler_result EQUAL 0 OR NOT EXISTS "${output}")
    fail_installed_validation(
            "Installed compiler failed (${compiler_result}):\n${compiler_output}${compiler_error}")
endif()

execute_process(
        COMMAND "${output}"
        RESULT_VARIABLE program_result
        OUTPUT_VARIABLE program_output
        ERROR_VARIABLE program_error
)
if(NOT program_result EQUAL 0)
    fail_installed_validation(
            "Installed native program failed (${program_result}):\n${program_output}${program_error}")
endif()

string(REPLACE "\r\n" "\n" normalized_output "${program_output}")
if(NOT normalized_output STREQUAL EXPECTED_OUTPUT)
    fail_installed_validation(
            "Installed native output mismatch:\nExpected:\n${EXPECTED_OUTPUT}\nActual:\n${normalized_output}")
endif()

file(REMOVE_RECURSE "${STAGING_DIR}")
message(STATUS "Installed compiler produced and ran the expected native executable")