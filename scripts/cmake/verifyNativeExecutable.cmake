if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED EXPECTED_OUTPUT)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, and EXPECTED_OUTPUT are required")
endif()

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
get_filename_component(output_name "${OUTPUT_FILE}" NAME)
if(output_name MATCHES "^(.+)\\.[^.]+$")
    set(pdb_name "${CMAKE_MATCH_1}.pdb")
else()
    set(pdb_name "${output_name}.pdb")
endif()
if(output_directory)
    set(pdb_file "${output_directory}/${pdb_name}")
else()
    set(pdb_file "${pdb_name}")
endif()
set(dsym_directory "${OUTPUT_FILE}.dSYM")

function(cleanup_native_artifacts)
    file(REMOVE "${OUTPUT_FILE}" "${pdb_file}")
    file(REMOVE_RECURSE "${dsym_directory}")
endfunction()

function(fail_native_validation message_text)
    cleanup_native_artifacts()
    message(FATAL_ERROR "${message_text}")
endfunction()

cleanup_native_artifacts()

if(EXPECTED_DEBUG_PATTERNS AND NOT LLVM_READOBJ)
    fail_native_validation("LLVM_READOBJ is required to inspect native debug sections")
endif()
if(EXPECTED_PDB_PATTERNS AND NOT LLVM_PDBUTIL)
    fail_native_validation("LLVM_PDBUTIL is required to inspect PDB line records")
endif()

set(compiler_command "${JOYEER_EXECUTABLE}" --lang=v0.1)
if(DEFINED COMPILER_ARGS)
    list(APPEND compiler_command ${COMPILER_ARGS})
endif()
list(APPEND compiler_command -o "${OUTPUT_FILE}" "${INPUT_FILE}")

execute_process(
        COMMAND ${compiler_command}
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
if(NOT compiler_result EQUAL 0)
    fail_native_validation(
            "Native compilation failed (${compiler_result}):\n${compiler_output}${compiler_error}")
endif()
if(NOT EXISTS "${OUTPUT_FILE}")
    fail_native_validation("Joyeer succeeded without producing ${OUTPUT_FILE}")
endif()


if(EXPECT_PDB)
    if(NOT EXISTS "${pdb_file}")
        fail_native_validation("Debug compilation did not produce ${pdb_file}")
    endif()
    file(SIZE "${pdb_file}" pdb_size)
    if(pdb_size EQUAL 0)
        fail_native_validation("Debug compilation produced an empty PDB")
    endif()
endif()

set(debug_target "${OUTPUT_FILE}")
if(EXPECT_DSYM)
    if(NOT IS_DIRECTORY "${dsym_directory}")
        fail_native_validation("Debug compilation did not produce ${dsym_directory}")
    endif()
    get_filename_component(output_name "${OUTPUT_FILE}" NAME)
    set(debug_target "${dsym_directory}/Contents/Resources/DWARF/${output_name}")
endif()

if(LLVM_READOBJ AND EXPECTED_DEBUG_PATTERNS)
    set(readobj_arguments --sections)
    if(READOBJ_COFF_DEBUG_DIRECTORY)
        list(APPEND readobj_arguments --coff-debug-directory)
    endif()
    execute_process(
            COMMAND "${LLVM_READOBJ}" ${readobj_arguments} "${debug_target}"
            RESULT_VARIABLE readobj_result
            OUTPUT_VARIABLE readobj_output
            ERROR_VARIABLE readobj_error
    )
    set(readobj_combined "${readobj_output}${readobj_error}")
    if(NOT readobj_result EQUAL 0)
        fail_native_validation("Cannot inspect native debug artifact:\n${readobj_combined}")
    endif()
    foreach(expected_pattern IN LISTS EXPECTED_DEBUG_PATTERNS)
        string(FIND "${readobj_combined}" "${expected_pattern}" pattern_at)
        if(pattern_at EQUAL -1)
                fail_native_validation(
                    "Native debug artifact is missing '${expected_pattern}':\n${readobj_combined}")
        endif()
    endforeach()
endif()

if(LLVM_PDBUTIL AND EXPECT_PDB AND EXPECTED_PDB_PATTERNS)
    execute_process(
            COMMAND "${LLVM_PDBUTIL}" dump -modules -files -l "${pdb_file}"
            RESULT_VARIABLE pdbutil_result
            OUTPUT_VARIABLE pdbutil_output
            ERROR_VARIABLE pdbutil_error
    )
    set(pdbutil_combined "${pdbutil_output}${pdbutil_error}")
    if(NOT pdbutil_result EQUAL 0)
        fail_native_validation("Cannot inspect PDB:\n${pdbutil_combined}")
    endif()
    foreach(expected_pattern IN LISTS EXPECTED_PDB_PATTERNS)
        string(FIND "${pdbutil_combined}" "${expected_pattern}" pattern_at)
        if(pattern_at EQUAL -1)
            fail_native_validation("PDB is missing '${expected_pattern}':\n${pdbutil_combined}")
        endif()
    endforeach()
endif()

if(EXPECT_NO_DEBUG_ARTIFACT AND (EXISTS "${pdb_file}" OR IS_DIRECTORY "${dsym_directory}"))
    fail_native_validation("Non-debug compilation unexpectedly produced a debug artifact")
endif()

set(program_working_directory_arguments)
if(DEFINED PROGRAM_WORKING_DIRECTORY)
    if(NOT IS_DIRECTORY "${PROGRAM_WORKING_DIRECTORY}")
    fail_native_validation(
        "PROGRAM_WORKING_DIRECTORY does not exist: ${PROGRAM_WORKING_DIRECTORY}")
    endif()
    list(APPEND program_working_directory_arguments
        WORKING_DIRECTORY "${PROGRAM_WORKING_DIRECTORY}")
endif()

execute_process(
    COMMAND "${OUTPUT_FILE}"
    ${program_working_directory_arguments}
        RESULT_VARIABLE program_result
        OUTPUT_VARIABLE program_output
        ERROR_VARIABLE program_error
)
cleanup_native_artifacts()

if(NOT program_result EQUAL 0)
    message(FATAL_ERROR
            "Native program failed (${program_result}):\n${program_output}${program_error}")
endif()

string(REPLACE "\r\n" "\n" normalized_output "${program_output}")
if(NOT normalized_output STREQUAL EXPECTED_OUTPUT)
    message(FATAL_ERROR
            "Native output mismatch:\nExpected:\n${EXPECTED_OUTPUT}\nActual:\n${normalized_output}")
endif()

message(STATUS "Native executable produced expected output")
