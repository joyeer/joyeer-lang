# moss is a Joyeer-written JVM inspection CLI. It is an ordinary fern project:
# once the compiler and fern exist, fern builds and tests it as members of the
# root workspace.toml. This file only hooks that into the repository build. Moss
# needs the Compression standard module (ZIP/JAR input), so it is configured only
# with JOYEER_BUILD_COMPRESSION (on by default).
file(GLOB_RECURSE JOYEER_MOSS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.joyeer"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*")
list(APPEND JOYEER_MOSS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/project.toml"
    "${JOYEER_SOURCE_DIR}/workspace.toml")

# The executable goes to the same bin folder as joyeer and fern.
set(JOYEER_MOSS_BIN_DIR "$<TARGET_FILE_DIR:joyeer>")
set(JOYEER_MOSS_EXECUTABLE "${JOYEER_MOSS_BIN_DIR}/moss${CMAKE_EXECUTABLE_SUFFIX}")
set(JOYEER_MOSS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/moss.stamp")
# Debug uses fern's default profile; every other build type is --release. A
# generator expression would pass an empty argument to fern in Debug, so the
# profile is fixed at configure time (single-config generators, as in the presets).
set(JOYEER_MOSS_PROFILE_ARGS)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(JOYEER_MOSS_PROFILE_ARGS --release)
endif()

add_custom_command(
    OUTPUT "${JOYEER_MOSS_STAMP}"
    COMMAND "${JOYEER_FERN_EXECUTABLE}" build
        --compiler "$<TARGET_FILE:joyeer>"
        --workspace "${JOYEER_SOURCE_DIR}/workspace.toml"
        --module moss.app
        --bin-dir "${JOYEER_MOSS_BIN_DIR}"
        ${JOYEER_MOSS_PROFILE_ARGS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${JOYEER_MOSS_STAMP}"
    DEPENDS fern joyeer JoyeerBackend JoyeerRuntime JoyeerStandardLibrary
        ${JOYEER_MOSS_SOURCES}
    COMMENT "Building moss with fern"
    VERBATIM)
add_custom_target(moss ALL
    DEPENDS "${JOYEER_MOSS_STAMP}"
    SOURCES ${JOYEER_MOSS_SOURCES})

install(PROGRAMS "${JOYEER_MOSS_EXECUTABLE}"
    DESTINATION . COMPONENT Moss)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    # The test modules belong to the repository's root workspace; its fern run
    # also covers them. These entries allow focused iteration (-L moss).
    set(moss_fern_tests "")
    foreach(module IN ITEMS moss.tests moss.binary.tests moss.jvm.tests moss.archive.tests)
        add_test(NAME "Moss.Fern.${module}"
            COMMAND "${JOYEER_FERN_EXECUTABLE}" test
                --compiler "$<TARGET_FILE:joyeer>"
                --workspace "${JOYEER_SOURCE_DIR}/workspace.toml"
                --module "${module}"
                ${JOYEER_MOSS_PROFILE_ARGS})
        list(APPEND moss_fern_tests "Moss.Fern.${module}")
    endforeach()
    add_test(NAME Moss.JavapGoldens
        COMMAND "${CMAKE_COMMAND}"
            "-DMOSS=${JOYEER_MOSS_EXECUTABLE}"
            "-DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/jvm"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verifyGoldens.cmake")
    set_tests_properties(${moss_fern_tests} Moss.JavapGoldens PROPERTIES
        LABELS "moss;native" TIMEOUT 900)
    set_tests_properties(${moss_fern_tests} PROPERTIES RUN_SERIAL ON)
endif()
