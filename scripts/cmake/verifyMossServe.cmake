# Runs `moss serve` as a real process with stdin from a file and checks the
# framed replies. Required: MOSS_EXECUTABLE, FIXTURE_DIR (tools/moss/tests).
# Windows stdout turns LF into CRLF, so output is normalized before comparing.
if(NOT DEFINED MOSS_EXECUTABLE OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "MOSS_EXECUTABLE and FIXTURE_DIR are required")
endif()
set(class_dir "${FIXTURE_DIR}/fixtures/jvm")

function(run_serve input_file out_stdout out_stderr)
    execute_process(
        COMMAND "${MOSS_EXECUTABLE}" serve
        INPUT_FILE "${input_file}"
        WORKING_DIRECTORY "${class_dir}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error
        TIMEOUT 60)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "moss serve exited with ${status}:\n${standard_output}${standard_error}")
    endif()
    string(REPLACE "\r\n" "\n" standard_output "${standard_output}")
    set(${out_stdout} "${standard_output}" PARENT_SCOPE)
    set(${out_stderr} "${standard_error}" PARENT_SCOPE)
endfunction()

# Whole-class disassembly must equal the checked-in javap golden byte for byte,
# preceded by one header that counts its lines.
file(READ "${class_dir}/Example8.javap.txt" golden)
string(REGEX MATCHALL "\n" golden_newlines "${golden}")
list(LENGTH golden_newlines golden_lines)
set(golden_frame "moss: ok ${golden_lines}\n${golden}")

run_serve("${FIXTURE_DIR}/serve/session.in" session_output session_error)
if(NOT session_error STREQUAL "")
    message(FATAL_ERROR "moss serve wrote to stderr:\n${session_error}")
endif()
set(head "${golden_frame}moss: error 1\ncli.usage [arguments]: unknown command; use moss --help\n")
set(pattern "^(.*)moss: error 1\ninput\\.inspect-failed \\[input\\]: cannot acquire input 'Nope\\.class' \\(native code [0-9]+\\)\n(.*)$")
if(NOT session_output MATCHES "${pattern}")
    message(FATAL_ERROR "Unexpected session output:\n${session_output}")
endif()
set(before "${CMAKE_MATCH_1}")
set(after "${CMAKE_MATCH_2}")
if(NOT before STREQUAL head)
    message(FATAL_ERROR "Replies before the missing-input error differ.\nExpected:\n${head}\nActual:\n${before}")
endif()
# The second query reuses the cached class; the session ends at quit, so the
# trailing request is never answered.
set(tail "${golden_frame}moss: ok 1\nbye\n")
if(NOT after STREQUAL tail)
    message(FATAL_ERROR "Replies after the missing-input error differ.\nExpected:\n${tail}\nActual:\n${after}")
endif()

# End of input without quit, and a final request with no newline, still ends
# cleanly after answering it.
run_serve("${FIXTURE_DIR}/serve/eof.in" eof_output eof_error)
if(NOT eof_error STREQUAL "")
    message(FATAL_ERROR "moss serve wrote to stderr at end of input:\n${eof_error}")
endif()
if(NOT eof_output MATCHES "^moss: ok [0-9]+\nCompiled from \"Example\\.java\"\n")
    message(FATAL_ERROR "Final unterminated request was not answered:\n${eof_output}")
endif()
message(STATUS "moss serve produced the expected framed replies")
