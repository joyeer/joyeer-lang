# Tool acceptance is independent of GoogleTest and JOYEER_BUILD_UNITTESTS.
set(joypm_fixture_dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(joypm_test_bin "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bin")
set(joypm_spy "${joypm_test_bin}/compiler-spy${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_no_output "${joypm_test_bin}/compiler-no-output${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_forwarder "${joypm_test_bin}/forwarding-driver${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_logic "${joypm_test_bin}/logic${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_input_writer "${joypm_test_bin}/cmake-input-writer${CMAKE_EXECUTABLE_SUFFIX}")

add_executable(JoypmHostFixture "${joypm_fixture_dir}/host-fixture.c")
set_target_properties(JoypmHostFixture PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${joypm_test_bin}")
if(WIN32)
    target_link_libraries(JoypmHostFixture PRIVATE advapi32)
endif()

foreach(fixture IN ITEMS compiler-spy compiler-no-output forwarding-driver)
    add_custom_command(
        OUTPUT "${joypm_test_bin}/${fixture}${CMAKE_EXECUTABLE_SUFFIX}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${joypm_test_bin}"
        COMMAND "$<TARGET_FILE:joyeer>" -O0 -g0
            -o "${joypm_test_bin}/${fixture}${CMAKE_EXECUTABLE_SUFFIX}"
            -- "${joypm_fixture_dir}/${fixture}.joyeer"
        DEPENDS joyeer JoyeerNativeBackend JoyeerNativeRuntime
            "${joypm_fixture_dir}/${fixture}.joyeer"
        VERBATIM)
endforeach()

set(joypm_input_writer_sources
    "${JOYEER_SOURCE_DIR}/src/tools/joypm/common.joyeer"
    "${CMAKE_BINARY_DIR}/src/tools/joypm/platform.joyeer"
    "${joypm_fixture_dir}/cmake-input-writer.joyeer")
add_custom_command(
    OUTPUT "${joypm_input_writer}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${joypm_test_bin}"
    COMMAND "$<TARGET_FILE:joyeer>" --module-name joypm.app -O0 -g0
        -o "${joypm_input_writer}" -- ${joypm_input_writer_sources}
    DEPENDS joyeer JoyeerNativeBackend JoyeerNativeRuntime ${joypm_input_writer_sources}
    VERBATIM)

set(joypm_logic_sources ${JOYEER_JOYPM_SOURCES})
list(REMOVE_ITEM joypm_logic_sources "${JOYEER_SOURCE_DIR}/src/tools/joypm/main.joyeer")
list(APPEND joypm_logic_sources "${joypm_fixture_dir}/logic.joyeer")
add_custom_command(
    OUTPUT "${joypm_logic}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${joypm_test_bin}"
    COMMAND "$<TARGET_FILE:joyeer>" --module-name joypm.app -O0 -g0
        -o "${joypm_logic}" -- ${joypm_logic_sources}
    DEPENDS joyeer JoyeerNativeBackend JoyeerNativeRuntime ${joypm_logic_sources}
    VERBATIM)
add_custom_target(JoypmAcceptanceTools ALL
    DEPENDS "${joypm_spy}" "${joypm_no_output}" "${joypm_forwarder}" "${joypm_logic}"
        "${joypm_input_writer}"
    SOURCES "${joypm_fixture_dir}/compiler-spy.joyeer"
        "${joypm_fixture_dir}/compiler-no-output.joyeer"
        "${joypm_fixture_dir}/forwarding-driver.joyeer" ${joypm_logic_sources}
        ${joypm_input_writer_sources})
add_dependencies(JoypmAcceptanceTools joypm)
add_dependencies(JoypmAcceptanceTools JoypmHostFixture)

function(joypm_add_acceptance name script labels timeout)
    add_test(NAME "Joypm.${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYPM_EXECUTABLE=${JOYEER_JOYPM_EXECUTABLE}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DCOMPILER_SPY=${joypm_spy}"
            "-DCOMPILER_NO_OUTPUT=${joypm_no_output}"
            "-DFORWARDING_DRIVER=${joypm_forwarder}"
            "-DLOGIC_EXECUTABLE=${joypm_logic}"
            "-DINPUT_WRITER=${joypm_input_writer}"
            "-DHOST_FIXTURE=$<TARGET_FILE:JoypmHostFixture>"
            "-DSELF_BUILD_PROJECT=${JOYEER_JOYPM_PROJECT_DIRECTORY}"
            "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
            "-DTEST_CONFIGURATION=$<CONFIG>"
            "-DCOMPILER_FILE_NAME=$<TARGET_FILE_NAME:joyeer>"
            "-DRUNTIME_FILE_NAMES=$<TARGET_FILE_NAME:joyeer>;$<TARGET_FILE_NAME:JoyeerNativeBackend>;$<TARGET_FILE_NAME:JoyeerNativeRuntime>"
            "-DLICENSE_SOURCES=${JOYEER_PRODUCT_LICENSE_FILES}"
            "-DLIBXML_COPYRIGHT=${JOYEER_LIBXML2_SOURCE_DIR}/Copyright"
            "-DFIXTURE_DIR=${joypm_fixture_dir}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runs/${name}"
            "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
    set_tests_properties("Joypm.${name}" PROPERTIES
        LABELS "joypm;native;acceptance;${labels}"
        TIMEOUT "${timeout}")
endfunction()

joypm_add_acceptance(Cli verifyCli.cmake "cli" 90)
joypm_add_acceptance(Init verifyInit.cmake "cli;filesystem" 120)
joypm_add_acceptance(Manifest verifyManifest.cmake "manifest" 180)
joypm_add_acceptance(ManifestLimits verifyManifestLimits.cmake "manifest;logic;limits" 240)
joypm_add_acceptance(Plans verifyPlans.cmake "planner" 180)
joypm_add_acceptance(Workflow verifyJoypm.cmake "compiler;filesystem;process" 300)
joypm_add_acceptance(TestRunner verifyTestRunner.cmake "compiler;process" 300)
joypm_add_acceptance(Unicode verifyUnicode.cmake "compiler;process;unicode" 180)
joypm_add_acceptance(Logic verifyLogic.cmake "cli;manifest;logic" 60)
joypm_add_acceptance(Concurrent verifyConcurrent.cmake "compiler;filesystem;process" 180)
joypm_add_acceptance(HostFailures verifyHostFailures.cmake "compiler;filesystem;process" 240)
joypm_add_acceptance(SelfBuild verifySelfBuild.cmake "compiler;self-build" 300)
joypm_add_acceptance(Installed verifyInstalled.cmake "compiler;packaging" 300)
if(WIN32)
    joypm_add_acceptance(Bootstrap verifyBootstrap.cmake "bootstrap;debug-info" 180)
    set_tests_properties(Joypm.Bootstrap PROPERTIES RUN_SERIAL TRUE)
endif()