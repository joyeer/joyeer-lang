set(JOYEER_MOSS_DIR "${JOYEER_SOURCE_DIR}/tools/moss")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tools/moss")
# moss is a Joyeer-written JVM inspection CLI. It is an ordinary fern project:
# once the compiler and fern exist, fern builds and tests it as members of the
# root workspace.toml. This module only hooks that into the repository build. Moss
# needs the Compression standard module (ZIP/JAR input), so it is configured only
# with JOYEER_BUILD_COMPRESSION (on by default).
file(GLOB_RECURSE JOYEER_MOSS_SOURCES CONFIGURE_DEPENDS
    "${JOYEER_MOSS_DIR}/src/*.joyeer"
    "${JOYEER_MOSS_DIR}/src/*.toml"
    "${JOYEER_MOSS_DIR}/tests/*.joyeer"
    "${JOYEER_MOSS_DIR}/tests/*.toml"
    "${JOYEER_MOSS_DIR}/tests/fixtures/*")
list(APPEND JOYEER_MOSS_SOURCES
    "${JOYEER_MOSS_DIR}/project.toml"
    "${JOYEER_MOSS_DIR}/workspace.toml")

# The executable goes to the same bin folder as joyeer and fern.
set(JOYEER_MOSS_BIN_DIR "$<TARGET_FILE_DIR:joyeer>")
set(JOYEER_MOSS_EXECUTABLE "${JOYEER_MOSS_BIN_DIR}/moss${CMAKE_EXECUTABLE_SUFFIX}")
set(JOYEER_MOSS_STAMP "${CMAKE_BINARY_DIR}/tools/moss/moss.stamp")
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
    # The javap goldens run `moss disasm` end to end from a fern test module. Fern
    # does not build bin modules, so the freshly built executable is passed explicitly.
    add_test(NAME Moss.Fern.moss.golden.tests
        COMMAND "${JOYEER_FERN_EXECUTABLE}" test
            --compiler "$<TARGET_FILE:joyeer>"
            --workspace "${JOYEER_SOURCE_DIR}/workspace.toml"
            --module moss.golden.tests
            ${JOYEER_MOSS_PROFILE_ARGS}
            -- --moss "${JOYEER_MOSS_EXECUTABLE}")
    list(APPEND moss_fern_tests Moss.Fern.moss.golden.tests)
    # `moss serve` end to end: a real process with stdin from a file, so the
    # framing, caching path and clean exit are checked as a client sees them.
    add_test(NAME Moss.Serve
        COMMAND "${CMAKE_COMMAND}"
            "-DMOSS_EXECUTABLE=${JOYEER_MOSS_EXECUTABLE}"
            "-DFIXTURE_DIR=${JOYEER_MOSS_DIR}/tests"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyMossServe.cmake")
    list(APPEND moss_fern_tests Moss.Serve)
    set_tests_properties(${moss_fern_tests} PROPERTIES
        LABELS "moss;native" TIMEOUT 900)
    set_tests_properties(${moss_fern_tests} PROPERTIES RUN_SERIAL ON)
endif()
