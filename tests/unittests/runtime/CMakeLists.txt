
add_executable(NativeRuntimeTests
        native_runtime_test.cpp
        host_test.cpp
)

target_include_directories(NativeRuntimeTests PRIVATE
        ${JOYEER_SOURCE_DIR}/lib/native
)

target_link_libraries(NativeRuntimeTests
        GTest::gtest_main
        JoyeerNativeRuntime
)

joyeer_discover_tests(NativeRuntimeTests
        PROPERTIES LABELS native-runtime
)

add_executable(NativeFilesystemTests filesystem_test.cpp)
target_link_libraries(NativeFilesystemTests
        GTest::gtest_main
        JoyeerNativeRuntime
)
# Preserve label lists through the wrapper and generated discovery script.
joyeer_discover_tests(NativeFilesystemTests
        PROPERTIES LABELS "native-runtime\\\\;filesystem"
)

add_executable(NativeProcessTestChild process_test_child.c)
set_target_properties(NativeProcessTestChild PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
)
add_executable(NativeProcessTests process_test.cpp)
target_compile_definitions(NativeProcessTests PRIVATE
        JOYEER_PROCESS_TEST_CHILD_PATH="$<TARGET_FILE:NativeProcessTestChild>"
)
add_dependencies(NativeProcessTests NativeProcessTestChild)
target_link_libraries(NativeProcessTests
        GTest::gtest_main
        JoyeerNativeRuntime
)
joyeer_discover_tests(NativeProcessTests
        PROPERTIES LABELS "native-runtime\\\\;process"
)

if(WIN32)
    add_executable(NativeEntryEncodingTests
            entry_encoding_test.cpp
            ${JOYEER_SOURCE_DIR}/lib/native/entry.c
    )
    set_target_properties(NativeEntryEncodingTests PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
    )
    target_compile_definitions(NativeEntryEncodingTests PRIVATE
            wmain=joyeer_test_entry_wmain
    )
    target_link_libraries(NativeEntryEncodingTests
            GTest::gtest_main
            JoyeerNativeRuntime
    )
    joyeer_discover_tests(NativeEntryEncodingTests
            PROPERTIES LABELS "native-runtime\\\\;entry\\\\;encoding"
    )
endif()

foreach(mode IN ITEMS O0 O2 FullDebug)
    if(mode STREQUAL "FullDebug")
        set(host_compiler_args "-O0;-gfull")
    else()
        set(host_compiler_args "-${mode}")
    endif()
    set(host_work_directory "${CMAKE_CURRENT_BINARY_DIR}/portable-host-${mode}")
    file(MAKE_DIRECTORY "${host_work_directory}")
    set(filesystem_output "${CMAKE_CURRENT_BINARY_DIR}/filesystem-${mode}${CMAKE_EXECUTABLE_SUFFIX}")
    add_test(NAME NativePortableFilesystem${mode}
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/native/filesystem.joyeer
                    -DOUTPUT_FILE=${filesystem_output}
                    "-DCOMPILER_ARGS=${host_compiler_args}"
                    "-DPROGRAM_ARGUMENT_1=${host_work_directory}"
                    "-DEXPECTED_OUTPUT=filesystem ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set(process_output "${CMAKE_CURRENT_BINARY_DIR}/process-${mode}${CMAKE_EXECUTABLE_SUFFIX}")
    add_test(NAME NativeSynchronousProcess${mode}
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/native/process.joyeer
                    -DOUTPUT_FILE=${process_output}
                    "-DCOMPILER_ARGS=${host_compiler_args}"
                    "-DPROGRAM_ARGUMENT_1=${process_output}"
                    "-DPROGRAM_ARGUMENT_2=${host_work_directory}"
                    "-DEXPECTED_OUTPUT=child ok\nprocess ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set(module_host_output "${CMAKE_CURRENT_BINARY_DIR}/module-host-${mode}${CMAKE_EXECUTABLE_SUFFIX}")
    add_test(NAME NativeModuleHost${mode}
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DMODULE_NAME=host.app
                    -DINPUT_FILES=${JOYEER_SOURCE_DIR}/tests/modules/host_app/main.joyeer
                    -DOUTPUT_FILE=${module_host_output}
                    "-DCOMPILER_ARGS=${host_compiler_args};--module-source;host.services=${JOYEER_SOURCE_DIR}/tests/modules/host_lib/files.joyeer;--module-source;host.services=${JOYEER_SOURCE_DIR}/tests/modules/host_lib/process.joyeer"
                    "-DPROGRAM_ARGUMENT_1=${module_host_output}"
                    "-DPROGRAM_ARGUMENT_2=${host_work_directory}"
                    "-DEXPECTED_OUTPUT=module host ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(
            NativePortableFilesystem${mode} NativeSynchronousProcess${mode} NativeModuleHost${mode}
            PROPERTIES LABELS "native;native-runtime;host;ownership" TIMEOUT 120
    )
endforeach()

add_executable(RuntimeOptionsTests
        arguments_test.cpp
)

target_link_libraries(RuntimeOptionsTests
        GTest::gtest_main
        JoyeerCLI
        JoyeerDiagnostic
)

joyeer_discover_tests(RuntimeOptionsTests
        PROPERTIES LABELS "native-runtime\;optimization\;debug-info"
)
