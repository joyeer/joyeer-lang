cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/helpers.cmake")
set(package "${work}/concurrent project")
jp_copy_project(single "${package}")

set(retained "${package}/target/debug/sample/build-1")
file(MAKE_DIRECTORY "${retained}")
file(WRITE "${retained}/sentinel" "preserved generation")

# execute_process launches COMMAND entries concurrently as a pipeline. Builds
# do not read stdin or write stdout, so this does not alter their stream policy.
foreach(round RANGE 1 2)
    execute_process(
        COMMAND "${JOYPM_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/joyeer.toml"
        COMMAND "${JOYPM_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/joyeer.toml"
        COMMAND "${JOYPM_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/joyeer.toml"
        COMMAND "${JOYPM_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/joyeer.toml"
        WORKING_DIRECTORY "${outside}"
        RESULTS_VARIABLE results OUTPUT_VARIABLE jp_out ERROR_VARIABLE jp_err
        TIMEOUT 60)
    if(NOT results STREQUAL "0;0;0;0" OR NOT jp_out STREQUAL "")
        jp_fail("Concurrent builders did not all succeed: ${results}")
    endif()
    jp_no_runtime_error()
    math(EXPR expected "${round} * 4")
    jp_artifacts("${package}" debug sample "${expected}")
endforeach()
file(READ "${retained}/sentinel" sentinel)
if(NOT sentinel STREQUAL "preserved generation" OR
        EXISTS "${retained}/sample${EXECUTABLE_SUFFIX}")
    jp_fail("Concurrent builders populated or changed a collided generation")
endif()
file(GLOB generations LIST_DIRECTORIES true "${package}/target/debug/sample/build-*")
list(LENGTH generations count)
if(NOT count EQUAL 9)
    jp_fail("Concurrent allocation left unexpected or incomplete generations: ${generations}")
endif()
foreach(artifact IN LISTS jp_artifact_list)
    execute_process(COMMAND "${artifact}" WORKING_DIRECTORY "${package}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT status STREQUAL "0" OR
            NOT output STREQUAL "fixture executed\npackage cwd\n" OR NOT error STREQUAL "")
        message(FATAL_ERROR "Concurrent artifact failed (${status}):\n${output}\n${error}")
    endif()
endforeach()
message(STATUS "joypm eight concurrent fresh generations and retained collision acceptance passed")
