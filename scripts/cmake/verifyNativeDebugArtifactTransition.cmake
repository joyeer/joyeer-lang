if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED ARTIFACT_KIND)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, and ARTIFACT_KIND are required")
endif()

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
get_filename_component(output_name "${OUTPUT_FILE}" NAME)
if(output_name MATCHES "^(.+)\\.[^.]+$")
    set(pdb_name "${CMAKE_MATCH_1}.pdb")
else()
    set(pdb_name "${output_name}.pdb")
endif()
set(pdb_file "${output_directory}/${pdb_name}")
set(dsym_directory "${OUTPUT_FILE}.dSYM")

function(cleanup_transition_artifacts)
    file(REMOVE "${OUTPUT_FILE}" "${pdb_file}")
    file(REMOVE_RECURSE "${dsym_directory}")
endfunction()

cleanup_transition_artifacts()
if(ARTIFACT_KIND STREQUAL "PDB")
    set(debug_argument -gcodeview)
    set(debug_artifact "${pdb_file}")
elseif(ARTIFACT_KIND STREQUAL "DSYM")
    set(debug_argument -gdwarf)
    set(debug_artifact "${dsym_directory}")
else()
    message(FATAL_ERROR "ARTIFACT_KIND must be PDB or DSYM")
endif()

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 "${debug_argument}" -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE debug_result
        OUTPUT_VARIABLE debug_output
        ERROR_VARIABLE debug_error
)
if(NOT debug_result EQUAL 0 OR NOT EXISTS "${debug_artifact}")
    cleanup_transition_artifacts()
    message(FATAL_ERROR
            "Initial debug compilation did not produce ${ARTIFACT_KIND}:\n${debug_output}${debug_error}")
endif()

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 -g0 -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE nodebug_result
        OUTPUT_VARIABLE nodebug_output
        ERROR_VARIABLE nodebug_error
)
if(NOT nodebug_result EQUAL 0)
    cleanup_transition_artifacts()
    message(FATAL_ERROR
            "No-debug rebuild failed:\n${nodebug_output}${nodebug_error}")
endif()
if(EXISTS "${debug_artifact}")
    cleanup_transition_artifacts()
    message(FATAL_ERROR
            "No-debug rebuild left stale ${ARTIFACT_KIND} artifact: ${debug_artifact}")
endif()
if(NOT EXISTS "${OUTPUT_FILE}")
    cleanup_transition_artifacts()
    message(FATAL_ERROR "No-debug rebuild produced no executable")
endif()

cleanup_transition_artifacts()
message(STATUS "No-debug rebuild removed the stale ${ARTIFACT_KIND} artifact")
