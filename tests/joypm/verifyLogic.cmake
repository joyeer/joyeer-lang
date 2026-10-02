cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/helpers.cmake")
execute_process(COMMAND "${LOGIC_EXECUTABLE}"
    WORKING_DIRECTORY "${outside}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
string(REPLACE "\r\n" "\n" output "${output}")
if(NOT result STREQUAL "0" OR NOT output STREQUAL "joypm logic ok\n" OR
   NOT error STREQUAL "")
    message(FATAL_ERROR "joypm pure logic failed (${result}):\n${output}\n${error}")
endif()