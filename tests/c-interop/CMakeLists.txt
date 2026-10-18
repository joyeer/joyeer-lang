add_library(CInteropNativeStatic STATIC native.c)
add_library(CInteropNativeShared SHARED native.c)
target_compile_definitions(CInteropNativeShared PRIVATE CINTEROP_SHARED)
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
    set(objects)
    foreach(unit IN ITEMS roundtrip second)
        set(object "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${profile}/${unit}${CMAKE_C_OUTPUT_EXTENSION}")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${profile}"
            COMMAND "$<TARGET_FILE:joyeer>"
                --module-name "runtime.${unit}" --runtime-module "runtime.${unit}"
                ${compile_flags} --emit-runtime-object "${object}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${unit}.joyeer"
            DEPENDS joyeer JoyeerBackend "${CMAKE_CURRENT_SOURCE_DIR}/${unit}.joyeer"
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
        add_executable(${target} caller.c)
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

add_executable(CInteropMissingLibrary EXCLUDE_FROM_ALL caller.c)
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
        "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-context.cmake")
set_tests_properties(JoyeerCInterop.CompilationContext PROPERTIES
    LABELS "c-interop;native" TIMEOUT 60)
