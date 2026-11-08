include(CheckCSourceCompiles)
check_c_source_compiles("#include <limits.h>
#if CHAR_BIT != 8 || CHAR_MIN >= 0
#error unsupported signed char model
#endif
    int main(void) { return 0; }" JOYEER_CINTEROP_SIGNED_CHAR)
if(JOYEER_CINTEROP_SIGNED_CHAR)
    set(CINTEROP_CHAR_TYPE Int8)
else()
    check_c_source_compiles("#include <limits.h>
#if CHAR_BIT != 8 || CHAR_MIN != 0 || CHAR_MAX != 255
#error unsupported unsigned char model
#endif
        int main(void) { return 0; }" JOYEER_CINTEROP_UNSIGNED_CHAR)
    if(NOT JOYEER_CINTEROP_UNSIGNED_CHAR)
        message(FATAL_ERROR "Cannot establish the target C plain-char representation")
    endif()
    set(CINTEROP_CHAR_TYPE UInt8)
endif()
if(WIN32)
    set(CINTEROP_LONG_TYPE Int32)
    set(CINTEROP_ULONG_TYPE UInt32)
else()
    set(CINTEROP_LONG_TYPE Int64)
    set(CINTEROP_ULONG_TYPE UInt64)
endif()
configure_file(platform-scalars.joyeer.in platform-scalars.joyeer @ONLY)

add_library(CInteropNativeStatic STATIC native.c native-scalars.c)
add_library(CInteropNativeShared SHARED native.c native-scalars.c)
add_library(CInteropBuffers STATIC native-buffers.c)
target_include_directories(CInteropBuffers PRIVATE "${JOYEER_SRC_DIR}")
set_target_properties(CInteropBuffers PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
add_library(CInteropPointers STATIC native-pointers.c)
set_target_properties(CInteropPointers PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
target_compile_definitions(CInteropNativeShared PRIVATE CINTEROP_SHARED)
foreach(provider IN ITEMS CInteropNativeStatic CInteropNativeShared)
    target_compile_definitions(${provider} PRIVATE
        "CINTEROP_CHAR_SIGNED=$<BOOL:${JOYEER_CINTEROP_SIGNED_CHAR}>")
endforeach()
set_target_properties(CInteropNativeStatic CInteropNativeShared PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)

get_filename_component(joyeer_llvm_bin "${JOYEER_CLANG_EXECUTABLE}" DIRECTORY)
find_program(JOYEER_LLVM_NM_EXECUTABLE NAMES llvm-nm llvm-nm.exe
    HINTS "${JOYEER_LLVM_ROOT}/bin" "${joyeer_llvm_bin}")

foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.Buffers${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/buffers.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/buffers-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.buffers;--runtime-module;runtime.buffers;--link-library;$<TARGET_FILE:CInteropBuffers>;${compile_flags}"
            "-DEXPECTED_OUTPUT=native buffer ownership passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.Buffers${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
    add_test(NAME "JoyeerCInterop.Pointers${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/pointers.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/pointers-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.pointers;--runtime-module;runtime.pointers;--link-library;$<TARGET_FILE:CInteropPointers>;${compile_flags}"
            "-DEXPECTED_OUTPUT=native pointer buffers passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.Pointers${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
    add_test(NAME "JoyeerCInterop.Intrinsics${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/intrinsics.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/intrinsics-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.intrinsics;--runtime-module;runtime.intrinsics;${compile_flags}"
            "-DEXPECTED_OUTPUT=intrinsics passed
"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.Intrinsics${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
    add_test(NAME "JoyeerCInterop.ScalarValues${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/scalar-values.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/scalar-values-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=${compile_flags}"
            "-DEXPECTED_OUTPUT=-128\n65535\n-2147483648\n18446744073709551615\ntrue\ntrue\n0\n127\n1.5\n2.25\n3.5\n1.25\ntrue\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.ScalarValues${profile}" PROPERTIES
        LABELS "c-interop;native" TIMEOUT 60)
    foreach(operation IN ITEMS addition division remainder unsigned)
        if(operation STREQUAL unsigned)
            set(trap_pattern "integer division by zero")
        elseif(operation STREQUAL addition)
            set(trap_pattern "integer addition overflow")
        else()
            set(trap_pattern "integer ${operation} overflow")
        endif()
        add_test(NAME "JoyeerCInterop.ScalarTrap${operation}${profile}"
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/scalar-traps.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/scalar-trap-${operation}-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
                "-DCOMPILER_ARGS=${compile_flags}"
                "-DPROGRAM_ARGUMENT_1=${operation}"
                "-DEXPECTED_OUTPUT="
                "-DEXPECTED_EXIT_CODE=1"
                "-DEXPECTED_ERROR_PATTERN=${trap_pattern}"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
        set_tests_properties("JoyeerCInterop.ScalarTrap${operation}${profile}" PROPERTIES
            LABELS "c-interop;native" TIMEOUT 60)
    endforeach()
    set(objects)
    foreach(unit IN ITEMS roundtrip second scalars platform-scalars indirect static-slots-first static-slots-second never-export)
        if(unit STREQUAL platform-scalars)
            set(unit_source "${CMAKE_CURRENT_BINARY_DIR}/${unit}.joyeer")
        else()
            set(unit_source "${CMAKE_CURRENT_SOURCE_DIR}/${unit}.joyeer")
        endif()
        string(REPLACE "-" "_" module_name "${unit}")
        set(object "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${profile}/${unit}${CMAKE_C_OUTPUT_EXTENSION}")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${profile}"
            COMMAND "$<TARGET_FILE:joyeer>"
                --module-name "runtime.${module_name}" --runtime-module "runtime.${module_name}"
                ${compile_flags} --emit-runtime-object "${object}"
                "${unit_source}"
            DEPENDS joyeer JoyeerBackend "${unit_source}"
            COMMENT "Compiling ${profile} C interop runtime unit ${unit}"
            VERBATIM)
        set_source_files_properties("${object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
        list(APPEND objects "${object}")
    endforeach()
    add_library(CInteropJoyeer${profile} STATIC ${objects})
    set_target_properties(CInteropJoyeer${profile} PROPERTIES LINKER_LANGUAGE C)

    # The same Joyeer imports work with either provider; library choice and
    # loader paths belong to these build targets, not to source annotations.
    foreach(linkage IN ITEMS Static Shared)
        set(target "CInteropRoundTrip${linkage}${profile}")
        add_executable(${target} caller.c scalar-caller.c)
        target_link_libraries(${target} PRIVATE
            CInteropJoyeer${profile} CInteropNative${linkage})
        set_target_properties(${target} PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded
            BUILD_RPATH_USE_ORIGIN TRUE)
        add_test(NAME "JoyeerCInterop.${linkage}${profile}" COMMAND ${target})
        set_tests_properties("JoyeerCInterop.${linkage}${profile}" PROPERTIES
            LABELS "c-interop;native" TIMEOUT 30)
    endforeach()

    if(JOYEER_LLVM_NM_EXECUTABLE)
        add_test(NAME "JoyeerCInterop.Symbols${profile}"
            COMMAND "${CMAKE_COMMAND}"
                "-DLLVM_NM=${JOYEER_LLVM_NM_EXECUTABLE}"
                "-DOBJECT_FILE=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${profile}/roundtrip${CMAKE_C_OUTPUT_EXTENSION}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-symbols.cmake")
        set_tests_properties("JoyeerCInterop.Symbols${profile}" PROPERTIES
            LABELS "c-interop;native" TIMEOUT 30)
    endif()
endforeach()

add_executable(CInteropMissingLibrary EXCLUDE_FROM_ALL caller.c scalar-caller.c)
target_link_libraries(CInteropMissingLibrary PRIVATE CInteropJoyeerDebug)
set_target_properties(CInteropMissingLibrary PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
add_test(NAME JoyeerCInterop.MissingLibrary
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
        "-DBUILD_CONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-missing-library.cmake")
set_tests_properties(JoyeerCInterop.MissingLibrary PROPERTIES
    LABELS "c-interop;native" RUN_SERIAL TRUE TIMEOUT 60)

add_test(NAME JoyeerCInterop.CompilationContext
    COMMAND "${CMAKE_COMMAND}"
        "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.joyeer"
        "-DCONFLICT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/conflicting-runtime-symbol.joyeer"
        "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-context.cmake")
set_tests_properties(JoyeerCInterop.CompilationContext PROPERTIES
    LABELS "c-interop;native" TIMEOUT 60)

# Indirect calls through an address held as an Int (spec 19.4.6): the runtime's
# arrays and dictionaries call generated element clone and destroy functions.
add_library(CInteropCallbacks STATIC native-callbacks.c)
set_target_properties(CInteropCallbacks PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.Callbacks${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/callbacks.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/callbacks-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.callbacks;--runtime-module;runtime.callbacks;--link-library;$<TARGET_FILE:CInteropCallbacks>;${compile_flags}"
            "-DEXPECTED_OUTPUT=indirect calls passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.Callbacks${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
endforeach()

# Measures an indirect call from Joyeer against a C loop; not a test. Run
#   cmake --build <dir> --target cinterop-indirect-benchmark
# in a Release tree.
add_executable(CInteropIndirectBenchmark indirect-benchmark.c)
target_link_libraries(CInteropIndirectBenchmark PRIVATE CInteropJoyeerOptimized JoyeerRuntime)
set_target_properties(CInteropIndirectBenchmark PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
add_custom_target(cinterop-indirect-benchmark
    COMMAND CInteropIndirectBenchmark
    DEPENDS CInteropIndirectBenchmark
    VERBATIM)

# The address of a string literal's NUL-terminated constant data (spec 19.4.6).
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.LiteralAddress${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/literal-address.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/literal-address-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.literal;--runtime-module;runtime.literal;--link-library;$<TARGET_FILE:CInteropCallbacks>;${compile_flags}"
            "-DEXPECTED_OUTPUT=literal addresses passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.LiteralAddress${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
endforeach()

# The address of a named function, stored as a callback (spec 19.4.6).
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.SymbolAddress${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/symbol-address.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/symbol-address-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.symbol;--runtime-module;runtime.symbol;--link-library;$<TARGET_FILE:CInteropCallbacks>;${compile_flags}"
            "-DEXPECTED_OUTPUT=symbol addresses passed
"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.SymbolAddress${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
endforeach()

# Static storage: a table of zeroed 8-byte slots shared by every unit in the
# program (spec 19.4.6), reached by index with a range check.
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.StaticSlots${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/static-slots.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/static-slots-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.static_slots;--runtime-module;runtime.static_slots;${compile_flags}"
            "-DEXPECTED_OUTPUT=static slots passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.StaticSlots${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
    foreach(case IN ITEMS high negative maximum)
        add_test(NAME "JoyeerCInterop.StaticSlotTrap${case}${profile}"
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/static-slot-traps.joyeer"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/static-slot-trap-${case}-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
                "-DCOMPILER_ARGS=--module-name;runtime.static_slot_traps;--runtime-module;runtime.static_slot_traps;${compile_flags}"
                "-DPROGRAM_ARGUMENT_1=${case}"
                "-DEXPECTED_OUTPUT="
                "-DEXPECTED_EXIT_CODE=1"
                "-DEXPECTED_ERROR_PATTERN=static slot index out of range"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
        set_tests_properties("JoyeerCInterop.StaticSlotTrap${case}${profile}" PROPERTIES
            LABELS "c-interop;native" TIMEOUT 60)
    endforeach()
    # Two separately compiled objects must share the one table.
    set(target "CInteropStaticSlots${profile}")
    add_executable(${target} static-slots-caller.c)
    target_link_libraries(${target} PRIVATE CInteropJoyeer${profile} JoyeerRuntime)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_test(NAME "JoyeerCInterop.SharedStaticSlots${profile}" COMMAND ${target})
    set_tests_properties("JoyeerCInterop.SharedStaticSlots${profile}" PROPERTIES
        LABELS "c-interop;native" TIMEOUT 30)
endforeach()

# Never as the result of a C binding: a C function that does not return, and a
# Joyeer export that does not (spec 19.4.6).
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.NeverImport${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/never-import.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/never-import-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.never_import;--runtime-module;runtime.never_import;${compile_flags}"
            "-DEXPECTED_OUTPUT=before exit\n"
            "-DEXPECTED_EXIT_CODE=3"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.NeverImport${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
    set(target "CInteropNeverExport${profile}")
    add_executable(${target} never-export-caller.c)
    target_link_libraries(${target} PRIVATE CInteropJoyeer${profile} JoyeerRuntime)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_test(NAME "JoyeerCInterop.NeverExport${profile}" COMMAND ${target})
    set_tests_properties("JoyeerCInterop.NeverExport${profile}" PROPERTIES
        PASS_REGULAR_EXPRESSION "never export before" LABELS "c-interop;native" TIMEOUT 30)
endforeach()

# The address of a C data symbol with @c_data (spec 19.4.6).
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.DataSymbol${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/data-symbol.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/data-symbol-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.data_symbol;--runtime-module;runtime.data_symbol;--link-library;$<TARGET_FILE:CInteropCallbacks>;${compile_flags}"
            "-DEXPECTED_OUTPUT=data symbols passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.DataSymbol${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
endforeach()

# Bulk memory intrinsics (spec 19.4.5): copy, overlapping move, fill.
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(compile_flags -O0 -gfull)
    else()
        set(compile_flags -O2 -g0)
    endif()
    add_test(NAME "JoyeerCInterop.MemoryIntrinsics${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/memory-intrinsics.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/memory-intrinsics-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--module-name;runtime.memory;--runtime-module;runtime.memory;${compile_flags}"
            "-DEXPECTED_OUTPUT=memory intrinsics passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("JoyeerCInterop.MemoryIntrinsics${profile}" PROPERTIES LABELS "c-interop;native" TIMEOUT 60)
endforeach()
