cmake_minimum_required(VERSION 3.20)

# Differential check: compile one fixture at every requested optimization level,
# run each executable, and require identical exit status, stdout, and stderr.
# A trapping program is acceptable as long as every level traps the same way.
#
# Required: JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_PREFIX
# Optional: LEVELS (default 0;1;2;3), PROGRAM_TIMEOUT (seconds, default 30)

if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_PREFIX)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, and OUTPUT_PREFIX are required")
endif()
if(NOT DEFINED LEVELS)
    set(LEVELS 0 1 2 3)
endif()
if(NOT DEFINED PROGRAM_TIMEOUT)
    set(PROGRAM_TIMEOUT 30)
endif()

set(baseline_level "")
foreach(level IN LISTS LEVELS)
    set(output_file "${OUTPUT_PREFIX}-O${level}${CMAKE_EXECUTABLE_SUFFIX}")
    file(REMOVE "${output_file}")

    execute_process(
            COMMAND "${JOYEER_EXECUTABLE}" -O${level} -o "${output_file}" -- "${INPUT_FILE}"
            RESULT_VARIABLE compiler_result
            OUTPUT_VARIABLE compiler_output
            ERROR_VARIABLE compiler_error
            ENCODING UTF-8
    )
    if(NOT compiler_result EQUAL 0 OR NOT EXISTS "${output_file}")
        message(FATAL_ERROR
                "Compilation at -O${level} failed (${compiler_result}):\n${compiler_output}${compiler_error}")
    endif()

    execute_process(
            COMMAND "${output_file}"
            RESULT_VARIABLE program_result
            OUTPUT_VARIABLE program_output
            ERROR_VARIABLE program_error
            TIMEOUT ${PROGRAM_TIMEOUT}
            ENCODING UTF-8
    )
    file(REMOVE "${output_file}" "${OUTPUT_PREFIX}-O${level}.pdb")
    file(REMOVE_RECURSE "${output_file}.dSYM")

    string(REPLACE "\r\n" "\n" program_output "${program_output}")
    string(REPLACE "\r\n" "\n" program_error "${program_error}")

    if(baseline_level STREQUAL "")
        set(baseline_level "${level}")
        set(baseline_result "${program_result}")
        set(baseline_output "${program_output}")
        set(baseline_error "${program_error}")
        continue()
    endif()

    if(NOT program_result STREQUAL baseline_result)
        message(FATAL_ERROR
                "Exit status differs: -O${baseline_level} gave ${baseline_result}, "
                "-O${level} gave ${program_result}\n${INPUT_FILE}")
    endif()
    if(NOT program_output STREQUAL baseline_output)
        message(FATAL_ERROR
                "stdout differs between -O${baseline_level} and -O${level} for ${INPUT_FILE}\n"
                "-O${baseline_level}:\n${baseline_output}\n-O${level}:\n${program_output}")
    endif()
    if(NOT program_error STREQUAL baseline_error)
        message(FATAL_ERROR
                "stderr differs between -O${baseline_level} and -O${level} for ${INPUT_FILE}\n"
                "-O${baseline_level}:\n${baseline_error}\n-O${level}:\n${program_error}")
    endif()
endforeach()

message(STATUS "Optimization levels agree (exit ${baseline_result})")
