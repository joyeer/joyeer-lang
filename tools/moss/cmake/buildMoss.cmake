# Stage the Moss workspace in the build tree (fern writes target/ next to the
# workspace), build moss.app with fern, and copy the executable to OUTPUT.
# Compression comes from the compiler's own standard library.
#
# Required: SOURCE_DIR WORKSPACE_DIR FERN COMPILER PROFILE
# OUTPUT. Optional: ACTION=build|test (default build).
foreach(name IN ITEMS SOURCE_DIR WORKSPACE_DIR FERN COMPILER PROFILE OUTPUT)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "buildMoss.cmake: ${name} is required")
    endif()
endforeach()
if(NOT DEFINED ACTION)
    set(ACTION build)
endif()

# A clean stage each time: fern keeps at most 128 generations per module and
# never cleans them, and stale sources must not survive a removed file.
file(REMOVE_RECURSE "${WORKSPACE_DIR}")
file(MAKE_DIRECTORY "${WORKSPACE_DIR}")
foreach(entry IN ITEMS src tests project.toml workspace.toml)
    file(COPY "${SOURCE_DIR}/${entry}" DESTINATION "${WORKSPACE_DIR}")
endforeach()

set(fern_args "${ACTION}" --compiler "${COMPILER}"
    --workspace "${WORKSPACE_DIR}/workspace.toml")
if(ACTION STREQUAL "build")
    list(APPEND fern_args --module moss.app)
endif()
if(PROFILE STREQUAL "release")
    list(APPEND fern_args --release)
endif()

execute_process(
    COMMAND "${FERN}" ${fern_args}
    WORKING_DIRECTORY "${WORKSPACE_DIR}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
message("${out}${err}")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "fern ${ACTION} failed with status ${status}")
endif()

if(ACTION STREQUAL "build")
    # fern reports the generation it created: "built module '...' at '<path>'".
    if(NOT "${out}${err}" MATCHES "built module 'moss\.app' at '([^']+)'")
        message(FATAL_ERROR "fern did not report the moss.app artifact")
    endif()
    set(built "${CMAKE_MATCH_1}")
    if(NOT EXISTS "${built}")
        message(FATAL_ERROR "reported moss artifact is missing: ${built}")
    endif()
    get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${built}" "${OUTPUT}"
        RESULT_VARIABLE copy_status)
    if(NOT copy_status EQUAL 0)
        message(FATAL_ERROR "could not copy ${built} to ${OUTPUT}")
    endif()
endif()
