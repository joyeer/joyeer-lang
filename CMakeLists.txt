cmake_minimum_required(VERSION 3.20)

project (joyeer 
        VERSION 0.0.1
        LANGUAGES C CXX)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "Joyeer on Windows requires the MSVC compiler from Visual Studio. "
        "Run CMake from a Visual Studio Developer shell.")
endif()

if(WIN32)
    string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _joyeer_windows_arch)
    if(_joyeer_windows_arch STREQUAL "arm64")
        set(JOYEER_WINDOWS_ARCH arm64)
        set(JOYEER_WINDOWS_MACHINE IMAGE_FILE_MACHINE_ARM64)
        set(_joyeer_dia_arch arm64)
        set(_joyeer_llvm_arch AArch64)
    elseif(_joyeer_windows_arch STREQUAL "x64")
        set(JOYEER_WINDOWS_ARCH x64)
        set(JOYEER_WINDOWS_MACHINE IMAGE_FILE_MACHINE_AMD64)
        set(_joyeer_dia_arch amd64)
        set(_joyeer_llvm_arch X86)
    else()
        message(FATAL_ERROR
            "Joyeer on Windows supports only x64 and ARM64 MSVC targets. "
            "Use a matching Visual Studio Developer shell.")
    endif()
endif()

cmake_policy(SET CMP0110 NEW)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CTEST_OUTPUT_ON_FAILURE TRUE)

# custom cmake module
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cmake")

# enable the testing
include(CTest)
enable_language(CXX)

option(JOYEER_BUILD_UNITTESTS "Build Joyeer C++ unit tests" ON)
option(JOYEER_BUILD_FERN "Bootstrap the Joyeer-written fern project tool" ON)
option(JOYEER_BUILD_COMPRESSION "Build the miniz-backed Compression standard module (needed by the root workspace for moss)" ON)
option(JOYEER_BUILD_MOSS "Build the Joyeer-written moss JVM inspection tool (needs fern and Compression)" ON)

if(JOYEER_BUILD_FERN AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "fern bootstrapping must execute the newly built Joyeer compiler. "
        "Cross-compiling builds must set JOYEER_BUILD_FERN=OFF.")
endif()

set(JOYEER_LLVM_ROOT "$ENV{LLVM_HOME}" CACHE PATH
    "Root of the LLVM/LLD 22.1.8 development SDK")

if(WIN32)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_HTTP OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_ICONV OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_LZMA OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_MODULES OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_PYTHON OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBXML2_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        joyeer_libxml2
        URL https://download.gnome.org/sources/libxml2/2.14/libxml2-2.14.5.tar.xz
        URL_HASH SHA256=03d006f3537616833c16c53addcdc32a0eb20e55443cba4038307e3fa7d8d44b
        SOURCE_SUBDIR joyeer-no-automatic-add-subdirectory
    )
    FetchContent_MakeAvailable(joyeer_libxml2)
    FetchContent_GetProperties(joyeer_libxml2
        SOURCE_DIR JOYEER_LIBXML2_SOURCE_DIR
        BINARY_DIR JOYEER_LIBXML2_BINARY_DIR)
    add_subdirectory(
        "${JOYEER_LIBXML2_SOURCE_DIR}"
        "${JOYEER_LIBXML2_BINARY_DIR}"
        EXCLUDE_FROM_ALL)
    set_target_properties(LibXml2 PROPERTIES
        MSVC_RUNTIME_LIBRARY MultiThreaded)
    set(LIBXML2_INCLUDE_DIR "${JOYEER_LIBXML2_SOURCE_DIR}/include"
        CACHE PATH "" FORCE)
    set(LIBXML2_LIBRARY LibXml2 CACHE STRING "" FORCE)

endif()

find_package(LLVM 22.1.8 EXACT CONFIG REQUIRED
    HINTS
        "${JOYEER_LLVM_ROOT}/lib/cmake/llvm"
        "/opt/homebrew/opt/llvm/lib/cmake/llvm"
        "/usr/local/opt/llvm/lib/cmake/llvm")
