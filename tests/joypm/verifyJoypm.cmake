cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/helpers.cmake")
set(package "${work}/single project")
set(multi "${work}/multi project")
jp_copy_project(single "${package}")
jp_copy_project(multi "${multi}")

# Exercise caller-relative manifest/source coordinates, not just absolute paths.
jp_call(0 "${outside}" check --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "../single project/joyeer.toml")
jp_no_outputs("${package}")
jp_call(0 "${outside}" check --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${multi}/joyeer.toml")
jp_no_outputs("${multi}")

jp_call(0 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "../single project/joyeer.toml" --verbose)
jp_contains("${jp_err}" "-O0")
jp_contains("${jp_err}" "-gfull")
jp_artifacts("${package}" debug sample 1)
list(GET jp_artifact_list 0 first)
file(SHA256 "${first}" first_hash)
execute_process(COMMAND "${first}" WORKING_DIRECTORY "${package}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
string(REPLACE "\r\n" "\n" output "${output}")
if(NOT status STREQUAL "0" OR
   NOT output STREQUAL "fixture executed\npackage cwd\n" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Built single-source artifact failed (${status}):\n${output}\n${error}")
endif()

jp_call(0 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${package}/joyeer.toml")
jp_artifacts("${package}" debug sample 2)
file(SHA256 "${first}" second_hash)
if(NOT first_hash STREQUAL second_hash)
    jp_fail("A later generation overwrote an earlier successful artifact")
endif()
jp_call(0 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${package}/joyeer.toml" --release --verbose)
jp_contains("${jp_err}" "-O2")
jp_contains("${jp_err}" "-g0")
jp_artifacts("${package}" release sample 1)
list(GET jp_artifact_list 0 release)
get_filename_component(release_dir "${release}" DIRECTORY)
if(EXISTS "${release_dir}/sample.pdb" OR EXISTS "${release}.dSYM")
    jp_fail("Release -g0 unexpectedly produced platform debug artifacts")
endif()

jp_call(0 "${outside}" run --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${multi}/joyeer.toml")
jp_contains("${jp_out}" "multi executed\n42\n")
jp_artifacts("${multi}" debug multi 1)
jp_forward(run "${package}" ascii)
jp_artifacts("${package}" debug sample 3)
jp_call(1 "${outside}" run --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${package}/joyeer.toml" -- fail)
jp_contains("${jp_out}" "fixture executed\npackage cwd\n")
jp_contains("${jp_err}" "23")
jp_artifacts("${package}" debug sample 4)

# A compiler error must never return/run an old executable, even with four
# successful prior generations present. Both compilation and launch errors are 1.
configure_file("${FIXTURE_DIR}/invalid.joyeer" "${package}/main.joyeer" COPYONLY)
jp_call(1 "${outside}" run --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${package}/joyeer.toml")
jp_absent("${jp_out}" "fixture executed")
if(jp_err STREQUAL "")
    jp_fail("Failed rebuild must report a diagnostic")
endif()
jp_artifacts("${package}" debug sample 4)
file(SHA256 "${first}" final_hash)
if(NOT first_hash STREQUAL final_hash)
    jp_fail("Failed rebuild changed an old executable")
endif()
jp_call(1 "${outside}" check --compiler "${work}/missing compiler${EXECUTABLE_SUFFIX}"
    --manifest-path "${multi}/joyeer.toml")

# A compiler substitute that exits successfully without an artifact must not
# cause joypm to launch a prior generation or claim a successful build.
set(no_output "${work}/successful compiler without artifact")
jp_copy_project(single "${no_output}")
jp_call(1 "${outside}" run --compiler "${COMPILER_NO_OUTPUT}"
    --manifest-path "${no_output}/joyeer.toml")
jp_absent("${jp_out}" "fixture executed")
jp_artifacts("${no_output}" debug sample 0)
if(jp_err STREQUAL "")
    jp_fail("Missing expected artifact was not diagnosed")
endif()

# Explicit selection and ambiguity are CLI errors, not failed builds.
set(selection "${work}/selection")
jp_copy_project(single "${selection}")
file(READ "${selection}/joyeer.toml" manifest)
file(APPEND "${selection}/joyeer.toml"
    "\n[[targets]]\nname = 'other'\nkind = 'bin'\nmodule = 'sample.app'\n")
jp_call(2 "${outside}" run --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${selection}/joyeer.toml")
jp_no_outputs("${selection}")
jp_call(2 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${selection}/joyeer.toml" --target missing)
jp_no_outputs("${selection}")
jp_call(0 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${selection}/joyeer.toml" --target other)
jp_artifacts("${selection}" debug other 1)
jp_artifacts("${selection}" debug sample 0)
jp_call(0 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${selection}/joyeer.toml")
jp_artifacts("${selection}" debug other 2)
jp_artifacts("${selection}" debug sample 1)
jp_call(0 "${outside}" run --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${selection}/joyeer.toml" --target other)
jp_contains("${jp_out}" "fixture executed\npackage cwd\n")
jp_artifacts("${selection}" debug other 3)

# Bounded generation allocation: do not reuse, remove or populate a collision.
set(collision "${work}/collision retry")
jp_copy_project(single "${collision}")
file(MAKE_DIRECTORY "${collision}/target/debug/sample/build-1")
file(WRITE "${collision}/target/debug/sample/build-1/sentinel" "preserved")
jp_call(0 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${collision}/joyeer.toml")
jp_artifacts("${collision}" debug sample 1)
if(EXISTS "${collision}/target/debug/sample/build-1/sample${EXECUTABLE_SUFFIX}")
    jp_fail("Collision generation was populated")
endif()
file(READ "${collision}/target/debug/sample/build-1/sentinel" sentinel)
if(NOT sentinel STREQUAL "preserved")
    jp_fail("Collision generation was altered")
endif()
set(exhausted "${work}/collision exhaustion")
jp_copy_project(single "${exhausted}")
foreach(id RANGE 1 128)
    file(MAKE_DIRECTORY "${exhausted}/target/debug/sample/build-${id}")
    file(WRITE "${exhausted}/target/debug/sample/build-${id}/sentinel" "${id}")
endforeach()
set(exhaustion_cwd "${work}/exhaustion caller")
file(MAKE_DIRECTORY "${exhaustion_cwd}")
jp_call(1 "${exhaustion_cwd}" build --compiler "${COMPILER_SPY}"
    --manifest-path "${exhausted}/joyeer.toml")
if(EXISTS "${exhaustion_cwd}/compiler-record.txt" OR
   EXISTS "${exhausted}/target/debug/sample/build-129")
    jp_fail("Exhausted 128 generation attempts still invoked the compiler or allocated output")
endif()
foreach(id RANGE 1 128)
    file(READ "${exhausted}/target/debug/sample/build-${id}/sentinel" sentinel)
    if(NOT sentinel STREQUAL "${id}")
        jp_fail("Exhaustion altered an existing generation")
    endif()
endforeach()

set(blocked "${work}/blocked target directory")
jp_copy_project(single "${blocked}")
file(WRITE "${blocked}/target" "ordinary file")
jp_call(1 "${outside}" build --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${blocked}/joyeer.toml")
file(READ "${blocked}/target" sentinel)
if(NOT sentinel STREQUAL "ordinary file")
    jp_fail("Build replaced a conflicting parent file")
endif()
message(STATUS "joypm native workflow/profile/selection/generation/stale-artifact acceptance passed")