
add_executable(RuntimeTests
        runtime_test.cpp
        host_test.cpp
)

target_include_directories(RuntimeTests PRIVATE
        ${JOYEER_RUNTIME_INCLUDE_DIR}
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
target_include_directories(RuntimeBenchmarks PRIVATE ${JOYEER_RUNTIME_INCLUDE_DIR})
target_link_libraries(RuntimeBenchmarks PRIVATE JoyeerRuntime)
add_custom_target(runtime-benchmark
    COMMAND RuntimeBenchmarks
    DEPENDS RuntimeBenchmarks
    COMMENT "Running the runtime micro-benchmarks"
    VERBATIM)

# The Joyeer runtime core object may define only the approved exports and call
# only the approved imports. Update both lists in the same change that adds a
# function to lib/Runtime/core/ (runtime plan §4).
get_filename_component(joyeer_llvm_bin "${JOYEER_CLANG_EXECUTABLE}" DIRECTORY)
find_program(JOYEER_RUNTIME_NM_EXECUTABLE NAMES llvm-nm llvm-nm.exe
    HINTS "${JOYEER_LLVM_ROOT}/bin" "${joyeer_llvm_bin}")
get_target_property(JOYEER_RUNTIME_CORE_OBJECT JoyeerRuntime JOYEER_CORE_OBJECT)
if(JOYEER_RUNTIME_NM_EXECUTABLE AND JOYEER_RUNTIME_CORE_OBJECT)
    # The calls the core may make: compare, copy and fill (a variable-size
    # memcpy/memset intrinsic lowers to the call, decision 0009 D11) and the operating
    # system's own interface for memory, standard streams and exit (decision 0011). Linux
    # makes system calls and imports nothing else.
    set(joyeer_core_imports "memcmp;memcpy;memset")
    if(WIN32)
        list(APPEND joyeer_core_imports VirtualAlloc VirtualFree GetStdHandle GetFileType WriteFile ReadFile
            GetConsoleMode WriteConsoleW MultiByteToWideChar GetLastError ExitProcess _fltused)
    elseif(APPLE)
        list(APPEND joyeer_core_imports mmap munmap write read isatty __error _exit)
    else()
        # Linux: system calls only.
    endif()
    add_test(NAME RuntimeCoreObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_CORE_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_host_valid_utf8;joyeer_string_equal_abi;joyeer_string_compare_abi;joyeer_panic;joyeer_static_slots;joyeer_alloc;joyeer_realloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_string_destroy_abi;joyeer_byte_to_string_abi;joyeer_string_concat_abi;joyeer_print_string_abi;joyeer_print_int;joyeer_print_uint;joyeer_print_byte;joyeer_print_bool;joyeer_array_create_owned_abi;joyeer_array_clone_abi;joyeer_array_append_owned_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy;joyeer_native_bytes_init;joyeer_native_strings_init;joyeer_native_bytes_assign;joyeer_native_string_assign;joyeer_native_strings_assign;joyeer_dictionary_create_owned_abi;joyeer_dictionary_clone_abi;joyeer_dictionary_set_owned_abi;joyeer_dictionary_find_abi;joyeer_dictionary_at_abi;joyeer_dictionary_destroy_abi;joyeer_print_float;joyeer_print_double;joyeer_format_double_abi;joyeer_format_float_abi;joyeer_stdout_write;joyeer_stdout_flush;joyeer_stderr_write;joyeer_stdin_read_byte;joyeer_stdin_discard"
            "-DALLOWED_UNDEFINED=${joyeer_core_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeCoreObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)

    # The archive is the three Joyeer objects and nothing compiled from C.
    find_program(JOYEER_RUNTIME_AR_EXECUTABLE NAMES llvm-ar llvm-ar.exe
        HINTS "${JOYEER_LLVM_ROOT}/bin" "${joyeer_llvm_bin}")
    if(JOYEER_RUNTIME_AR_EXECUTABLE)
        get_filename_component(core_name "${JOYEER_RUNTIME_CORE_OBJECT}" NAME)
        get_target_property(entry_object JoyeerRuntime JOYEER_ENTRY_OBJECT)
        get_filename_component(entry_name "${entry_object}" NAME)
        get_target_property(main_object JoyeerRuntime JOYEER_MAIN_OBJECT)
        get_filename_component(main_name "${main_object}" NAME)
        add_test(NAME RuntimeArchiveMembers
            COMMAND "${CMAKE_COMMAND}"
                "-DAR=${JOYEER_RUNTIME_AR_EXECUTABLE}"
                "-DARCHIVE=$<TARGET_FILE:JoyeerRuntime>"
                "-DEXPECTED_MEMBERS=${core_name};${entry_name};${main_name}"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeArchive.cmake")
        set_tests_properties(RuntimeArchiveMembers PROPERTIES LABELS "runtime;native" TIMEOUT 30)
    endif()

    # The entry object defines the program entry point and reaches the core only
    # through its C ABI.
    get_target_property(JOYEER_RUNTIME_ENTRY_OBJECT JoyeerRuntime JOYEER_ENTRY_OBJECT)
    set(joyeer_entry_imports "joyeer_stderr_write;joyeer_stdout_flush;joyeer_main;joyeer_main_uses_arguments;joyeer_panic;joyeer_alloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_array_create_owned_abi;joyeer_array_append_owned_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy")
    if(WIN32)
        list(APPEND joyeer_entry_imports WideCharToMultiByte GetCommandLineW VirtualAlloc ExitProcess)
        set(joyeer_entry_point wmain)
        set(joyeer_entry_defined "joyeer_entry_run;joyeer_static_slots;joyeer_start")
    else()
        set(joyeer_entry_point main)
        set(joyeer_entry_defined "joyeer_entry_run;joyeer_static_slots")
    endif()
    add_test(NAME RuntimeEntryObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_ENTRY_OBJECT}"
            "-DEXPECTED_DEFINED=${joyeer_entry_defined}"
            "-DALLOWED_UNDEFINED=${joyeer_entry_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeEntryObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)

    # The C wrappers: the only part of the runtime that names the C library's startup.
    get_target_property(JOYEER_RUNTIME_MAIN_OBJECT JoyeerRuntime JOYEER_MAIN_OBJECT)
    add_test(NAME RuntimeMainObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_MAIN_OBJECT}"
            "-DEXPECTED_DEFINED=${joyeer_entry_point}"
            "-DALLOWED_UNDEFINED=atexit;joyeer_entry_run;joyeer_stdout_flush"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeMainObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)
endif()

# A Windows program that does not call the C library starts at its own entry point and
# imports the operating system's functions and nothing else; one that calls it keeps
# the C startup (decision 0011).
if(WIN32 AND JOYEER_RUNTIME_NM_EXECUTABLE)
    get_filename_component(joyeer_nm_directory "${JOYEER_RUNTIME_NM_EXECUTABLE}" DIRECTORY)
    find_program(JOYEER_RUNTIME_READOBJ_EXECUTABLE NAMES llvm-readobj llvm-readobj.exe
        HINTS "${joyeer_nm_directory}")
    if(JOYEER_RUNTIME_READOBJ_EXECUTABLE)
        add_test(NAME RuntimeOwnEntryImportsOnlySystemFunctions
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/target-architecture.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/own-entry-imports.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0;--module-name;runtime.own_entry;--runtime-module;runtime.own_entry"
                "-DEXPECTED_IMPORTS=ExitProcess;GetCommandLineW;GetConsoleMode;GetFileType;GetLastError;GetStdHandle;MultiByteToWideChar;ReadFile;VirtualAlloc;VirtualFree;WideCharToMultiByte;WriteConsoleW;WriteFile"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        add_test(NAME RuntimeCRuntimeProgramKeepsTheCStartup
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/never-import.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/c-startup-imports.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0;--module-name;runtime.never_import;--runtime-module;runtime.never_import"
                "-DMINIMUM_COUNT=30"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        set_tests_properties(RuntimeOwnEntryImportsOnlySystemFunctions RuntimeCRuntimeProgramKeepsTheCStartup
            PROPERTIES LABELS "runtime;native;entry" TIMEOUT 120)
        # `System` itself (files, processes, standard streams, the legacy reader) calls only
        # the operating system on Windows: a program using each part starts at its own entry
        # point and imports nothing that exists for the C startup or the C heap.
        foreach(fixture IN ITEMS filesystem process read_file stdin_lines)
            add_test(NAME RuntimeSystemProgramHasNoCRuntime_${fixture}
                COMMAND "${CMAKE_COMMAND}"
                    "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                    "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/native/${fixture}.joyeer"
                    "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/system-${fixture}.exe"
                    "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                    "-DCOMPILER_ARGS=-O0;-g0"
                    "-DFORBIDDEN_IMPORTS=IsProcessorFeaturePresent;GetStartupInfoW;InitializeSListHead;UnhandledExceptionFilter;SetUnhandledExceptionFilter;IsDebuggerPresent;GetProcessHeap;HeapAlloc;HeapFree;GetEnvironmentStringsW"
                    -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
            set_tests_properties(RuntimeSystemProgramHasNoCRuntime_${fixture}
                PROPERTIES LABELS "runtime;native;entry;host" TIMEOUT 120)
        endforeach()
    endif()
endif()

# On Linux `System` calls the kernel directly (decision 0011): a program that uses each
# part of it needs, from the C library, only the startup the linker adds and the compiler
# support functions.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND JOYEER_RUNTIME_NM_EXECUTABLE)
    foreach(fixture IN ITEMS filesystem process read_file stdin_lines)
        add_test(NAME RuntimeSystemProgramNeedsOnlyTheCStartup_${fixture}
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/native/${fixture}.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/system-${fixture}"
                "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0"
                "-DALLOWED_IMPORTS=__libc_start_main;__cxa_atexit;__gmon_start__;_ITM_registerTMCloneTable;_ITM_deregisterTMCloneTable;memcpy;memset;memmove;memcmp"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        set_tests_properties(RuntimeSystemProgramNeedsOnlyTheCStartup_${fixture}
            PROPERTIES LABELS "runtime;native;entry;host" TIMEOUT 120)
    endforeach()
endif()
