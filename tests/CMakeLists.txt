if(BUILD_TESTING)
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
            LABELS "skill;native"
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
