add_library(SystemMacOSFixture STATIC macos-fixture.c)
set_target_properties(SystemMacOSFixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

set(empty_filesystem "${CMAKE_CURRENT_BINARY_DIR}/empty")
file(MAKE_DIRECTORY "${empty_filesystem}")
set(macos_fixtures "${JOYEER_TESTS_DIR}/e2e/macos-system")
foreach(level IN ITEMS 0 2)
    add_test(NAME "SystemMacOS.ErrorCodesO${level}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${macos_fixtures}/error_codes.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/macos-error-codes-O${level}"
            "-DCOMPILER_ARGS=-O${level};-g0"
            "-DPROGRAM_WORKING_DIRECTORY=${empty_filesystem}"
            "-DEXPECTED_OUTPUT=macOS filesystem error codes passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    add_test(NAME "SystemMacOS.InterruptedO${level}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${macos_fixtures}/interrupted.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/macos-interrupted-O${level}"
            "-DCOMPILER_ARGS=--module-name;test.host;--runtime-module;test.host;--link-library;$<TARGET_FILE:SystemMacOSFixture>;-O${level};-g0"
            "-DEXPECTED_OUTPUT=macOS interrupted calls retried\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("SystemMacOS.ErrorCodesO${level}" "SystemMacOS.InterruptedO${level}"
        PROPERTIES LABELS "host;native" TIMEOUT 60)
endforeach()
