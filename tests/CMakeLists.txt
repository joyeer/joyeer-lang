if(BUILD_TESTING)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ValidationScriptTests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_validate.py")
        set_tests_properties(ValidationScriptTests PROPERTIES LABELS "tooling" TIMEOUT 30)
    endif()

    set(joyeer_skill_examples "${JOYEER_SOURCE_DIR}/skills/joyeer/examples")
    set(joyeer_skill_output "${CMAKE_CURRENT_BINARY_DIR}/skill")
    file(MAKE_DIRECTORY "${joyeer_skill_output}")
    file(MAKE_DIRECTORY "${joyeer_skill_output}/missing-input")

    function(joyeer_add_skill_example name source expected_output working_directory)
        add_test(NAME "JoyeerSkill.${name}"
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILE=${joyeer_skill_examples}/${source}.joyeer"
                "-DOUTPUT_FILE=${joyeer_skill_output}/${name}${CMAKE_EXECUTABLE_SUFFIX}"
                "-DEXPECTED_OUTPUT=${expected_output}"
                "-DPROGRAM_WORKING_DIRECTORY=${working_directory}"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
        set_tests_properties("JoyeerSkill.${name}" PROPERTIES
            LABELS "skill;e2e"
            TIMEOUT 60)
    endfunction()

    joyeer_add_skill_example(Hello hello
        "Hello, Joyeer!\n42\n" "${joyeer_skill_examples}")
    joyeer_add_skill_example(Collections collections
        "2\n42\nJoy\n66\n7\n" "${joyeer_skill_examples}")
    joyeer_add_skill_example(OwnershipAndErrors ownership-and-errors
        "42\nhello!\nreinitialized\n7\nabsent\n8\nexpected non-negative\n"
        "${joyeer_skill_examples}")
    joyeer_add_skill_example(ReadFile read-file
        "2\n" "${joyeer_skill_examples}")
    joyeer_add_skill_example(ReadFileMissing read-file
        "read failed\n" "${joyeer_skill_output}/missing-input")
endif()

if(BUILD_TESTING AND JOYEER_BUILD_FERN)
    add_subdirectory(fern)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(c-interop)
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
        set(web_server_head "${JOYEER_SOURCE_DIR}/example/web-server/src/http.joyeer")
        set(web_server_markdown "${JOYEER_SOURCE_DIR}/example/web-server/src/markdown.joyeer")
        set(web_server_fixture "${CMAKE_CURRENT_SOURCE_DIR}/e2e/web-server/request_head_deadline.joyeer")
        add_test(NAME WebServer.RequestHeadDeadline
            COMMAND "${CMAKE_COMMAND}"
                "-DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>"
                "-DINPUT_FILES=${web_server_head};${web_server_markdown};${web_server_fixture}"
                "-DMODULE_NAME=test.request_head"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/web-request-head-deadline${CMAKE_EXECUTABLE_SUFFIX}"
                "-DCOMPILER_ARGS=-O0;-g0"
                "-DEXPECTED_OUTPUT=request-head deadline passed\n"
                -P "${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake")
        set_tests_properties(WebServer.RequestHeadDeadline PROPERTIES LABELS "e2e;network" TIMEOUT 60)
        if(Python3_Interpreter_FOUND)
            foreach(level IN ITEMS 0 2)
                add_test(NAME "WebServer.IdleDispatchO${level}"
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_idle_workers.py"
                        "$<TARGET_FILE:joyeer>" "${JOYEER_SOURCE_DIR}/example/web-server" "${level}")
                set_tests_properties("WebServer.IdleDispatchO${level}" PROPERTIES LABELS "e2e;network;channels" TIMEOUT 150)
            endforeach()
        endif()
    endif()
    if(APPLE)
        add_subdirectory(lib/System)
    endif()
    if(JOYEER_BUILD_COMPRESSION)
        add_subdirectory(lib/Compression)
    endif()
endif()

if(JOYEER_BUILD_UNITTESTS)
    add_subdirectory(unittests)
endif()
