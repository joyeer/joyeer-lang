# Tool acceptance is independent of GoogleTest and JOYEER_BUILD_UNITTESTS.
set(joypm_fixture_dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(joypm_test_bin "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bin")
set(joypm_spy "${joypm_test_bin}/compiler-spy${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_no_output "${joypm_test_bin}/compiler-no-output${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_forwarder "${joypm_test_bin}/forwarding-driver${CMAKE_EXECUTABLE_SUFFIX}")
set(joypm_logic "${joypm_test_bin}/logic${CMAKE_EXECUTABLE_SUFFIX}")

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
    SOURCES "${joypm_fixture_dir}/compiler-spy.joyeer"
        "${joypm_fixture_dir}/compiler-no-output.joyeer"
        "${joypm_fixture_dir}/forwarding-driver.joyeer" ${joypm_logic_sources})
add_dependencies(JoypmAcceptanceTools joypm)

function(joypm_add_acceptance name script labels timeout)
    add_test(NAME "Joypm.${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYPM_EXECUTABLE=${JOYEER_JOYPM_EXECUTABLE}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DCOMPILER_SPY=${joypm_spy}"
            "-DCOMPILER_NO_OUTPUT=${joypm_no_output}"
            "-DFORWARDING_DRIVER=${joypm_forwarder}"
            "-DLOGIC_EXECUTABLE=${joypm_logic}"
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