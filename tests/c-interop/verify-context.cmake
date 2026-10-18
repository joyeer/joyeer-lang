function(expect_failure pattern)
    execute_process(
        COMMAND "${JOYEER_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(status EQUAL 0 OR NOT "${output}${errors}" MATCHES "${pattern}")
        message(FATAL_ERROR "Expected failure '${pattern}', got ${status}:\n${output}\n${errors}")
    endif()
endfunction()

expect_failure("type-checking.unauthorized-native-binding"
    --module-name runtime.fixture "${SOURCE_FILE}")
expect_failure("module.invalid-runtime-context"
    --module-name runtime.fixture --emit-runtime-object "${OUTPUT_DIRECTORY}/missing-context.o" "${SOURCE_FILE}")
expect_failure("linker.missing-entry-point"
    --module-name runtime.fixture --runtime-module runtime.fixture
    -o "${OUTPUT_DIRECTORY}/not-an-executable" "${SOURCE_FILE}")
expect_failure("output path must not overwrite"
    --module-name runtime.fixture --runtime-module runtime.fixture
    --emit-runtime-object "${SOURCE_FILE}" "${SOURCE_FILE}")
expect_failure("linker.file-error"
    --module-name runtime.fixture --runtime-module runtime.fixture
    --emit-runtime-object "${OUTPUT_DIRECTORY}" "${SOURCE_FILE}")
if(NOT IS_DIRECTORY "${OUTPUT_DIRECTORY}")
    message(FATAL_ERROR "Runtime object emission removed its directory output")
endif()