if(WIN32 AND NOT LLVM_NATIVE_ARCH STREQUAL _joyeer_llvm_arch)
    message(FATAL_ERROR
        "The LLVM SDK must match the MSVC ${JOYEER_WINDOWS_ARCH} target. "
        "Found LLVM_NATIVE_ARCH=${LLVM_NATIVE_ARCH}. "
        "Set JOYEER_LLVM_ROOT to a matching LLVM/LLD 22.1.8 development SDK "
        "and configure in a fresh build directory.")
endif()
find_package(LLD 22.1.8 EXACT CONFIG REQUIRED
    HINTS
        "${JOYEER_LLVM_ROOT}/lib/cmake/lld"
        "/opt/homebrew/opt/lld/lib/cmake/lld"
        "/opt/homebrew/opt/llvm/lib/cmake/lld"
        "/usr/local/opt/lld/lib/cmake/lld"
        "/usr/local/opt/llvm/lib/cmake/lld")
if(UNIX AND NOT APPLE)
    find_package(Clang 22.1.8 EXACT CONFIG REQUIRED
        HINTS "${JOYEER_LLVM_ROOT}/lib/cmake/clang")
endif()
message(STATUS "Joyeer embedded LLVM SDK: ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
message(STATUS "Joyeer embedded LLD SDK: ${LLD_DIR}")

if(WIN32)
    find_library(JOYEER_DIA_GUIDS_LIBRARY
        NAMES diaguids
        HINTS "$ENV{VSINSTALLDIR}/DIA SDK/lib/${_joyeer_dia_arch}"
        REQUIRED
        NO_DEFAULT_PATH)
    get_target_property(_joyeer_pdb_link_libraries
        LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
    set(_joyeer_pdb_link_libraries_fixed)
    foreach(_joyeer_pdb_link_library IN LISTS _joyeer_pdb_link_libraries)
        if(_joyeer_pdb_link_library MATCHES "diaguids\\.lib$")
            list(APPEND _joyeer_pdb_link_libraries_fixed
                "${JOYEER_DIA_GUIDS_LIBRARY}")
        else()
            list(APPEND _joyeer_pdb_link_libraries_fixed
                "${_joyeer_pdb_link_library}")
        endif()
    endforeach()
    set_target_properties(LLVMDebugInfoPDB PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_joyeer_pdb_link_libraries_fixed}")
endif()

find_program(JOYEER_CLANG_EXECUTABLE
        NAMES clang clang-22
        HINTS
            "${JOYEER_LLVM_ROOT}/bin"
            "$ENV{LLVM_HOME}/bin"
            "$ENV{ProgramFiles}/LLVM/bin"
            "/usr/local/opt/llvm/bin"
            "/opt/homebrew/opt/llvm/bin"
        DOC "Clang driver used to independently verify generated LLVM IR"
)
if(JOYEER_CLANG_EXECUTABLE)
    message(STATUS "Joyeer LLVM test driver: ${JOYEER_CLANG_EXECUTABLE}")
else()
    message(STATUS "Joyeer LLVM test driver not found; Clang oracle tests are unavailable")
endif()

if(APPLE)
    find_program(JOYEER_XCRUN_EXECUTABLE NAMES xcrun REQUIRED)
    set(JOYEER_MACOS_SDK_PATH "" CACHE PATH
            "macOS SDK used by the embedded Mach-O linker")
    set(JOYEER_MACOS_SDK_VERSION "" CACHE STRING
            "macOS SDK version used by the embedded Mach-O linker")
    set(JOYEER_MACOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE STRING
            "Minimum macOS version for Joyeer native executables")
    if(NOT JOYEER_MACOS_SDK_PATH OR NOT IS_DIRECTORY "${JOYEER_MACOS_SDK_PATH}")
        execute_process(
                COMMAND "${JOYEER_XCRUN_EXECUTABLE}" --sdk macosx --show-sdk-path
                RESULT_VARIABLE JOYEER_XCRUN_RESULT
                OUTPUT_VARIABLE JOYEER_XCRUN_SDK_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(JOYEER_XCRUN_RESULT EQUAL 0 AND IS_DIRECTORY "${JOYEER_XCRUN_SDK_PATH}")
            set(JOYEER_MACOS_SDK_PATH "${JOYEER_XCRUN_SDK_PATH}"
                    CACHE PATH
                    "macOS SDK used by the embedded Mach-O linker"
                    FORCE)
        endif()
    endif()
    if(NOT JOYEER_MACOS_SDK_VERSION)
        execute_process(
                COMMAND "${JOYEER_XCRUN_EXECUTABLE}" --sdk macosx --show-sdk-version
                RESULT_VARIABLE JOYEER_XCRUN_VERSION_RESULT
                OUTPUT_VARIABLE JOYEER_XCRUN_SDK_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(JOYEER_XCRUN_VERSION_RESULT EQUAL 0 AND JOYEER_XCRUN_SDK_VERSION)
            set(JOYEER_MACOS_SDK_VERSION "${JOYEER_XCRUN_SDK_VERSION}"
                    CACHE STRING
                    "macOS SDK version used by the embedded Mach-O linker"
                    FORCE)
        endif()
    endif()
    if(NOT JOYEER_MACOS_DEPLOYMENT_TARGET)
        set(JOYEER_MACOS_DEPLOYMENT_TARGET "${JOYEER_MACOS_SDK_VERSION}"
                CACHE STRING
                "Minimum macOS version for Joyeer native executables"
                FORCE)
    endif()
    if(NOT DEFINED JOYEER_DSYMUTIL_EXECUTABLE)
        execute_process(
                COMMAND "${JOYEER_XCRUN_EXECUTABLE}" --find dsymutil
                RESULT_VARIABLE JOYEER_XCRUN_DSYMUTIL_RESULT
                OUTPUT_VARIABLE JOYEER_XCRUN_DSYMUTIL
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(JOYEER_XCRUN_DSYMUTIL_RESULT EQUAL 0 AND
           EXISTS "${JOYEER_XCRUN_DSYMUTIL}")
            set(JOYEER_DSYMUTIL_EXECUTABLE "${JOYEER_XCRUN_DSYMUTIL}" CACHE FILEPATH
                "Tool used to create macOS dSYM bundles; empty disables bundles")
        else()
            set(JOYEER_DSYMUTIL_EXECUTABLE "" CACHE FILEPATH
                "Tool used to create macOS dSYM bundles; empty disables bundles")
        endif()
    endif()
    if(NOT JOYEER_MACOS_SDK_PATH OR
       NOT JOYEER_MACOS_SDK_VERSION OR
           NOT JOYEER_MACOS_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "Joyeer requires an active macOS SDK from Xcode Command Line Tools")
    endif()
    if(JOYEER_DSYMUTIL_EXECUTABLE AND
       (NOT EXISTS "${JOYEER_DSYMUTIL_EXECUTABLE}" OR
        IS_DIRECTORY "${JOYEER_DSYMUTIL_EXECUTABLE}"))
        message(FATAL_ERROR
            "JOYEER_DSYMUTIL_EXECUTABLE must name a dsymutil file, or be empty to disable bundles")
    endif()
    if(NOT JOYEER_DSYMUTIL_EXECUTABLE)
        message(STATUS
            "Joyeer dsymutil not found; macOS native output remains available but -g debug bundles are unavailable")
    endif()
    message(STATUS
            "Joyeer macOS SDK: ${JOYEER_MACOS_SDK_PATH} (${JOYEER_MACOS_SDK_VERSION}), deployment target ${JOYEER_MACOS_DEPLOYMENT_TARGET}")
endif()

# Use C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Standard C++ (-std=c++20), not gnu++20: GNU mode predefines `linux` as a macro, which
# breaks the TargetPlatform enumerator of that name.
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(JOYEER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} REALPATH)

# C headers of the runtime ABI; included as <joyeer/runtime.h> and <joyeer/native.h>.
set(JOYEER_RUNTIME_INCLUDE_DIR "${JOYEER_SOURCE_DIR}/lib/Runtime/include")

set(JOYEER_BIN_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin")
set(JOYEER_LIB_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib")

set(JOYEER_TESTS_DIR "${JOYEER_SOURCE_DIR}/tests")
set(JOYEER_UNITTESTS_DIR "${JOYEER_TESTS_DIR}/unittests")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${JOYEER_LIB_OUTPUT_INTDIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${JOYEER_LIB_OUTPUT_INTDIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${JOYEER_BIN_OUTPUT_INTDIR})

# Build stamp shared by the compiler and the standard library's lib/index.toml.
string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" JOYEER_TOOLCHAIN_TARGET)
set(JOYEER_TOOLCHAIN_VERSION "${PROJECT_VERSION}")

add_subdirectory(compiler)

# Object generation needs the compiler before a mixed-language runtime exists.
# Stage the usable product separately, rather than making joyeer wait for it.
add_custom_target(JoyeerToolchain ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:JoyeerRuntime>"
        "$<TARGET_FILE_DIR:joyeer>/$<TARGET_FILE_NAME:JoyeerRuntime>"
    DEPENDS joyeer JoyeerRuntime
    COMMENT "Staging the Joyeer compiler/runtime product"
    VERBATIM)

add_subdirectory(lib)

# Standard modules live in <toolchain>/lib, found by the compiler relative to
# its own executable. Stage them (and lib/index.toml) next to the build tree's
# compiler so a development build resolves its own library.
file(GLOB_RECURSE JOYEER_STDLIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/Foundation/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/System/*")
set(JOYEER_STDLIB_DIRECTORY "$<TARGET_FILE_DIR:joyeer>/lib")
set(JOYEER_STDLIB_STAMP "${CMAKE_CURRENT_BINARY_DIR}/stdlib.stamp")
set(JOYEER_STDLIB_COMPRESSION "")
set(JOYEER_STDLIB_EXTRA_DEPENDS "")
set(JOYEER_STDLIB_DEPENDS joyeer)
if(JOYEER_BUILD_COMPRESSION)
    set(JOYEER_STDLIB_COMPRESSION
        "${CMAKE_BINARY_DIR}/lib/Compression/package/$<CONFIG>/Compression")
    list(APPEND JOYEER_STDLIB_DEPENDS JoyeerCompression)
    set(JOYEER_STDLIB_EXTRA_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/Compression/compression.joyeer")
endif()
add_custom_command(
    OUTPUT "${JOYEER_STDLIB_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        "-DDESTINATION=${JOYEER_STDLIB_DIRECTORY}"
        "-DLIBRARY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/lib"
        "-DCOMPRESSION_PACKAGE=${JOYEER_STDLIB_COMPRESSION}"
        "-DVERSION=${JOYEER_TOOLCHAIN_VERSION}"
        "-DTARGET=${JOYEER_TOOLCHAIN_TARGET}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cmake/stageStandardLibrary.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${JOYEER_STDLIB_STAMP}"
    DEPENDS ${JOYEER_STDLIB_DEPENDS} ${JOYEER_STDLIB_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cmake/stageStandardLibrary.cmake"
        ${JOYEER_STDLIB_EXTRA_DEPENDS}
    COMMENT "Staging the standard library next to the compiler"
    VERBATIM)
add_custom_target(JoyeerStandardLibrary ALL DEPENDS "${JOYEER_STDLIB_STAMP}")
# <prefix>/lib sits beside the installed compiler, where it looks for it. The
# index lists every staged module, so the library installs as one component.
install(DIRECTORY "${JOYEER_STDLIB_DIRECTORY}/"
    DESTINATION lib COMPONENT StandardLibrary)

if(JOYEER_BUILD_FERN)
    add_subdirectory(tools/fern)
    if(JOYEER_BUILD_MOSS AND JOYEER_BUILD_COMPRESSION)
        include(scripts/cmake/moss.cmake)
    endif()
endif()
add_subdirectory(tests)

include(GNUInstallDirs)
install(TARGETS joyeer JoyeerBackend
    RUNTIME DESTINATION . COMPONENT JoyeerRuntime
    LIBRARY DESTINATION . COMPONENT JoyeerRuntime)
install(TARGETS JoyeerRuntime
    ARCHIVE DESTINATION . COMPONENT JoyeerRuntime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION licenses COMPONENT JoyeerRuntime)
install(FILES lib/Runtime/include/joyeer/native.h lib/Runtime/include/joyeer/runtime.h
    DESTINATION include/joyeer COMPONENT NativeDevelopment EXCLUDE_FROM_ALL)
