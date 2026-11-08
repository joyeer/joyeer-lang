
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
    # The entry logic alone, without the `wmain` wrapper, so the test supplies its
    # own `joyeer_main` and keeps gtest's entry point.
    set(JOYEER_TEST_ENTRY_OBJECT
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/test-entry${CMAKE_C_OUTPUT_EXTENSION}")
    joyeer_runtime_core_object("${JOYEER_TEST_ENTRY_OBJECT}" runtime.entry
            "Compiling the Joyeer entry logic for the encoding tests" "${JOYEER_RUNTIME_ENTRY_LOGIC}")
    add_executable(NativeEntryEncodingTests
            entry_encoding_test.cpp
            "${JOYEER_TEST_ENTRY_OBJECT}"
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
    # The calls the core may make: the system memory mapping (allocator), compare, copy and fill (a
    # variable-size memcpy/memset intrinsic lowers to the call, decision 0009 D11), write
    # and exit (panic), and the C stdout stream (print). Each platform spells those differently.
    set(joyeer_core_imports "memcmp;memcpy;memset;_Exit;fwrite;fputc")
    if(WIN32)
        list(APPEND joyeer_core_imports VirtualAlloc VirtualFree _write __acrt_iob_func _fltused)
    elseif(APPLE)
        list(APPEND joyeer_core_imports mmap munmap write __stdoutp)
    else()
        list(APPEND joyeer_core_imports mmap munmap write stdout)
    endif()
    add_test(NAME RuntimeCoreObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_CORE_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_host_valid_utf8;joyeer_string_equal_abi;joyeer_string_compare_abi;joyeer_panic;joyeer_static_slots;joyeer_alloc;joyeer_realloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_string_destroy_abi;joyeer_byte_to_string_abi;joyeer_string_concat_abi;joyeer_print_string_abi;joyeer_print_int;joyeer_print_uint;joyeer_print_byte;joyeer_print_bool;joyeer_array_create_owned_abi;joyeer_array_clone_abi;joyeer_array_append_owned_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy;joyeer_native_bytes_init;joyeer_native_strings_init;joyeer_native_bytes_assign;joyeer_native_string_assign;joyeer_native_strings_assign;joyeer_dictionary_create_owned_abi;joyeer_dictionary_clone_abi;joyeer_dictionary_set_owned_abi;joyeer_dictionary_find_abi;joyeer_dictionary_at_abi;joyeer_dictionary_destroy_abi;joyeer_print_float;joyeer_print_double;joyeer_format_double_abi;joyeer_format_float_abi"
            "-DALLOWED_UNDEFINED=${joyeer_core_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeCoreObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)

    # The archive is the two Joyeer objects and nothing compiled from C.
    find_program(JOYEER_RUNTIME_AR_EXECUTABLE NAMES llvm-ar llvm-ar.exe
        HINTS "${JOYEER_LLVM_ROOT}/bin" "${joyeer_llvm_bin}")
    if(JOYEER_RUNTIME_AR_EXECUTABLE)
        get_filename_component(core_name "${JOYEER_RUNTIME_CORE_OBJECT}" NAME)
        get_target_property(entry_object JoyeerRuntime JOYEER_ENTRY_OBJECT)
        get_filename_component(entry_name "${entry_object}" NAME)
        add_test(NAME RuntimeArchiveMembers
            COMMAND "${CMAKE_COMMAND}"
                "-DAR=${JOYEER_RUNTIME_AR_EXECUTABLE}"
                "-DARCHIVE=$<TARGET_FILE:JoyeerRuntime>"
                "-DEXPECTED_MEMBERS=${core_name};${entry_name}"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeArchive.cmake")
        set_tests_properties(RuntimeArchiveMembers PROPERTIES LABELS "runtime;native" TIMEOUT 30)
    endif()

    # The entry object defines the program entry point and reaches the core only
    # through its C ABI.
    get_target_property(JOYEER_RUNTIME_ENTRY_OBJECT JoyeerRuntime JOYEER_ENTRY_OBJECT)
    set(joyeer_entry_imports "joyeer_main;joyeer_main_uses_arguments;joyeer_panic;joyeer_alloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_array_create_owned_abi;joyeer_array_append_owned_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy")
    if(WIN32)
        list(APPEND joyeer_entry_imports WideCharToMultiByte _write)
        set(joyeer_entry_point wmain)
    else()
        list(APPEND joyeer_entry_imports write)
        set(joyeer_entry_point main)
    endif()
    add_test(NAME RuntimeEntryObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_ENTRY_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_entry_run;joyeer_static_slots;${joyeer_entry_point}"
            "-DALLOWED_UNDEFINED=${joyeer_entry_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeEntryObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)
endif()
