cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS JOYPM_EXECUTABLE JOYEER_EXECUTABLE FIXTURE_DIR TEST_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${JOYPM_EXECUTABLE}" OR IS_DIRECTORY "${JOYPM_EXECUTABLE}")
    message(FATAL_ERROR "Build the joypm ALL target before running acceptance tests")
endif()
if(NOT DEFINED EXECUTABLE_SUFFIX)
    set(EXECUTABLE_SUFFIX "")
endif()

# Keep each invocation independent, even after a failed previous CTest run.
# Never remove a project generation or an existing test directory.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(work "${TEST_ROOT}/run ${run_id}")
file(MAKE_DIRECTORY "${work}" "${work}/invocation outside package")
set(outside "${work}/invocation outside package")

function(jp_fail text)
    message(FATAL_ERROR "${text}\nstdout:\n${jp_out}\nstderr:\n${jp_err}")
endfunction()

function(jp_call expected cwd)
    if(NOT DEFINED jp_timeout)
        set(jp_timeout 60)
    endif()
    execute_process(
        COMMAND "${JOYPM_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${cwd}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        ENCODING UTF-8
        TIMEOUT "${jp_timeout}")
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r\n" "\n" error "${error}")
    if(NOT "${result}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "joypm returned '${result}', expected ${expected}; argv: ${ARGN}\n"
            "stdout:\n${output}\nstderr:\n${error}")
    endif()
    set(jp_out "${output}" PARENT_SCOPE)
    set(jp_err "${error}" PARENT_SCOPE)
endfunction()

function(jp_usage)
    jp_call(2 "${outside}" ${ARGN})
    if(NOT jp_out STREQUAL "" OR jp_err STREQUAL "")
        jp_fail("Usage errors must have empty stdout and nonempty stderr")
    endif()
endfunction()

function(jp_contains text needle)
    string(FIND "${text}" "${needle}" at)
    if(at LESS 0)
        jp_fail("Missing expected text '${needle}'")
    endif()
endfunction()

function(jp_absent text needle)
    string(FIND "${text}" "${needle}" at)
    if(NOT at LESS 0)
        jp_fail("Unexpected text '${needle}'")
    endif()
endfunction()

function(jp_no_runtime_error)
    jp_absent("${jp_out}${jp_err}" "Joyeer runtime error:")
    jp_absent("${jp_out}${jp_err}" "leaked allocation(s)")
    jp_absent("${jp_out}${jp_err}" "allocation leak")
endfunction()

