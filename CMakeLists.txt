cmake_minimum_required(VERSION 3.16)

project (joyeer 
        VERSION 0.0.1
        LANGUAGES C CXX)

cmake_policy(SET CMP0110 NEW)
cmake_policy(SET CMP0114 NEW)

set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CTEST_OUTPUT_ON_FAILURE TRUE)

# custom cmake module
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cmake")

# enable the testing
enable_testing()
enable_language(CXX)

option(JOYEER_BUILD_UNITTESTS "Build Joyeer C++ unit tests" ON)

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
