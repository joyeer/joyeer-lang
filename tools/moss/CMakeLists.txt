# moss is a Joyeer-written JVM inspection CLI built by the build-tree fern.
# It needs the optional Compression module (ZIP/JAR input), so it is built
# only when JOYEER_BUILD_COMPRESSION is ON.
#
# fern rejects parent-traversing member paths, so the build stages the Moss
# workspace and the generated Compression package side by side in the build tree.
file(GLOB_RECURSE JOYEER_MOSS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*")
list(APPEND JOYEER_MOSS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/project.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/workspace.toml")

set(JOYEER_MOSS_WORKSPACE "${CMAKE_CURRENT_BINARY_DIR}/workspace")
set(JOYEER_MOSS_COMPRESSION_PACKAGE
    "${CMAKE_BINARY_DIR}/src/library/Compression/package/$<CONFIG>")
if(CMAKE_CONFIGURATION_TYPES)
    set(JOYEER_MOSS_EXECUTABLE
        "${CMAKE_BINARY_DIR}/$<CONFIG>/bin/moss${CMAKE_EXECUTABLE_SUFFIX}")
else()
    set(JOYEER_MOSS_EXECUTABLE
        "${JOYEER_BIN_OUTPUT_INTDIR}/moss${CMAKE_EXECUTABLE_SUFFIX}")
endif()

# Debug builds use fern's default profile; other configurations use --release.
set(JOYEER_MOSS_PROFILE "$<IF:$<CONFIG:Debug>,debug,release>")

add_custom_command(
    OUTPUT "${JOYEER_MOSS_EXECUTABLE}"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DWORKSPACE_DIR=${JOYEER_MOSS_WORKSPACE}"
        "-DCOMPRESSION_PACKAGE=${JOYEER_MOSS_COMPRESSION_PACKAGE}"
        "-DFERN=${JOYEER_FERN_EXECUTABLE}"
        "-DCOMPILER=$<TARGET_FILE:joyeer>"
        "-DPROFILE=${JOYEER_MOSS_PROFILE}"
        "-DOUTPUT=${JOYEER_MOSS_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildMoss.cmake"
    DEPENDS fern joyeer JoyeerBackend JoyeerRuntime JoyeerCompression
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildMoss.cmake"
        ${JOYEER_MOSS_SOURCES}
    COMMENT "Building moss with the build-tree fern"
    VERBATIM)
add_custom_target(moss ALL
    DEPENDS "${JOYEER_MOSS_EXECUTABLE}"
    SOURCES ${JOYEER_MOSS_SOURCES})

install(PROGRAMS "${JOYEER_MOSS_EXECUTABLE}"
    DESTINATION . COMPONENT PRODUCT)

set(JOYEER_MOSS_EXECUTABLE "${JOYEER_MOSS_EXECUTABLE}" PARENT_SCOPE)
set(JOYEER_MOSS_WORKSPACE "${JOYEER_MOSS_WORKSPACE}" PARENT_SCOPE)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    # Module tests run through fern against the same staged workspace.
    add_test(NAME Moss.FernTests
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DWORKSPACE_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-workspace"
            "-DCOMPRESSION_PACKAGE=${JOYEER_MOSS_COMPRESSION_PACKAGE}"
            "-DFERN=${JOYEER_FERN_EXECUTABLE}"
            "-DCOMPILER=$<TARGET_FILE:joyeer>"
            "-DPROFILE=${JOYEER_MOSS_PROFILE}"
            "-DOUTPUT=unused"
            -DACTION=test
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/buildMoss.cmake")
    add_test(NAME Moss.JavapGoldens
        COMMAND "${CMAKE_COMMAND}"
            "-DMOSS=${JOYEER_MOSS_EXECUTABLE}"
            "-DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/jvm"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verifyGoldens.cmake")
    set_tests_properties(Moss.FernTests Moss.JavapGoldens PROPERTIES
        LABELS "moss;native" TIMEOUT 900)
    set_tests_properties(Moss.FernTests PROPERTIES RUN_SERIAL ON)
endif()
