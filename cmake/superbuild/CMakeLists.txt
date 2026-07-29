cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
endif()

project(JoyeerSuperbuild NONE)

include(ExternalProject)

find_program(JOYEER_CURL_EXECUTABLE NAMES curl curl.exe REQUIRED)
execute_process(
                COMMAND "${JOYEER_CURL_EXECUTABLE}" --version
                RESULT_VARIABLE _joyeer_curl_result
                OUTPUT_VARIABLE _joyeer_curl_version_output
                OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _joyeer_curl_result EQUAL 0 OR
   NOT _joyeer_curl_version_output MATCHES "^curl ([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        message(FATAL_ERROR "Unable to determine curl version")
endif()
if(CMAKE_MATCH_1 LESS 7 OR
   (CMAKE_MATCH_1 EQUAL 7 AND CMAKE_MATCH_2 LESS 71))
        message(FATAL_ERROR "Joyeer LLVM downloads require curl 7.71 or newer")
endif()

set(JOYEER_LLVM_VERSION "22.1.8" CACHE STRING
        "LLVM and LLD version used by the Joyeer backend")
set(JOYEER_LLVM_SOURCE_URL
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-${JOYEER_LLVM_VERSION}/llvm-project-${JOYEER_LLVM_VERSION}.src.tar.xz"
        CACHE STRING "Pinned LLVM source archive")
set(JOYEER_LLVM_SOURCE_SHA256
        "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888"
        CACHE STRING "SHA-256 of the pinned LLVM source archive")
set(JOYEER_LLVM_TARGETS_TO_BUILD "Native" CACHE STRING
        "LLVM target backends included in the Joyeer SDK")
set(JOYEER_LLVM_BUILD_TYPE "Release" CACHE STRING
        "Build type for the private LLVM and LLD SDK")
set(JOYEER_LLVM_PARALLEL_LINK_JOBS "1" CACHE STRING
        "Maximum concurrent LLVM link jobs")
set(JOYEER_LLVM_INSTALL_DIR "${CMAKE_BINARY_DIR}/llvm-sdk" CACHE PATH
        "Installation prefix for the private LLVM and LLD SDK")
set(JOYEER_BUILD_DIR "${CMAKE_BINARY_DIR}/joyeer" CACHE PATH
        "Build directory for the main Joyeer project")
set(JOYEER_BUILD_TYPE "Release" CACHE STRING
        "Build type for the main Joyeer project")
option(JOYEER_BUILD_UNITTESTS "Build Joyeer C++ unit tests" OFF)

set(_joyeer_llvm_archive
        "${CMAKE_BINARY_DIR}/downloads/llvm-project-${JOYEER_LLVM_VERSION}.src.tar.xz")

set(_joyeer_llvm_cmake_args
        -DCMAKE_BUILD_TYPE=${JOYEER_LLVM_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DLLVM_ENABLE_PROJECTS=lld
        -DLLVM_TARGETS_TO_BUILD=${JOYEER_LLVM_TARGETS_TO_BUILD}
        -DLLVM_ENABLE_PIC=ON
        -DLLVM_ENABLE_ASSERTIONS=OFF
        -DLLVM_APPEND_VC_REV=OFF
        -DLLVM_ENABLE_BINDINGS=OFF
        -DLLVM_ENABLE_CURL=OFF
        -DLLVM_ENABLE_FFI=OFF
        -DLLVM_ENABLE_HTTPLIB=OFF
        -DLLVM_ENABLE_LIBEDIT=OFF
        -DLLVM_ENABLE_LIBPFM=OFF
        -DLLVM_ENABLE_RTTI=OFF
        -DLLVM_ENABLE_TERMINFO=OFF
        -DLLVM_ENABLE_Z3_SOLVER=OFF
        -DLLVM_ENABLE_ZLIB=OFF
        -DLLVM_ENABLE_ZSTD=OFF
        -DLLVM_INCLUDE_BENCHMARKS=OFF
        -DLLVM_INCLUDE_EXAMPLES=OFF
        -DLLVM_INCLUDE_TESTS=OFF
        -DLLVM_INCLUDE_UTILS=OFF
        -DLLVM_BUILD_BENCHMARKS=OFF
        -DLLVM_BUILD_DOCS=OFF
        -DLLVM_BUILD_EXAMPLES=OFF
        -DLLVM_BUILD_TESTS=OFF
        -DLLVM_BUILD_TOOLS=OFF
        -DLLVM_BUILD_LLVM_DYLIB=OFF
        -DLLVM_LINK_LLVM_DYLIB=OFF
        -DLLD_BUILD_TOOLS=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DLLVM_PARALLEL_LINK_JOBS=${JOYEER_LLVM_PARALLEL_LINK_JOBS}
)

if(WIN32)
    list(APPEND _joyeer_llvm_cmake_args
            -DCMAKE_C_COMPILER=cl.exe
            -DCMAKE_CXX_COMPILER=cl.exe
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
    )
endif()

ExternalProject_Add(JoyeerLLVM
        DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads"
        SOURCE_DIR "${CMAKE_BINARY_DIR}/src/llvm-project"
        BINARY_DIR "${CMAKE_BINARY_DIR}/llvm-build"
        INSTALL_DIR "${JOYEER_LLVM_INSTALL_DIR}"
        SOURCE_SUBDIR llvm
        DOWNLOAD_COMMAND
            "${CMAKE_COMMAND}"
            "-DJOYEER_DOWNLOAD_URL=${JOYEER_LLVM_SOURCE_URL}"
            "-DJOYEER_DOWNLOAD_ARCHIVE=${_joyeer_llvm_archive}"
            "-DJOYEER_DOWNLOAD_SHA256=${JOYEER_LLVM_SOURCE_SHA256}"
            "-DJOYEER_DOWNLOAD_SOURCE_DIR=<SOURCE_DIR>"
            "-DJOYEER_CURL_EXECUTABLE=${JOYEER_CURL_EXECUTABLE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/DownloadLLVM.cmake"
        CMAKE_ARGS ${_joyeer_llvm_cmake_args}
        STEP_TARGETS download configure
        USES_TERMINAL_DOWNLOAD TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE
)

ExternalProject_Add(Joyeer
        DEPENDS JoyeerLLVM
        SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.."
        BINARY_DIR "${JOYEER_BUILD_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        INSTALL_COMMAND ""
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${JOYEER_BUILD_TYPE}
            -DJOYEER_BUILD_UNITTESTS=${JOYEER_BUILD_UNITTESTS}
            -DJOYEER_USE_LLVM_SDK=ON
            -DJOYEER_VERIFY_LLVM_SDK=ON
            -DLLVM_DIR=${JOYEER_LLVM_INSTALL_DIR}/lib/cmake/llvm
            -DLLD_DIR=${JOYEER_LLVM_INSTALL_DIR}/lib/cmake/lld
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
)
