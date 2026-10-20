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
    foreach(unit IN ITEMS roundtrip second scalars platform-scalars)
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
