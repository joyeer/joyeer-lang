
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)

    file(GLOB_RECURSE joyeerTestFiles "*.joyeer")
    foreach(joyeerTest ${joyeerTestFiles})
        add_test(NAME "${joyeerTest}"
                COMMAND Python3::Interpreter -m testRunner "${JOYEER_BIN_OUTPUT_INTDIR}/joyeer" ${joyeerTest}
                WORKING_DIRECTORY ${JOYEER_TESTS_DIR})
    endforeach()
endif()

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python3 not found, skipping integration tests")
endif()

