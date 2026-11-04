cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/helpers.cmake")
set(package "${work}/concurrent project")
jp_copy_project(single "${package}")
set(native_temp "${work}/native temporary files")
file(MAKE_DIRECTORY "${native_temp}")
file(WRITE "${native_temp}/sentinel" "unrelated temporary input")
set(ENV{TMPDIR} "${native_temp}")
set(ENV{TMP} "${native_temp}")
set(ENV{TEMP} "${native_temp}")

set(retained "${package}/target/debug/sample.app/build-1")
file(MAKE_DIRECTORY "${retained}")
file(WRITE "${retained}/sentinel" "preserved generation")

# execute_process launches COMMAND entries concurrently as a pipeline. Builds
# do not read stdin or write stdout, so this does not alter their stream policy.
foreach(round RANGE 1 2)
    execute_process(
        COMMAND "${FERN_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/project.toml"
        COMMAND "${FERN_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/project.toml"
        COMMAND "${FERN_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/project.toml"
        COMMAND "${FERN_EXECUTABLE}" build --compiler "${JOYEER_EXECUTABLE}"
            --manifest-path "${package}/project.toml"
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
file(GLOB generations LIST_DIRECTORIES true "${package}/target/debug/sample.app/build-*")
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
set(commands)
foreach(index RANGE 1 4)
    set(distinct "${work}/distinct-${index}")
    jp_write_module("${distinct}" "distinct.app${index}" bin app "'main.joyeer'" "")
    file(WRITE "${distinct}/main.joyeer" "func main() { print(value: ${index}) }\n")
    list(APPEND commands COMMAND "${FERN_EXECUTABLE}" build
        --compiler "${JOYEER_EXECUTABLE}" --manifest-path "${distinct}/project.toml")
endforeach()
execute_process(${commands}
    WORKING_DIRECTORY "${outside}"
    RESULTS_VARIABLE results OUTPUT_VARIABLE jp_out ERROR_VARIABLE jp_err TIMEOUT 60)
if(NOT results STREQUAL "0;0;0;0" OR NOT jp_out STREQUAL "")
    jp_fail("Concurrent distinct-source builders did not all succeed: ${results}")
endif()
foreach(index RANGE 1 4)
    set(artifact "${work}/distinct-${index}/target/debug/distinct.app${index}/build-1/app${EXECUTABLE_SUFFIX}")
    execute_process(COMMAND "${artifact}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT status STREQUAL "0" OR NOT output STREQUAL "${index}\n" OR NOT error STREQUAL "")
        message(FATAL_ERROR "Concurrent compilation mixed source outputs (${status}):\n${output}\n${error}")
    endif()
endforeach()
file(GLOB remaining RELATIVE "${native_temp}" "${native_temp}/*")
file(READ "${native_temp}/sentinel" sentinel)
if(NOT remaining STREQUAL "sentinel" OR NOT sentinel STREQUAL "unrelated temporary input")
    message(FATAL_ERROR "Native compilation leaked temporary files or changed unrelated input: ${remaining}")
endif()
message(STATUS "fern concurrent generations, distinct native outputs and temporary cleanup passed")
