get_target_property(compression_package JoyeerCompression JOYEER_PACKAGE_DIRECTORY)
add_executable(CompressionOracle oracle.c)
target_include_directories(CompressionOracle PRIVATE "${JOYEER_SRC_DIR}")
target_link_libraries(CompressionOracle PRIVATE JoyeerCompression JoyeerRuntime)
set_target_properties(CompressionOracle PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded)
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME Compression.Interoperability
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_interop.py"
            "$<TARGET_FILE:CompressionOracle>")
    set_tests_properties(Compression.Interoperability PROPERTIES LABELS "compression;native" TIMEOUT 90)
endif()
# Compression imports System (readFilePrefix), and these programs compile with
# --no-stdlib, so System and the Foundation it needs are named explicitly.
set(compression_system_args "")
foreach(source IN ITEMS Foundation/text.joyeer Foundation/checksum.joyeer
        System/path.joyeer System/messages.joyeer System/filesystem.joyeer
        System/filesystem_windows.joyeer System/filesystem_posix.joyeer
        System/legacy_file.joyeer System/process.joyeer System/process_windows.joyeer
        System/process_posix.joyeer System/stdio.joyeer System/cruntime.joyeer)
    get_filename_component(module "${source}" DIRECTORY)
    string(APPEND compression_system_args
        "--module-source;${module}=${JOYEER_SRC_DIR}/library/${source};")
endforeach()
string(APPEND compression_system_args "--runtime-module;System;")
foreach(profile IN ITEMS Debug Optimized)
    if(profile STREQUAL Debug)
        set(flags -O0 -gfull)
    else()
        set(flags -O2 -g0)
    endif()
    add_test(NAME "Compression.Zip${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/main.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/zip-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--no-stdlib;--module-name;CompressionTests;${compression_system_args}--module-source;Compression=${JOYEER_SRC_DIR}/library/Compression/compression.joyeer;--runtime-module;Compression;--link-library;$<TARGET_FILE:JoyeerCompression>;${flags}"
            "-DEXPECTED_OUTPUT=Compression ZIP tests passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("Compression.Zip${profile}" PROPERTIES LABELS "compression;native" TIMEOUT 90)
    # A real JDK-built jar read from disk; the program runs next to its fixtures.
    add_test(NAME "Compression.Jar${profile}"
        COMMAND "${CMAKE_COMMAND}"
            "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/jar_test.joyeer"
            "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/jar-${profile}${CMAKE_EXECUTABLE_SUFFIX}"
            "-DCOMPILER_ARGS=--no-stdlib;--module-name;CompressionJarTests;${compression_system_args}--module-source;Compression=${JOYEER_SRC_DIR}/library/Compression/compression.joyeer;--runtime-module;Compression;--link-library;$<TARGET_FILE:JoyeerCompression>;${flags}"
            "-DPROGRAM_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
            "-DEXPECTED_OUTPUT=META-INF/ dir 0 0 0\nMETA-INF/MANIFEST.MF file 85 84 1159317140\ndemo/ dir 0 0 0\ndemo/Hello.class file 437 303 4071747383\ndemo/empty.txt file 0 0 0\ndemo/greeting.txt file 86 49 1575737333\nJar tests passed\n"
            -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
    set_tests_properties("Compression.Jar${profile}" PROPERTIES LABELS "compression;native" TIMEOUT 90)
endforeach()
if(JOYEER_BUILD_FERN)
    add_test(NAME Compression.Fern
        COMMAND "${JOYEER_FERN_EXECUTABLE}" test
            --compiler "$<TARGET_FILE:joyeer>"
            --workspace "${compression_package}/workspace.toml")
    set_tests_properties(Compression.Fern PROPERTIES LABELS "compression;native;fern" TIMEOUT 90)
endif()
