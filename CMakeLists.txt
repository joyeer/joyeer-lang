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

set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CTEST_OUTPUT_ON_FAILURE TRUE)

# custom cmake module
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cmake")

# enable the testing
enable_testing()
enable_language(CXX)

option(JOYEER_BUILD_UNITTESTS "Build Joyeer C++ unit tests" ON)

find_program(JOYEER_CLANG_EXECUTABLE
        NAMES clang clang-22
        HINTS
            "$ENV{ProgramFiles}/LLVM/bin"
            "/usr/local/opt/llvm/bin"
            "/opt/homebrew/opt/llvm/bin"
        DOC "Clang driver used to verify and compile generated LLVM IR"
)
if(JOYEER_CLANG_EXECUTABLE)
    message(STATUS "Joyeer LLVM driver: ${JOYEER_CLANG_EXECUTABLE}")
    if(WIN32)
        get_filename_component(JOYEER_LLVM_BIN_DIR "${JOYEER_CLANG_EXECUTABLE}" DIRECTORY)
        find_program(JOYEER_LLD_LINK_EXECUTABLE
                NAMES lld-link lld-link.exe
                HINTS "${JOYEER_LLVM_BIN_DIR}"
                NO_DEFAULT_PATH
                DOC "LLD linker required for Windows DWARF native output"
        )
        if(JOYEER_LLD_LINK_EXECUTABLE)
            message(STATUS "Joyeer Windows DWARF linker: ${JOYEER_LLD_LINK_EXECUTABLE}")
        else()
            message(STATUS "LLD linker not found; Windows DWARF executable output is unavailable")
        endif()
    elseif(APPLE)
        set(JOYEER_MACOS_SDK_PATH "" CACHE PATH
                "macOS SDK passed to the Clang driver for native output")
        if(NOT JOYEER_MACOS_SDK_PATH OR NOT IS_DIRECTORY "${JOYEER_MACOS_SDK_PATH}")
            find_program(JOYEER_XCRUN_EXECUTABLE NAMES xcrun)
            if(JOYEER_XCRUN_EXECUTABLE)
                execute_process(
                        COMMAND "${JOYEER_XCRUN_EXECUTABLE}" --sdk macosx --show-sdk-path
                        RESULT_VARIABLE JOYEER_XCRUN_RESULT
                        OUTPUT_VARIABLE JOYEER_XCRUN_SDK_PATH
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(JOYEER_XCRUN_RESULT EQUAL 0 AND IS_DIRECTORY "${JOYEER_XCRUN_SDK_PATH}")
                    set(JOYEER_MACOS_SDK_PATH "${JOYEER_XCRUN_SDK_PATH}"
                            CACHE PATH
                            "macOS SDK passed to the Clang driver for native output"
                            FORCE)
                endif()
            endif()
        endif()
        if(JOYEER_MACOS_SDK_PATH)
            message(STATUS "Joyeer macOS SDK: ${JOYEER_MACOS_SDK_PATH}")
        else()
            message(STATUS "Joyeer macOS SDK not found; native linking relies on the Clang default")
        endif()
    endif()
else()
    message(STATUS "Joyeer LLVM driver not found; textual emission remains available")
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
