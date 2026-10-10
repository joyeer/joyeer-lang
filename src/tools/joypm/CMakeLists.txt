if(WIN32)
    set(JOYEER_JOYPM_HOST_WINDOWS true)
else()
    set(JOYEER_JOYPM_HOST_WINDOWS false)
endif()
configure_file(platform.joyeer.in platform.joyeer @ONLY)

# This is the complete bootstrap compilation set, shared with the logic test.
# Do not discover sources: adding a file requires changing this explicit list.
set(JOYEER_JOYPM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/common.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/planner.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/executor.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.joyeer"
    "${CMAKE_CURRENT_BINARY_DIR}/platform.joyeer")

set(JOYEER_JOYPM_PROJECT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/project")
set(JOYEER_JOYPM_PROJECT_SOURCES "")
foreach(source IN LISTS JOYEER_JOYPM_SOURCES)
    get_filename_component(name "${source}" NAME)
    configure_file("${source}" "${JOYEER_JOYPM_PROJECT_DIRECTORY}/${name}" COPYONLY)
    string(APPEND JOYEER_JOYPM_PROJECT_SOURCES "    '${name}',\n")
endforeach()
configure_file(joyeer.toml.in "${JOYEER_JOYPM_PROJECT_DIRECTORY}/joyeer.toml" @ONLY)

if(CMAKE_CONFIGURATION_TYPES)
    set(JOYEER_JOYPM_EXECUTABLE
        "${CMAKE_BINARY_DIR}/$<CONFIG>/bin/joypm${CMAKE_EXECUTABLE_SUFFIX}")
else()
    set(JOYEER_JOYPM_EXECUTABLE
        "${JOYEER_BIN_OUTPUT_INTDIR}/joypm${CMAKE_EXECUTABLE_SUFFIX}")
endif()

# Debug sidecars are optional only when the host cannot produce them. In
# particular, missing dsymutil must not disable ordinary macOS native output.
set(joypm_debug_flag -gfull)
if(APPLE AND (NOT JOYEER_DSYMUTIL_EXECUTABLE OR
        NOT EXISTS "${JOYEER_DSYMUTIL_EXECUTABLE}" OR
        IS_DIRECTORY "${JOYEER_DSYMUTIL_EXECUTABLE}"))
    set(joypm_debug_flag -g0)
endif()

# CMake 3.20 supports configuration expressions in BYPRODUCTS. Ninja must
# know the Debug sidecar belongs to this edge so deleting it bootstraps again,
# without claiming nonexistent Release sidecars or sharing multi-config paths.
set(joypm_bootstrap_byproducts)
if(WIN32)
    get_filename_component(joypm_output_directory "${JOYEER_JOYPM_EXECUTABLE}" DIRECTORY)
    set(joypm_pdb "${joypm_output_directory}/joypm.pdb")
    list(APPEND joypm_bootstrap_byproducts "$<$<CONFIG:Debug>:${joypm_pdb}>")
elseif(APPLE AND joypm_debug_flag STREQUAL "-gfull")
    list(APPEND joypm_bootstrap_byproducts
        "$<$<CONFIG:Debug>:${JOYEER_JOYPM_EXECUTABLE}.dSYM>")
endif()

add_custom_command(
    OUTPUT "${JOYEER_JOYPM_EXECUTABLE}"
    BYPRODUCTS ${joypm_bootstrap_byproducts}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:joyeer>"
    COMMAND "$<TARGET_FILE:joyeer>"
        --module-name joypm.app
        "$<IF:$<CONFIG:Debug>,-O0,-O2>"
        "$<IF:$<CONFIG:Debug>,${joypm_debug_flag},-g0>"
        -o "${JOYEER_JOYPM_EXECUTABLE}" -- ${JOYEER_JOYPM_SOURCES}
    DEPENDS joyeer JoyeerNativeBackend JoyeerNativeRuntime
        ${JOYEER_JOYPM_SOURCES}
    COMMENT "Bootstrapping joypm with the build-tree Joyeer compiler"
    VERBATIM)
add_custom_target(joypm ALL
    DEPENDS "${JOYEER_JOYPM_EXECUTABLE}" ${JOYEER_JOYPM_SOURCES}
    SOURCES ${JOYEER_JOYPM_SOURCES} platform.joyeer.in joyeer.toml.in)
add_dependencies(joypm joyeer JoyeerNativeBackend JoyeerNativeRuntime)

install(PROGRAMS "${JOYEER_JOYPM_EXECUTABLE}"
    DESTINATION . COMPONENT PRODUCT)
if(WIN32)
    install(FILES "${joypm_pdb}" DESTINATION .
        CONFIGURATIONS Debug COMPONENT PRODUCT)
endif()

set(JOYEER_JOYPM_SOURCES "${JOYEER_JOYPM_SOURCES}" PARENT_SCOPE)
set(JOYEER_JOYPM_EXECUTABLE "${JOYEER_JOYPM_EXECUTABLE}" PARENT_SCOPE)
set(JOYEER_JOYPM_PROJECT_DIRECTORY "${JOYEER_JOYPM_PROJECT_DIRECTORY}" PARENT_SCOPE)