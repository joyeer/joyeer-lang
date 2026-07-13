
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)

    # tests/target contains forward-looking design fixtures that are not
    # executable golden tests yet.
    file(GLOB_RECURSE joyeerTestFiles
            "${CMAKE_CURRENT_SOURCE_DIR}/basis/*.joyeer"
            "${CMAKE_CURRENT_SOURCE_DIR}/errors/*.joyeer"
            "${CMAKE_CURRENT_SOURCE_DIR}/leetcode/*.joyeer")
    foreach(joyeerTest ${joyeerTestFiles})
        if(NOT EXISTS "${joyeerTest}.result.txt")
            message(FATAL_ERROR "Missing golden result for ${joyeerTest}")
        endif()
        add_test(NAME "${joyeerTest}"
                COMMAND Python3::Interpreter -m testRunner "${JOYEER_BIN_OUTPUT_INTDIR}/joyeer" ${joyeerTest}
                WORKING_DIRECTORY ${JOYEER_TESTS_DIR})
    endforeach()
endif()

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python3 not found, skipping integration tests")
endif()

