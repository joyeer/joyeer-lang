cmake_minimum_required(VERSION 3.20)

project (joyeer 
        VERSION 0.0.1
        LANGUAGES C CXX)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "Joyeer on Windows requires the MSVC compiler from Visual Studio. "
        "Run CMake from a Visual Studio Developer shell.")
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
enable_testing()
enable_language(CXX)

option(JOYEER_BUILD_UNITTESTS "Build Joyeer C++ unit tests" ON)

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
        HINTS "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64"
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
    execute_process(
            COMMAND "${JOYEER_XCRUN_EXECUTABLE}" --find dsymutil
            RESULT_VARIABLE JOYEER_XCRUN_DSYMUTIL_RESULT
            OUTPUT_VARIABLE JOYEER_XCRUN_DSYMUTIL
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT JOYEER_MACOS_SDK_PATH OR
       NOT JOYEER_MACOS_SDK_VERSION OR
           NOT JOYEER_MACOS_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "Joyeer requires an active macOS SDK from Xcode Command Line Tools")
        endif()
        if(JOYEER_XCRUN_DSYMUTIL_RESULT EQUAL 0 AND
           EXISTS "${JOYEER_XCRUN_DSYMUTIL}")
        set(JOYEER_DSYMUTIL_EXECUTABLE "${JOYEER_XCRUN_DSYMUTIL}" CACHE FILEPATH
            "Tool used to create macOS dSYM bundles" FORCE)
        else()
        set(JOYEER_DSYMUTIL_EXECUTABLE "" CACHE FILEPATH
            "Tool used to create macOS dSYM bundles" FORCE)
        message(STATUS
            "Joyeer dsymutil not found; macOS native output remains available but -g debug bundles are unavailable")
    endif()
    message(STATUS
            "Joyeer macOS SDK: ${JOYEER_MACOS_SDK_PATH} (${JOYEER_MACOS_SDK_VERSION}), deployment target ${JOYEER_MACOS_DEPLOYMENT_TARGET}")
endif()

# Use C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(JOYEER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} REALPATH)

set(JOYEER_INCLUDE_DIR "${JOYEER_SOURCE_DIR}/include")
set(JOYEER_LIB_DIR "${JOYEER_SOURCE_DIR}/lib")

set(JOYEER_BIN_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin")
set(JOYEER_LIB_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib")

set(JOYEER_TESTS_DIR "${JOYEER_SOURCE_DIR}/tests")
set(JOYEER_UNITTESTS_DIR "${JOYEER_SOURCE_DIR}/unittests")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${JOYEER_LIB_OUTPUT_INTDIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${JOYEER_LIB_OUTPUT_INTDIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${JOYEER_BIN_OUTPUT_INTDIR})

include(addJoyeer)

# include headers
set(CMAKE_INCLUDE_CURRENT_DIR ON)
include_directories(
    ${JOYEER_INCLUDE_DIR}
)

add_subdirectory(lib)
add_subdirectory(tests)
if(JOYEER_BUILD_UNITTESTS)
    add_subdirectory(unittests)
endif()

include(GNUInstallDirs)
install(TARGETS joyeer JoyeerNativeBackend
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .)
install(TARGETS JoyeerNativeRuntime
    ARCHIVE DESTINATION .)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION licenses)
