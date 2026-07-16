if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED ARTIFACT_KIND)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, OUTPUT_FILE, and ARTIFACT_KIND are required")
endif()

set(artifact_directory "${OUTPUT_FILE}")
set(debug_argument -g0)
if(ARTIFACT_KIND STREQUAL "PDB")
    get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
    get_filename_component(output_name "${OUTPUT_FILE}" NAME)
    if(output_name MATCHES "^(.+)\\.[^.]+$")
        set(pdb_name "${CMAKE_MATCH_1}.pdb")
    else()
        set(pdb_name "${output_name}.pdb")
    endif()
    set(artifact_directory "${output_directory}/${pdb_name}")
    set(debug_argument -gcodeview)
elseif(NOT ARTIFACT_KIND STREQUAL "OUTPUT")
    message(FATAL_ERROR "ARTIFACT_KIND must be OUTPUT or PDB")
endif()

file(REMOVE "${OUTPUT_FILE}")
file(REMOVE_RECURSE "${artifact_directory}")
file(MAKE_DIRECTORY "${artifact_directory}")

execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 "${debug_argument}" -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
set(combined "${compiler_output}${compiler_error}")

if(compiler_result EQUAL 0)
    file(REMOVE "${OUTPUT_FILE}")
    file(REMOVE_RECURSE "${artifact_directory}")
    message(FATAL_ERROR "Native compilation unexpectedly overwrote an artifact directory")
endif()
if(NOT IS_DIRECTORY "${artifact_directory}")
    file(REMOVE "${OUTPUT_FILE}")
    file(REMOVE_RECURSE "${artifact_directory}")
    message(FATAL_ERROR "Native compilation removed the protected artifact directory")
endif()
string(FIND "${combined}" "linker.file-error" diagnostic_at)
if(diagnostic_at EQUAL -1)
    file(REMOVE "${OUTPUT_FILE}")
    file(REMOVE_RECURSE "${artifact_directory}")
    message(FATAL_ERROR "Native compilation failed without linker.file-error:\n${combined}")
endif()

file(REMOVE "${OUTPUT_FILE}")
file(REMOVE_RECURSE "${artifact_directory}")
message(STATUS "Native linker preserved the protected ${ARTIFACT_KIND} directory")
