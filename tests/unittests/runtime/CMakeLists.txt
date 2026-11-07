
add_executable(RuntimeTests
        runtime_test.cpp
        host_test.cpp
)

target_include_directories(RuntimeTests PRIVATE
        ${JOYEER_SOURCE_DIR}/src/runtime
)

target_link_libraries(RuntimeTests
        GTest::gtest_main
        JoyeerRuntime
)

joyeer_discover_tests(RuntimeTests
        PROPERTIES LABELS runtime
)

if(WIN32)
    add_executable(NativeEntryEncodingTests
            entry_encoding_test.cpp
            ${JOYEER_SOURCE_DIR}/src/runtime/entry.c
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
            JoyeerRuntime
    )
    joyeer_discover_tests(NativeEntryEncodingTests
            PROPERTIES LABELS "runtime\\\\;entry\\\\;encoding"
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
            PROPERTIES LABELS "native;runtime;host;ownership" TIMEOUT 120
    )
endforeach()

add_executable(RuntimeOptionsTests
        arguments_test.cpp
)

target_link_libraries(RuntimeOptionsTests
        GTest::gtest_main
        JoyeerCompilerCore
)

joyeer_discover_tests(RuntimeOptionsTests
        PROPERTIES LABELS "runtime\;optimization\;debug-info"
)

# Micro-benchmarks for the exported runtime ABI. Deliberately not registered
# with CTest: timings are not pass/fail. Run in a Release build with
#   cmake --build <dir> --target runtime-benchmark
add_executable(RuntimeBenchmarks runtime_benchmark.cpp)
target_include_directories(RuntimeBenchmarks PRIVATE ${JOYEER_SOURCE_DIR}/src)
target_link_libraries(RuntimeBenchmarks PRIVATE JoyeerRuntime)
add_custom_target(runtime-benchmark
    COMMAND RuntimeBenchmarks
    DEPENDS RuntimeBenchmarks
    COMMENT "Running the runtime micro-benchmarks"
    VERBATIM)

# The Joyeer runtime core object may define only the approved exports and call
# only the approved imports. Update both lists in the same change that adds a
# function to src/runtime/joyeer/ (runtime plan §4).
get_filename_component(joyeer_llvm_bin "${JOYEER_CLANG_EXECUTABLE}" DIRECTORY)
find_program(JOYEER_RUNTIME_NM_EXECUTABLE NAMES llvm-nm llvm-nm.exe
    HINTS "${JOYEER_LLVM_ROOT}/bin" "${joyeer_llvm_bin}")
get_target_property(JOYEER_RUNTIME_CORE_OBJECT JoyeerRuntime JOYEER_CORE_OBJECT)
if(JOYEER_RUNTIME_NM_EXECUTABLE AND JOYEER_RUNTIME_CORE_OBJECT)
    # The C library calls the core may make: memory (allocator), compare and copy (a
    # variable-size memcpy intrinsic lowers to the call, decision 0009 D11), write
    # and exit (panic), and the C stdout stream (print). Windows spells the write call `_write`.
    set(joyeer_core_imports "malloc;realloc;free;memcmp;memcpy;_Exit;fwrite;fputc;joyeer_native_stdio")
    if(WIN32)
        list(APPEND joyeer_core_imports _write)
    else()
        list(APPEND joyeer_core_imports write)
    endif()
    add_test(NAME RuntimeCoreObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_CORE_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_host_valid_utf8;joyeer_string_equal_abi;joyeer_string_compare_abi;joyeer_panic;joyeer_static_slots;joyeer_alloc;joyeer_realloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_string_destroy_abi;joyeer_byte_to_string_abi;joyeer_string_concat_abi;joyeer_print_string_abi;joyeer_print_int;joyeer_print_uint;joyeer_print_byte;joyeer_print_bool"
            "-DALLOWED_UNDEFINED=${joyeer_core_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeCoreObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)
endif()