function(jp_write_bytes path contents)
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef write_id)
    set(input "${path}.cmake-${write_id}")
    set(output "${path}.bytes-${write_id}")
    file(WRITE "${input}" "${contents}")
    execute_process(
        COMMAND "${INPUT_WRITER}" "${input}" "${output}"
        RESULT_VARIABLE result OUTPUT_VARIABLE writer_out ERROR_VARIABLE writer_err
        ENCODING UTF-8
        TIMEOUT 30)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Exact-byte input writer failed (${result}):\n${writer_out}\n${writer_err}")
    endif()
    string(LENGTH "${contents}" expected_size)
    string(SHA256 expected_hash "${contents}")
    file(SIZE "${output}" actual_size)
    file(SHA256 "${output}" actual_hash)
    if(NOT actual_size EQUAL expected_size OR NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Manifest fixture bytes changed: ${path}; expected ${expected_size}, got ${actual_size}")
    endif()
    configure_file("${output}" "${path}" COPYONLY)
    file(REMOVE "${input}" "${output}")
endfunction()

# Validate byte coordinates against the original input, without normalizing
# its UTF-8, escapes, CRLFs, or caller-supplied manifest path. ARGN optionally
# supplies an exact zero-based byte offset for targeted location regressions.
function(jp_manifest_diagnostic manifest contents)
    jp_no_runtime_error()
    if(NOT jp_out STREQUAL "")
        jp_fail("Manifest diagnostics must have empty stdout")
    endif()
    set(prefix "${manifest}:")
    string(FIND "${jp_err}" "${prefix}" at)
    if(NOT at EQUAL 0)
        jp_fail("Manifest diagnostic lost the original path '${manifest}'")
    endif()
    string(LENGTH "${prefix}" prefix_size)
    string(SUBSTRING "${jp_err}" "${prefix_size}" -1 location)
    if(NOT location MATCHES "^([1-9][0-9]*):([1-9][0-9]*): [^\n]+\n$")
        jp_fail("Manifest diagnostic must contain a source line and byte column")
    endif()
    set(line "${CMAKE_MATCH_1}")
    set(column "${CMAKE_MATCH_2}")
    set(line_start 0)
    set(current_line 1)
    while(current_line LESS line)
        string(SUBSTRING "${contents}" "${line_start}" -1 tail)
        string(FIND "${tail}" "\n" newline)
        if(newline LESS 0)
            jp_fail("Manifest diagnostic line ${line} is outside the original input")
        endif()
        math(EXPR line_start "${line_start} + ${newline} + 1")
        math(EXPR current_line "${current_line} + 1")
    endwhile()
    string(SUBSTRING "${contents}" "${line_start}" -1 tail)
    string(FIND "${tail}" "\n" line_size)
    if(line_size LESS 0)
        string(LENGTH "${tail}" line_size)
    endif()
    math(EXPR maximum_column "${line_size} + 1")
    if(column GREATER maximum_column)
        jp_fail("Manifest diagnostic column ${column} is outside its original source line")
    endif()
    math(EXPR byte_pos "${line_start} + ${column} - 1")
    if(ARGC GREATER 2 AND NOT byte_pos EQUAL ARGV2)
        jp_fail("Manifest diagnostic byte offset ${byte_pos}, expected ${ARGV2}")
    endif()
endfunction()

function(jp_reject_manifest name contents)
    set(package "${work}/${name}")
    set(cwd "${work}/caller ${name}")
    file(MAKE_DIRECTORY "${package}" "${cwd}")
    # The lexical /./ spelling is deliberate: diagnostics preserve caller
    # coordinates, rather than reporting a canonicalized filesystem path.
    set(manifest "${package}/./joyeer.toml")
    jp_write_bytes("${manifest}" "${contents}")
    jp_call(1 "${cwd}" check --compiler "${COMPILER_SPY}"
        --manifest-path "${manifest}")
    jp_manifest_diagnostic("${manifest}" "${contents}" ${ARGN})
    if(EXISTS "${cwd}/compiler-record.txt")
        jp_fail("Invalid manifest ${name} reached the compiler")
    endif()
    jp_no_outputs("${package}")
endfunction()

function(jp_no_outputs package)
    if(EXISTS "${package}/target")
        jp_fail("Validation/selection must not create a target directory: ${package}")
    endif()
endfunction()

function(jp_copy_project fixture destination)
    file(MAKE_DIRECTORY "${destination}")
    file(COPY "${FIXTURE_DIR}/${fixture}/" DESTINATION "${destination}")
    file(WRITE "${destination}/cwd-marker.txt" "package cwd")
endfunction()

function(jp_artifacts package profile target expected)
    # Output inspection only; compilation inputs are always explicit lists.
    file(GLOB artifacts LIST_DIRECTORIES false
        "${package}/target/${profile}/${target}/build-*/${target}${EXECUTABLE_SUFFIX}")
    list(LENGTH artifacts count)
    if(NOT count EQUAL expected)
        jp_fail("Expected ${expected} ${profile}/${target} artifacts, found ${count}: ${artifacts}")
    endif()
    foreach(artifact IN LISTS artifacts)
        file(SIZE "${artifact}" size)
        if(size EQUAL 0)
            jp_fail("Empty successful artifact: ${artifact}")
        endif()
    endforeach()
    set(jp_artifact_list "${artifacts}" PARENT_SCOPE)
endfunction()

function(jp_forward command package mode)
    execute_process(
        COMMAND "${FORWARDING_DRIVER}" "${JOYPM_EXECUTABLE}"
            "${JOYEER_EXECUTABLE}" "${package}/joyeer.toml" "${outside}"
            "${command}" "${mode}"
        WORKING_DIRECTORY "${outside}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        ENCODING UTF-8
        TIMEOUT 60)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Forwarding driver returned ${result}:\n${output}\n${error}")
    endif()
    set(expected "fixture executed\npackage cwd\n<>\n<with spaces>\n<quote\"slash\\>\n<$&*%literal;<>|>\n<--not-an-option>\n")
    if(mode STREQUAL "unicode")
        string(APPEND expected "<雪🦀>\n")
    else()
        string(APPEND expected "<ASCII>\n")
    endif()
    string(FIND "${output}" "${expected}" at)
    if(at LESS 0)
        message(FATAL_ERROR "Argument boundaries or package cwd changed:\n${output}\n${error}")
    endif()
    set(jp_out "${output}" PARENT_SCOPE)
    set(jp_err "${error}" PARENT_SCOPE)
endfunction()