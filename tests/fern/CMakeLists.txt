# Tool acceptance is independent of GoogleTest and JOYEER_BUILD_UNITTESTS.
set(fern_fixture_dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(fern_test_bin "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bin")
set(fern_spy "${fern_test_bin}/compiler-spy${CMAKE_EXECUTABLE_SUFFIX}")
set(fern_no_output "${fern_test_bin}/compiler-no-output${CMAKE_EXECUTABLE_SUFFIX}")
set(fern_forwarder "${fern_test_bin}/forwarding-driver${CMAKE_EXECUTABLE_SUFFIX}")
set(fern_logic "${fern_test_bin}/logic${CMAKE_EXECUTABLE_SUFFIX}")
set(fern_input_writer "${fern_test_bin}/cmake-input-writer${CMAKE_EXECUTABLE_SUFFIX}")

add_executable(FernHostFixture "${fern_fixture_dir}/host-fixture.c")
set_target_properties(FernHostFixture PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${fern_test_bin}")
if(WIN32)
    target_link_libraries(FernHostFixture PRIVATE advapi32)
endif()

foreach(fixture IN ITEMS compiler-spy compiler-no-output forwarding-driver)
    add_custom_command(
        OUTPUT "${fern_test_bin}/${fixture}${CMAKE_EXECUTABLE_SUFFIX}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${fern_test_bin}"
        COMMAND "$<TARGET_FILE:joyeer>" -O0 -g0
            -o "${fern_test_bin}/${fixture}${CMAKE_EXECUTABLE_SUFFIX}"
            -- "${fern_fixture_dir}/${fixture}.joyeer"
        DEPENDS joyeer JoyeerBackend JoyeerRuntime
            "${fern_fixture_dir}/${fixture}.joyeer"
        VERBATIM)
endforeach()

set(fern_input_writer_sources
    "${JOYEER_SOURCE_DIR}/tools/fern/common.joyeer"
    "${CMAKE_BINARY_DIR}/tools/fern/platform.joyeer"
    "${fern_fixture_dir}/cmake-input-writer.joyeer")
add_custom_command(
    OUTPUT "${fern_input_writer}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fern_test_bin}"
    COMMAND "$<TARGET_FILE:joyeer>" --module-name fern.app -O0 -g0
        -o "${fern_input_writer}" -- ${fern_input_writer_sources}
    DEPENDS joyeer JoyeerBackend JoyeerRuntime ${fern_input_writer_sources}
    VERBATIM)

set(fern_logic_sources ${JOYEER_FERN_SOURCES})
list(REMOVE_ITEM fern_logic_sources "${JOYEER_SOURCE_DIR}/tools/fern/main.joyeer")
list(APPEND fern_logic_sources "${fern_fixture_dir}/logic.joyeer")
add_custom_command(
    OUTPUT "${fern_logic}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fern_test_bin}"
    COMMAND "$<TARGET_FILE:joyeer>" --module-name fern.app -O0 -g0
        -o "${fern_logic}" -- ${fern_logic_sources}
    DEPENDS joyeer JoyeerBackend JoyeerRuntime ${fern_logic_sources}
    VERBATIM)
add_custom_target(FernAcceptanceTools ALL
    DEPENDS "${fern_spy}" "${fern_no_output}" "${fern_forwarder}" "${fern_logic}"
        "${fern_input_writer}"
    SOURCES "${fern_fixture_dir}/compiler-spy.joyeer"
        "${fern_fixture_dir}/compiler-no-output.joyeer"
        "${fern_fixture_dir}/forwarding-driver.joyeer" ${fern_logic_sources}
        ${fern_input_writer_sources})
add_dependencies(FernAcceptanceTools fern)
add_dependencies(FernAcceptanceTools FernHostFixture)

function(fern_add_acceptance name script labels timeout)
    add_test(NAME "Fern.${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-DFERN_EXECUTABLE=${JOYEER_FERN_EXECUTABLE}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DCOMPILER_SPY=${fern_spy}"
            "-DCOMPILER_NO_OUTPUT=${fern_no_output}"
            "-DFORWARDING_DRIVER=${fern_forwarder}"
            "-DLOGIC_EXECUTABLE=${fern_logic}"
            "-DINPUT_WRITER=${fern_input_writer}"
            "-DHOST_FIXTURE=$<TARGET_FILE:FernHostFixture>"
            "-DSELF_BUILD_PROJECT=${JOYEER_FERN_PROJECT_DIRECTORY}"
            "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
            "-DTEST_CONFIGURATION=$<CONFIG>"
            "-DDSYMUTIL_EXECUTABLE=${JOYEER_DSYMUTIL_EXECUTABLE}"
            "-DCOMPILER_FILE_NAME=$<TARGET_FILE_NAME:joyeer>"
            "-DRUNTIME_FILE_NAMES=$<TARGET_FILE_NAME:joyeer>;$<TARGET_FILE_NAME:JoyeerBackend>;$<TARGET_FILE_NAME:JoyeerRuntime>"
            "-DLICENSE_SOURCE=${JOYEER_SOURCE_DIR}/LICENSE"
            "-DFIXTURE_DIR=${fern_fixture_dir}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runs/${name}"
            "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
    set_tests_properties("Fern.${name}" PROPERTIES
        LABELS "fern;native;acceptance;${labels}"
        TIMEOUT "${timeout}")
endfunction()

fern_add_acceptance(Cli verifyCli.cmake "cli" 90)
fern_add_acceptance(Init verifyInit.cmake "cli;filesystem" 120)
fern_add_acceptance(Manifest verifyManifest.cmake "manifest" 180)
fern_add_acceptance(ManifestLimits verifyManifestLimits.cmake "manifest;logic;limits" 240)
fern_add_acceptance(Plans verifyPlans.cmake "planner" 180)
fern_add_acceptance(Workspace verifyWorkspace.cmake "workspace;manifest;planner" 240)
fern_add_acceptance(Workflow verifyFern.cmake "compiler;filesystem;process" 300)
fern_add_acceptance(TestRunner verifyTestRunner.cmake "compiler;process" 300)
fern_add_acceptance(Unicode verifyUnicode.cmake "compiler;process;unicode" 180)
fern_add_acceptance(Logic verifyLogic.cmake "cli;manifest;logic" 60)
fern_add_acceptance(Concurrent verifyConcurrent.cmake "compiler;filesystem;process" 180)
fern_add_acceptance(HostFailures verifyHostFailures.cmake "compiler;filesystem;process" 240)
fern_add_acceptance(SelfBuild verifySelfBuild.cmake "compiler;self-build" 300)
fern_add_acceptance(Installed verifyInstalled.cmake "compiler;packaging" 300)
fern_add_acceptance(Bootstrap verifyBootstrap.cmake "bootstrap;debug-info" 180)
set_tests_properties(Fern.Bootstrap PROPERTIES RUN_SERIAL TRUE)