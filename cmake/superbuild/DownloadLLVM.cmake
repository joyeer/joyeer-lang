if(NOT DEFINED JOYEER_DOWNLOAD_URL OR
   NOT DEFINED JOYEER_DOWNLOAD_ARCHIVE OR
   NOT DEFINED JOYEER_DOWNLOAD_SHA256 OR
   NOT DEFINED JOYEER_DOWNLOAD_SOURCE_DIR OR
   NOT DEFINED JOYEER_CURL_EXECUTABLE)
    message(FATAL_ERROR "LLVM download script is missing required parameters")
endif()

get_filename_component(_joyeer_download_directory
        "${JOYEER_DOWNLOAD_ARCHIVE}" DIRECTORY)
file(MAKE_DIRECTORY "${_joyeer_download_directory}")

execute_process(
        COMMAND "${JOYEER_CURL_EXECUTABLE}"
                --location
                --fail
                --show-error
                --http1.1
                --retry 8
                --retry-delay 2
                --retry-max-time 900
                --retry-all-errors
                --continue-at -
                --output "${JOYEER_DOWNLOAD_ARCHIVE}"
                "${JOYEER_DOWNLOAD_URL}"
        RESULT_VARIABLE _joyeer_download_result
        COMMAND_ECHO STDOUT
)
if(NOT _joyeer_download_result EQUAL 0)
    message(FATAL_ERROR
            "curl failed to download LLVM with exit code ${_joyeer_download_result}")
endif()

file(SHA256 "${JOYEER_DOWNLOAD_ARCHIVE}" _joyeer_download_actual_sha256)
if(NOT _joyeer_download_actual_sha256 STREQUAL "${JOYEER_DOWNLOAD_SHA256}")
    file(REMOVE "${JOYEER_DOWNLOAD_ARCHIVE}")
    message(FATAL_ERROR
            "LLVM archive SHA-256 mismatch: expected ${JOYEER_DOWNLOAD_SHA256}, "
            "got ${_joyeer_download_actual_sha256}")
endif()
message(STATUS "Verified LLVM archive SHA-256: ${_joyeer_download_actual_sha256}")

set(_joyeer_extract_directory "${JOYEER_DOWNLOAD_SOURCE_DIR}.extract")
file(REMOVE_RECURSE
        "${JOYEER_DOWNLOAD_SOURCE_DIR}"
        "${_joyeer_extract_directory}"
)
file(MAKE_DIRECTORY "${_joyeer_extract_directory}")
file(ARCHIVE_EXTRACT
        INPUT "${JOYEER_DOWNLOAD_ARCHIVE}"
        DESTINATION "${_joyeer_extract_directory}"
)

file(GLOB _joyeer_extracted_entries
        LIST_DIRECTORIES TRUE
        "${_joyeer_extract_directory}/*")
list(LENGTH _joyeer_extracted_entries _joyeer_extracted_entry_count)
if(NOT _joyeer_extracted_entry_count EQUAL 1 OR
   NOT IS_DIRECTORY "${_joyeer_extracted_entries}")
    file(REMOVE_RECURSE "${_joyeer_extract_directory}")
    message(FATAL_ERROR "LLVM archive did not contain exactly one source directory")
endif()

file(RENAME
        "${_joyeer_extracted_entries}"
        "${JOYEER_DOWNLOAD_SOURCE_DIR}"
)
file(REMOVE_RECURSE "${_joyeer_extract_directory}")
