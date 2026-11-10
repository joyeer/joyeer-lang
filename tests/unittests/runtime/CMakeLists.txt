
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
        set(host_compiler_args "-O0;-gfull")
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
            GetConsoleMode WriteConsoleW MultiByteToWideChar GetLastError ExitProcess _fltused
            AcquireSRWLockExclusive ReleaseSRWLockExclusive TryAcquireSRWLockExclusive Sleep)
    elseif(APPLE)
        list(APPEND joyeer_core_imports mmap munmap write read isatty __error _exit
            os_unfair_lock_lock os_unfair_lock_unlock os_unfair_lock_trylock sleep)
    else()
        # Linux: system calls only.
    endif()
    add_test(NAME RuntimeCoreObjectSymbols
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
            "-DOBJECT=${JOYEER_RUNTIME_CORE_OBJECT}"
            "-DEXPECTED_DEFINED=joyeer_host_valid_utf8;joyeer_string_equal_abi;joyeer_string_compare_abi;joyeer_panic;joyeer_static_slots;joyeer_alloc;joyeer_realloc;joyeer_free;joyeer_runtime_active_allocations;joyeer_string_clone_abi;joyeer_string_destroy_abi;joyeer_byte_to_string_abi;joyeer_string_concat_abi;joyeer_print_string_abi;joyeer_print_int;joyeer_print_uint;joyeer_print_byte;joyeer_print_bool;joyeer_array_create_owned_abi;joyeer_array_clone_abi;joyeer_array_append_owned_abi;joyeer_array_remove_last_abi;joyeer_array_consume_empty_abi;joyeer_array_destroy_abi;joyeer_native_string_element_clone;joyeer_native_string_element_destroy;joyeer_native_bytes_init;joyeer_native_strings_init;joyeer_native_bytes_assign;joyeer_native_string_assign;joyeer_native_strings_assign;joyeer_dictionary_create_owned_abi;joyeer_dictionary_clone_abi;joyeer_dictionary_set_owned_abi;joyeer_dictionary_find_abi;joyeer_dictionary_at_abi;joyeer_dictionary_destroy_abi;joyeer_print_float;joyeer_print_double;joyeer_format_double_abi;joyeer_format_float_abi;joyeer_stdout_write;joyeer_stdout_flush;joyeer_stderr_write;joyeer_stdin_read_byte;joyeer_stdin_discard"
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
        get_target_property(threads_object JoyeerRuntime JOYEER_THREADS_OBJECT)
        get_filename_component(threads_name "${threads_object}" NAME)
        add_test(NAME RuntimeArchiveMembers
            COMMAND "${CMAKE_COMMAND}"
                "-DAR=${JOYEER_RUNTIME_AR_EXECUTABLE}"
                "-DARCHIVE=$<TARGET_FILE:JoyeerRuntime>"
                "-DEXPECTED_MEMBERS=${core_name};${entry_name};${main_name};${threads_name}"
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
endif()

# Threads, mutexes and conditions driven from C++ through the exported C ABI. The test starts
# threads with C++ entry functions that never allocate (the runtime is not thread safe until P4).
find_package(Threads REQUIRED)
add_executable(RuntimeThreadTests thread_test.cpp thread_safety_test.cpp)
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
        add_test(NAME RuntimeOwnEntryImportsOnlySystemFunctions
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/c-interop/target-architecture.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/own-entry-imports.exe"
                "-DREADOBJ=${JOYEER_RUNTIME_READOBJ_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0;--module-name;runtime.own_entry;--runtime-module;runtime.own_entry"
                "-DEXPECTED_IMPORTS=AcquireSRWLockExclusive;ExitProcess;GetCommandLineW;GetConsoleMode;GetFileType;GetLastError;GetStdHandle;MultiByteToWideChar;ReadFile;ReleaseSRWLockExclusive;Sleep;TryAcquireSRWLockExclusive;VirtualAlloc;VirtualFree;WideCharToMultiByte;WriteConsoleW;WriteFile"
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
                "-DINPUT_FILE=${JOYEER_SOURCE_DIR}/tests/e2e/${fixture}.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/system-${fixture}"
                "-DNM=${JOYEER_RUNTIME_NM_EXECUTABLE}"
                "-DCOMPILER_ARGS=-O0;-g0"
                "-DALLOWED_IMPORTS=__libc_start_main;__cxa_atexit;__gmon_start__;_ITM_registerTMCloneTable;_ITM_deregisterTMCloneTable;memcpy;memset;memmove;memcmp"
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
            "-DALLOWED_IMPORTS=__libc_start_main;__cxa_atexit;__gmon_start__;_ITM_registerTMCloneTable;_ITM_deregisterTMCloneTable;memcpy;memset;memmove;memcmp;pthread_attr_init;pthread_attr_setstacksize;pthread_attr_destroy;pthread_create;pthread_join;pthread_mutex_lock;pthread_mutex_unlock;pthread_mutex_destroy;pthread_mutex_init;pthread_cond_wait;pthread_cond_signal;pthread_cond_broadcast;pthread_cond_destroy;pthread_cond_init"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyProgramImports.cmake")
    set_tests_properties(RuntimeTaskProgramNeedsOnlyThePthreadFunctions
        PROPERTIES LABELS "runtime;native;entry;tasks" TIMEOUT 120)
endif()
