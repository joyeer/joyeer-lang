
add_executable(RuntimeTests
        runtime_test.cpp
        host_test.cpp
        threaded_mode.cpp
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

get_filename_component(joyeer_llvm_bin "${JOYEER_CLANG_EXECUTABLE}" DIRECTORY)
find_program(JOYEER_RUNTIME_NM_EXECUTABLE NAMES llvm-nm llvm-nm-${LLVM_VERSION_MAJOR}
    HINTS "${JOYEER_LLVM_ROOT}/bin" "${LLVM_TOOLS_BINARY_DIR}" "${joyeer_llvm_bin}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT JOYEER_RUNTIME_NM_EXECUTABLE)
    message(FATAL_ERROR "Linux runtime tests need llvm-nm from the LLVM SDK.")
endif()

if(JOYEER_SANITIZE_RUNTIME)
    add_executable(RuntimeAsanProbe asan_probe.c)
    target_compile_options(RuntimeAsanProbe PRIVATE -g -fsanitize=address)
    target_link_options(RuntimeAsanProbe PRIVATE -fsanitize=address)
    target_link_libraries(RuntimeAsanProbe PRIVATE JoyeerAsanRuntime)
    add_dependencies(RuntimeAsanProbe JoyeerToolchain)
    foreach(mode IN ITEMS valid invalid)
        add_test(NAME "RuntimeAddressSanitizer.${mode}"
            COMMAND "${CMAKE_COMMAND}"
                "-DPROBE=$<TARGET_FILE:RuntimeAsanProbe>" "-DMODE=${mode}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/verifyAsanRuntime.cmake")
        set_tests_properties("RuntimeAddressSanitizer.${mode}" PROPERTIES
            LABELS "runtime;sanitizer;native" TIMEOUT 30
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")
    endforeach()
    add_test(NAME RuntimeAddressSanitizer.PlainExecutable
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/asan-isolation"
            "-DEXPECTED_OUTPUT=42\nnative\n8\n42\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties(RuntimeAddressSanitizer.PlainExecutable PROPERTIES
        LABELS "runtime;sanitizer;native" TIMEOUT 60
        ENVIRONMENT "JOYEER_TEST_ASAN_RUNTIME=1")
    foreach(mode IN ITEMS O0 O2)
        add_test(NAME "RuntimeAddressSanitizer.ObjectScope.${mode}"
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DLLVM_NM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}" "-DMODE=${mode}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/verifyAsanObjectScope.cmake")
        set_tests_properties("RuntimeAddressSanitizer.ObjectScope.${mode}" PROPERTIES
            LABELS "runtime;sanitizer;native" TIMEOUT 60)
    endforeach()
endif()

# The whole suite again with one thread sleeping for the run, so every runtime function takes its
# locks (decision 0017): a lock cannot be tested by a program that never starts a thread.
add_test(NAME RuntimeTestsThreadedMode COMMAND RuntimeTests)
set_tests_properties(RuntimeTestsThreadedMode PROPERTIES
        ENVIRONMENT "JOYEER_TEST_THREADED=1"
        LABELS "runtime;threads"
        TIMEOUT 300)

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
        set(host_compiler_args "-O0;${joyeer_test_native_debug}")
    else()
        set(host_compiler_args "-${mode}")
    endif()
    set(host_work_directory "${CMAKE_CURRENT_BINARY_DIR}/portable-host-${mode}")
    file(MAKE_DIRECTORY "${host_work_directory}")
    set(filesystem_output "${CMAKE_CURRENT_BINARY_DIR}/filesystem-${mode}${CMAKE_EXECUTABLE_SUFFIX}")
    add_test(NAME E2EPortableFilesystem${mode}
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/filesystem.joyeer
                    -DOUTPUT_FILE=${filesystem_output}
                    "-DCOMPILER_ARGS=${host_compiler_args}"
                    "-DPROGRAM_ARGUMENT_1=${host_work_directory}"
                    "-DEXPECTED_OUTPUT=filesystem ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set(process_output "${CMAKE_CURRENT_BINARY_DIR}/process-${mode}${CMAKE_EXECUTABLE_SUFFIX}")
    add_test(NAME E2ESynchronousProcess${mode}
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/process.joyeer
                    -DOUTPUT_FILE=${process_output}
                    "-DCOMPILER_ARGS=${host_compiler_args}"
                    "-DPROGRAM_ARGUMENT_1=${process_output}"
                    "-DPROGRAM_ARGUMENT_2=${host_work_directory}"
                    "-DEXPECTED_OUTPUT=child ok\nprocess ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set(module_host_output "${CMAKE_CURRENT_BINARY_DIR}/module-host-${mode}${CMAKE_EXECUTABLE_SUFFIX}")
    add_test(NAME E2EModuleHost${mode}
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
            E2EPortableFilesystem${mode} E2ESynchronousProcess${mode} E2EModuleHost${mode}
            PROPERTIES LABELS "e2e;runtime;host;ownership" TIMEOUT 120
    )
    set_tests_properties(E2ESynchronousProcess${mode} PROPERTIES
            ENVIRONMENT "JOYEER_PROCESS_TEST_INHERITED=parent value=kept")
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
# function to lib/Runtime/core/ (decision 0009, D3).
get_target_property(JOYEER_RUNTIME_CORE_OBJECT JoyeerRuntime JOYEER_CORE_OBJECT)
if(JOYEER_RUNTIME_NM_EXECUTABLE AND JOYEER_RUNTIME_CORE_OBJECT)
    # The calls the core may make: compare, copy and fill (a variable-size
    # memcpy/memset intrinsic lowers to the call, decision 0009 D11) and the operating
    # system's own interface for memory, standard streams and exit (decision 0011). Linux
    # makes system calls and imports nothing else.
    set(joyeer_core_imports "memcmp;memcpy;memset")
    if(WIN32)
        list(APPEND joyeer_core_imports VirtualAlloc VirtualFree GetStdHandle GetFileType WriteFile ReadFile
            GetConsoleMode WriteConsoleW MultiByteToWideChar GetLastError ExitProcess _fltused
            AcquireSRWLockExclusive ReleaseSRWLockExclusive TryAcquireSRWLockExclusive Sleep)
    elseif(APPLE)
        list(APPEND joyeer_core_imports mmap munmap write read isatty __error _exit pthread_sigmask
            os_unfair_lock_lock os_unfair_lock_unlock os_unfair_lock_trylock sleep)
    else()
        # Linux: system calls only.
    endif()
    add_test(NAME RuntimeCoreObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_CORE_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_host_valid_utf8;joyeer_string_equal_abi;joyeer_string_compare_abi;joyeer_panic;joyeer_panic_bytes;joyeer_static_slots;joyeer_alloc;joyeer_realloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_string_destroy_abi;joyeer_byte_to_string_abi;joyeer_string_concat_abi;joyeer_print_string_abi;joyeer_print_int;joyeer_print_uint;joyeer_print_byte;joyeer_print_bool;joyeer_array_create_owned_abi;joyeer_array_clone_abi;joyeer_array_append_owned_abi;joyeer_array_remove_last_abi;joyeer_array_consume_empty_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy;joyeer_native_bytes_init;joyeer_native_strings_init;joyeer_native_bytes_assign;joyeer_native_string_assign;joyeer_native_strings_assign;joyeer_dictionary_create_owned_abi;joyeer_dictionary_clone_abi;joyeer_dictionary_set_owned_abi;joyeer_dictionary_find_abi;joyeer_dictionary_at_abi;joyeer_dictionary_destroy_abi;joyeer_print_float;joyeer_print_double;joyeer_format_double_abi;joyeer_format_float_abi;joyeer_stdout_write;joyeer_stdout_flush;joyeer_stderr_write;joyeer_stdin_read_byte;joyeer_stdin_discard"
            "-DALLOWED_UNDEFINED=${joyeer_core_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeCoreObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)

    # The archive contains only Joyeer objects.
    get_target_property(windows_memory_object JoyeerRuntime JOYEER_WINDOWS_MEMORY_OBJECT)
    if(windows_memory_object)
        add_test(NAME RuntimeWindowsMemoryObjectSymbols
            COMMAND "${CMAKE_COMMAND}"
                "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                "-DOBJECT=${windows_memory_object}"
                "-DEXPECTED_DEFINED=joyeer_windows_move_memory;joyeer_windows_fill_memory;joyeer_windows_compare_memory"
                "-DALLOWED_UNDEFINED=RtlMoveMemory;RtlFillMemory;RtlCompareMemory"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
        set_tests_properties(RuntimeWindowsMemoryObjectSymbols PROPERTIES LABELS "runtime;native" TIMEOUT 30)
    endif()
    find_program(JOYEER_RUNTIME_AR_EXECUTABLE NAMES llvm-ar llvm-ar.exe
        HINTS "${JOYEER_LLVM_ROOT}/bin" "${joyeer_llvm_bin}")
    if(JOYEER_RUNTIME_AR_EXECUTABLE)
        get_filename_component(core_name "${JOYEER_RUNTIME_CORE_OBJECT}" NAME)
        get_target_property(entry_object JoyeerRuntime JOYEER_ENTRY_OBJECT)
        get_filename_component(entry_name "${entry_object}" NAME)
        get_target_property(main_object JoyeerRuntime JOYEER_MAIN_OBJECT)
        get_filename_component(main_name "${main_object}" NAME)
        get_target_property(threads_object JoyeerRuntime JOYEER_THREADS_OBJECT)
        get_filename_component(threads_name "${threads_object}" NAME)
        get_target_property(channels_object JoyeerRuntime JOYEER_CHANNELS_OBJECT)
        get_filename_component(channels_name "${channels_object}" NAME)
        set(runtime_members "${core_name};${entry_name};${main_name};${threads_name};${channels_name}")
        if(windows_memory_object)
            get_filename_component(windows_memory_name "${windows_memory_object}" NAME)
            list(APPEND runtime_members "${windows_memory_name}")
        endif()
        add_test(NAME RuntimeArchiveMembers
            COMMAND "${CMAKE_COMMAND}"
                "-DAR=${JOYEER_RUNTIME_AR_EXECUTABLE}"
                "-DARCHIVE=$<TARGET_FILE:JoyeerRuntime>"
                "-DEXPECTED_MEMBERS=${runtime_members}"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeArchive.cmake")
        set_tests_properties(RuntimeArchiveMembers PROPERTIES LABELS "runtime;native" TIMEOUT 30)
    endif()

    # The entry object defines the program entry point and reaches the core only
    # through its C ABI.
    get_target_property(JOYEER_RUNTIME_ENTRY_OBJECT JoyeerRuntime JOYEER_ENTRY_OBJECT)
    set(joyeer_entry_imports "joyeer_stderr_write;joyeer_stdout_flush;joyeer_main;joyeer_main_uses_arguments;joyeer_panic;joyeer_alloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_array_create_owned_abi;joyeer_array_append_owned_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy")
    if(WIN32)
        list(APPEND joyeer_entry_imports WideCharToMultiByte GetCommandLineW VirtualAlloc ExitProcess)
        set(joyeer_entry_point "wmain;joyeer_runtime_crt_startup_anchor")
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
    # The thread object (decision 0016) defines the thread, mutex and condition exports and calls
    # only the operating system's thread interface, plus the panic of a static-slot bounds trap.
    get_target_property(JOYEER_RUNTIME_THREADS_OBJECT JoyeerRuntime JOYEER_THREADS_OBJECT)
    set(joyeer_threads_defined "joyeer_static_slots;joyeer_thread_trampoline;joyeer_thread_start_abi;joyeer_thread_join_abi;joyeer_mutex_init_abi;joyeer_mutex_lock_abi;joyeer_mutex_unlock_abi;joyeer_mutex_destroy_abi;joyeer_condition_init_abi;joyeer_condition_wait_abi;joyeer_condition_signal_abi;joyeer_condition_broadcast_abi;joyeer_condition_destroy_abi")
    set(joyeer_threads_imports "joyeer_panic;memcmp;memcpy;memset")
    if(WIN32)
        list(APPEND joyeer_threads_imports CreateThread WaitForSingleObject CloseHandle GetLastError
            AcquireSRWLockExclusive ReleaseSRWLockExclusive SleepConditionVariableSRW
            WakeConditionVariable WakeAllConditionVariable _fltused)
    else()
        list(APPEND joyeer_threads_imports pthread_attr_init pthread_attr_setstacksize pthread_attr_destroy
            pthread_create pthread_join pthread_mutex_lock pthread_mutex_unlock pthread_mutex_destroy
            pthread_cond_wait pthread_cond_signal pthread_cond_broadcast pthread_cond_destroy)
        if(APPLE)
            list(APPEND joyeer_threads_imports pthread_mutex_init pthread_cond_init)
        endif()
    endif()
    add_test(NAME RuntimeThreadsObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_THREADS_OBJECT}"
            "-DEXPECTED_DEFINED=${joyeer_threads_defined}"
            "-DALLOWED_UNDEFINED=${joyeer_threads_imports}"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeThreadsObjectSymbols PROPERTIES LABELS "runtime;native;threads" TIMEOUT 30)
    # The channel object (decision 0019) defines the four channel exports and calls only the thread
    # object's mutex and condition, the allocator and the panic.
    get_target_property(JOYEER_RUNTIME_CHANNELS_OBJECT JoyeerRuntime JOYEER_CHANNELS_OBJECT)
    add_test(NAME RuntimeChannelsObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_CHANNELS_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_static_slots;joyeer_channel_create_abi;joyeer_channel_send_abi;joyeer_channel_receive_abi;joyeer_channel_wait_ready_abi;joyeer_channel_close_abi"
            "-DALLOWED_UNDEFINED=joyeer_panic;joyeer_alloc;joyeer_free;joyeer_mutex_init_abi;joyeer_mutex_lock_abi;joyeer_mutex_unlock_abi;joyeer_mutex_destroy_abi;joyeer_condition_init_abi;joyeer_condition_wait_abi;joyeer_condition_signal_abi;joyeer_condition_broadcast_abi;joyeer_condition_destroy_abi;memcmp;memcpy;memset"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyRuntimeCoreObject.cmake")
    set_tests_properties(RuntimeChannelsObjectSymbols PROPERTIES LABELS "runtime;native;channels" TIMEOUT 30)
endif()

# Threads, mutexes and conditions driven from C++ through the exported C ABI. The test starts
# threads with C++ entry functions that never allocate (the runtime is not thread safe until P4).
find_package(Threads REQUIRED)
add_executable(RuntimeThreadTests thread_test.cpp thread_safety_test.cpp channel_test.cpp)
target_include_directories(RuntimeThreadTests PRIVATE ${JOYEER_RUNTIME_INCLUDE_DIR})
target_link_libraries(RuntimeThreadTests PRIVATE
        GTest::gtest_main
        JoyeerRuntime
        Threads::Threads)
joyeer_discover_tests(RuntimeThreadTests
        PROPERTIES LABELS "runtime\;threads"
)

# A Windows program that does not call the C library starts at its own entry point and
# imports the operating system's functions and nothing else; one that calls it keeps
# the C startup (decision 0011).
if(WIN32 AND JOYEER_RUNTIME_NM_EXECUTABLE)
    get_filename_component(joyeer_nm_directory "${JOYEER_RUNTIME_NM_EXECUTABLE}" DIRECTORY)
    find_program(JOYEER_RUNTIME_READOBJ_EXECUTABLE NAMES llvm-readobj llvm-readobj.exe
        HINTS "${joyeer_nm_directory}")
    if(JOYEER_RUNTIME_READOBJ_EXECUTABLE)
        set(own_entry_imports AcquireSRWLockExclusive ExitProcess GetCommandLineW GetConsoleMode GetFileType
            GetLastError GetStdHandle MultiByteToWideChar ReadFile ReleaseSRWLockExclusive Sleep
            TryAcquireSRWLockExclusive VirtualAlloc VirtualFree WideCharToMultiByte WriteConsoleW WriteFile)
        if(JOYEER_WINDOWS_ARCH STREQUAL "arm64")
            list(APPEND own_entry_imports RtlCompareMemory RtlFillMemory RtlMoveMemory)
        endif()
        add_test(NAME RuntimeOwnEntryImportsOnlySystemFunctions
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/target-architecture.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/own-entry-imports.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0;--module-name;runtime.own_entry;--runtime-module;runtime.own_entry;--link-path;${CMAKE_CURRENT_BINARY_DIR}"
                "-DEXPECTED_IMPORTS=${own_entry_imports}"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        add_test(NAME RuntimeCRuntimeProgramKeepsTheCStartup
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/never-import.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/c-startup-imports.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0;--module-name;runtime.never_import;--runtime-module;runtime.never_import"
                "-DREQUIRED_IMPORTS=GetStartupInfoW"
                "-DFORBIDDEN_IMPORTS=CreateThread;SleepConditionVariableSRW;WakeConditionVariable"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        set_tests_properties(RuntimeOwnEntryImportsOnlySystemFunctions RuntimeCRuntimeProgramKeepsTheCStartup
            PROPERTIES LABELS "runtime;native;entry" TIMEOUT 120)
        set(duplicate_startup_output "${CMAKE_CURRENT_BINARY_DIR}/duplicate-c-startup.exe")
        add_test(NAME RuntimeCStartupRejectsDuplicateWmain
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/duplicate-c-startup.joyeer"
                "-DEXTRA_ARGS=-O0;-g0;--module-name;runtime.duplicate_startup;--runtime-core-module;runtime.duplicate_startup;-o;${duplicate_startup_output}"
                "-DOUTPUT_TO_REMOVE=${duplicate_startup_output}"
                "-DEXPECTED_PATTERN=duplicate symbol[^a-zA-Z0-9]+wmain"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake")
        set_tests_properties(RuntimeCStartupRejectsDuplicateWmain PROPERTIES LABELS "runtime;native;entry" TIMEOUT 120)
        # `System` itself (files, processes, standard streams, the legacy reader) calls only
        # the operating system on Windows: a program using each part starts at its own entry
        # point and imports nothing that exists for the C startup or the C heap.
        foreach(fixture IN ITEMS filesystem process read_file stdin_lines)
            add_test(NAME RuntimeSystemProgramHasNoCRuntime_${fixture}
                COMMAND "${CMAKE_COMMAND}"
                    "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                    "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/${fixture}.joyeer"
                    "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/system-${fixture}.exe"
                    "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                    "-DCOMPILER_ARGS=-O0;-g0"
                    "-DFORBIDDEN_IMPORTS=IsProcessorFeaturePresent;GetStartupInfoW;InitializeSListHead;UnhandledExceptionFilter;SetUnhandledExceptionFilter;IsDebuggerPresent;GetProcessHeap;HeapAlloc;HeapFree;GetEnvironmentStringsW"
                    -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
            set_tests_properties(RuntimeSystemProgramHasNoCRuntime_${fixture}
                PROPERTIES LABELS "runtime;native;entry;host" TIMEOUT 120)
        endforeach()
        # A program that starts tasks (decision 0018) imports the thread functions from kernel32
        # and still needs nothing from the C runtime.
        add_test(NAME RuntimeTaskProgramHasNoCRuntime
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/task_basic.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/task-program.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0"
                "-DFORBIDDEN_IMPORTS=IsProcessorFeaturePresent;GetStartupInfoW;InitializeSListHead;UnhandledExceptionFilter;SetUnhandledExceptionFilter;IsDebuggerPresent;GetProcessHeap;HeapAlloc;HeapFree;GetEnvironmentStringsW"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        set_tests_properties(RuntimeTaskProgramHasNoCRuntime
            PROPERTIES LABELS "runtime;native;entry;tasks" TIMEOUT 120)
        # A program that only creates a channel pulls the thread object for its mutex and condition.
        add_test(NAME RuntimeChannelProgramHasNoCRuntime
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/channel_basic.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/channel-program.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0"
                "-DFORBIDDEN_IMPORTS=IsProcessorFeaturePresent;GetStartupInfoW;InitializeSListHead;UnhandledExceptionFilter;SetUnhandledExceptionFilter;IsDebuggerPresent;GetProcessHeap;HeapAlloc;HeapFree;GetEnvironmentStringsW"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        set_tests_properties(RuntimeChannelProgramHasNoCRuntime
            PROPERTIES LABELS "runtime;native;entry;channels" TIMEOUT 120)
    endif()
endif()

# Unoptimized objects can retain unused System entry points. Allow only the
# module's descriptor, directory, process, clock, and socket operations.
if(APPLE)
    if(NOT JOYEER_RUNTIME_NM_EXECUTABLE)
        message(FATAL_ERROR "macOS runtime tests need llvm-nm from the LLVM SDK.")
    endif()
    set(joyeer_macos_runtime_imports
        __error _exit atexit dyld_stub_binder isatty memcmp memcpy memmove memset mmap munmap
        os_unfair_lock_lock os_unfair_lock_trylock os_unfair_lock_unlock pthread_sigmask sleep)
    set(joyeer_macos_system_imports
        _NSGetEnviron accept access bind clock_gettime close closedir connect fcntl filesec_free fstat
        filesec_init filesec_set_property getcwd getpid getsockname getsockopt kevent64 kqueue listen lstat
        mkdir opendir openx_np poll posix_spawn posix_spawn_file_actions_addchdir_np
        posix_spawn_file_actions_destroy posix_spawn_file_actions_init proc_pidpath read readdir_r readlink recv rename rmdir
        send setsockopt socket unlink waitpid write)
    foreach(fixture IN ITEMS filesystem process read_file stdin_lines)
        set(required_imports write)
        if(fixture STREQUAL "filesystem" OR fixture STREQUAL "read_file")
            list(APPEND required_imports openx_np)
        endif()
        foreach(level IN ITEMS 0 2)
            set(test_name "RuntimeSystemProgramHasNoStdio_${fixture}O${level}")
            add_test(NAME "${test_name}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                    "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/${fixture}.joyeer"
                    "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/system-${fixture}-O${level}"
                    "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                    -DMACHO=ON
                    "-DCOMPILER_ARGS=-O${level};-g0"
                    "-DREQUIRED_IMPORTS=${required_imports}"
                    "-DALLOWED_IMPORTS=${joyeer_macos_runtime_imports};${joyeer_macos_system_imports}"
                    -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
            set_tests_properties("${test_name}" PROPERTIES LABELS "runtime;native;entry;host" TIMEOUT 120)
        endforeach()
    endforeach()
endif()

# On Linux `System` calls the kernel directly (decision 0011): a program that uses each
# part of it needs, from the C library, only the startup the linker adds and the compiler
# support functions.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(joyeer_linux_startup_imports __libc_start_main __cxa_atexit __gmon_start__
        _ITM_registerTMCloneTable _ITM_deregisterTMCloneTable memcpy memset memmove memcmp)
    set(joyeer_linux_import_options)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|AARCH64|arm64|ARM64)$")
        # AArch64 glibc's crt1.o calls abort if __libc_start_main returns.
        list(APPEND joyeer_linux_startup_imports abort)
        find_program(JOYEER_RUNTIME_OBJDUMP_EXECUTABLE NAMES llvm-objdump llvm-objdump-${LLVM_VERSION_MAJOR}
            HINTS "${JOYEER_LLVM_ROOT}/bin" "${LLVM_TOOLS_BINARY_DIR}" "${joyeer_llvm_bin}" REQUIRED)
        list(APPEND joyeer_linux_import_options "-DSTARTUP_OBJDUMP=${JOYEER_RUNTIME_OBJDUMP_EXECUTABLE}")
        foreach(kind IN ITEMS Call Address)
            if(kind STREQUAL "Call")
                set(fixture abort-import)
            else()
                set(fixture abort-address-import)
            endif()
            foreach(level IN ITEMS 0 2)
                set(test_name "RuntimeStartupImportsRejectProgramAbort${kind}O${level}")
                add_test(NAME "${test_name}"
                    COMMAND "${CMAKE_COMMAND}"
                        "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                        "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/${fixture}.joyeer"
                        "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/${fixture}-O${level}"
                        "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                        "-DCOMPILER_ARGS=-O${level};-g0;--module-name;test.abort_import;--runtime-module;test.abort_import"
                        "-DALLOWED_IMPORTS=${joyeer_linux_startup_imports}"
                        ${joyeer_linux_import_options}
                        -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
                set_tests_properties("${test_name}" PROPERTIES LABELS "runtime;native;entry" TIMEOUT 120
                    PASS_REGULAR_EXPRESSION "abort reference outside _start")
            endforeach()
        endforeach()
    endif()
    set(joyeer_linux_thread_imports pthread_attr_init pthread_attr_setstacksize pthread_attr_destroy
        pthread_create pthread_join pthread_mutex_lock pthread_mutex_unlock pthread_mutex_destroy
        pthread_mutex_init pthread_cond_wait pthread_cond_signal pthread_cond_broadcast pthread_cond_destroy pthread_cond_init)
    foreach(fixture IN ITEMS filesystem process read_file stdin_lines)
        add_test(NAME RuntimeSystemProgramNeedsOnlyTheCStartup_${fixture}
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/${fixture}.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/system-${fixture}"
                "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0"
                "-DALLOWED_IMPORTS=${joyeer_linux_startup_imports}"
                ${joyeer_linux_import_options}
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
        set_tests_properties(RuntimeSystemProgramNeedsOnlyTheCStartup_${fixture}
            PROPERTIES LABELS "runtime;native;entry;host" TIMEOUT 120)
    endforeach()
    # A program that starts tasks (decision 0018) also needs the thread object's pthread functions
    # and nothing else from the C library; every other program's gate above is unchanged, which
    # keeps the thread object out of programs that never start a thread.
    add_test(NAME RuntimeTaskProgramNeedsOnlyThePthreadFunctions
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/task_basic.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/task-program"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DCOMPILER_ARGS=-O0;-g0"
            "-DALLOWED_IMPORTS=${joyeer_linux_startup_imports};${joyeer_linux_thread_imports}"
            ${joyeer_linux_import_options}
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
    set_tests_properties(RuntimeTaskProgramNeedsOnlyThePthreadFunctions
        PROPERTIES LABELS "runtime;native;entry;tasks" TIMEOUT 120)
    # A program that only creates a channel links the thread object (its mutex and condition) and
    # -lpthread, and needs nothing else from the C library.
    add_test(NAME RuntimeChannelProgramNeedsOnlyThePthreadFunctions
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/channel_basic.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/channel-program"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DCOMPILER_ARGS=-O0;-g0"
            "-DALLOWED_IMPORTS=${joyeer_linux_startup_imports};${joyeer_linux_thread_imports}"
            ${joyeer_linux_import_options}
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
    set_tests_properties(RuntimeChannelProgramNeedsOnlyThePthreadFunctions
        PROPERTIES LABELS "runtime;native;entry;channels" TIMEOUT 120)
endif()
