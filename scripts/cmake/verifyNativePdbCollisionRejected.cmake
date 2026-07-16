if(NOT DEFINED JOYEER_EXECUTABLE OR NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "JOYEER_EXECUTABLE, INPUT_FILE, and OUTPUT_FILE are required")
endif()

file(REMOVE "${OUTPUT_FILE}")
execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" --lang=v0.1 -gcodeview -o "${OUTPUT_FILE}" "${INPUT_FILE}"
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
)
set(combined "${compiler_output}${compiler_error}")

if(compiler_result EQUAL 0)
    file(REMOVE "${OUTPUT_FILE}")
    message(FATAL_ERROR "CodeView compilation unexpectedly accepted a .pdb executable path")
endif()
if(EXISTS "${OUTPUT_FILE}")
    file(REMOVE "${OUTPUT_FILE}")
    message(FATAL_ERROR "Rejected CodeView compilation left a colliding output artifact")
endif()
string(FIND "${combined}" "linker.file-error" diagnostic_at)
if(diagnostic_at EQUAL -1)
    message(FATAL_ERROR "CodeView collision failed without linker.file-error:\n${combined}")
endif()

message(STATUS "CodeView executable/PDB path collision was rejected")
