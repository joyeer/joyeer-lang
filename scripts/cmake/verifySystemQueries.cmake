cmake_minimum_required(VERSION 3.20)

execute_process(
    COMMAND "${JOYEER_EXECUTABLE}" "${OPTIMIZATION}" -g0 --module-name host.queries
        --runtime-module host.queries -o "${OUTPUT_FILE}" "${INPUT_FILE}"
    RESULT_VARIABLE compiled OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT compiled STREQUAL "0")
    message(FATAL_ERROR "Host query fixture did not compile (${compiled}):\n${stdout}${stderr}")
endif()

get_filename_component(test_directory "${INPUT_FILE}" DIRECTORY)
string(REPEAT "123" 130 long_value)
set(environment_unsets --unset=JOYEER_QUERY_TEST_MISSING)
if(NOT CMAKE_HOST_WIN32)
    # Windows resolves the lowercase name to the same entry as the uppercase assignment.
    list(APPEND environment_unsets --unset=joyeer_query_test_long)
endif()
string(TIMESTAMP before "%s" UTC)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${environment_unsets}
        "JOYEER_QUERY_TEST_EMPTY=" "JOYEER_QUERY_TEST_LONG=${long_value}" "PWD=/not-the-current-directory"
        "${OUTPUT_FILE}"
    WORKING_DIRECTORY "${test_directory}"
    RESULT_VARIABLE status OUTPUT_VARIABLE wall ERROR_VARIABLE stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 20)
string(TIMESTAMP after "%s" UTC)
file(REMOVE "${OUTPUT_FILE}")
math(EXPR before "${before} * 1000")
math(EXPR after "(${after} + 1) * 1000")
if(NOT status STREQUAL "0" OR NOT stderr STREQUAL "" OR NOT wall MATCHES "^[0-9]+$"
    OR wall LESS before OR wall GREATER_EQUAL after)
    message(FATAL_ERROR "Host query fixture returned ${status}, stdout '${wall}', stderr '${stderr}'; wall time expected in [${before}, ${after})")
endif()
