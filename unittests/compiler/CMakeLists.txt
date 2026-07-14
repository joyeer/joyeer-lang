add_executable(CompilerLexerTests
        lexer_test.cpp
)

target_link_libraries(CompilerLexerTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
)

gtest_discover_tests(CompilerLexerTests
        PROPERTIES LABELS lexer
)

add_executable(CompilerParserTests
        parser_test.cpp
)

target_link_libraries(CompilerParserTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
)

target_compile_definitions(CompilerParserTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

gtest_discover_tests(CompilerParserTests
        PROPERTIES LABELS parser
)

add_executable(CompilerNameResolutionTests
        nameresolution_test.cpp
)

target_link_libraries(CompilerNameResolutionTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
)

target_compile_definitions(CompilerNameResolutionTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

gtest_discover_tests(CompilerNameResolutionTests
        PROPERTIES LABELS name-resolution
)

add_executable(CompilerTypeCheckingTests
        typechecking_test.cpp
)

target_link_libraries(CompilerTypeCheckingTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
)

target_compile_definitions(CompilerTypeCheckingTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

gtest_discover_tests(CompilerTypeCheckingTests
        PROPERTIES LABELS type-checking
)

add_executable(IRModelTests
        ir_test.cpp
)

target_link_libraries(IRModelTests
        GTest::gtest_main
        JoyeerIR
)

gtest_discover_tests(IRModelTests
        PROPERTIES LABELS ir
)

add_executable(IRLoweringTests
        irlowering_test.cpp
)

target_link_libraries(IRLoweringTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerIR
        JoyeerVM
)

target_compile_definitions(IRLoweringTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

gtest_discover_tests(IRLoweringTests
        PROPERTIES LABELS ir-lowering
)

add_executable(LLVMBackendTests
        llvm_test.cpp
)

target_link_libraries(LLVMBackendTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerLLVMBackend
        JoyeerVM
)

gtest_discover_tests(LLVMBackendTests
        PROPERTIES LABELS llvm-backend
)

if(JOYEER_CLANG_EXECUTABLE)
    add_executable(LLVMFixtureGenerator
            llvm_fixture.cpp
    )
    target_link_libraries(LLVMFixtureGenerator
            JoyeerLLVMBackend
    )

    add_test(NAME LLVMBackendClangValidation
            COMMAND ${CMAKE_COMMAND}
                    -DGENERATOR=$<TARGET_FILE:LLVMFixtureGenerator>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-llvm-validation.ll
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyLLVMIR.cmake
    )
    set_tests_properties(LLVMBackendClangValidation PROPERTIES
            LABELS llvm-backend
    )
endif()

add_test(NAME NameResolutionCliRejectsUndefinedName
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/name-resolution/err/undefined_name.joyeer
                -DEXPECTED_PATTERN=name-resolution.undefined-name
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(NameResolutionCliRejectsUndefinedName PROPERTIES
        LABELS name-resolution
)

add_test(NAME TypeCheckingCliRejectsMismatch
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/type_mismatch.joyeer
                -DEXPECTED_PATTERN=type-checking.type-mismatch
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(TypeCheckingCliRejectsMismatch PROPERTIES
        LABELS type-checking
)

add_test(NAME IRLoweringCliRejectsMissingReturn
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/ir-lowering/err/missing_return.joyeer
                -DEXPECTED_PATTERN=ir-lowering.missing-return
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(IRLoweringCliRejectsMissingReturn PROPERTIES
        LABELS ir-lowering
)

add_test(NAME ParserMvpCliAcceptance
        COMMAND $<TARGET_FILE:joyeer>
                --lang=v0.1
                "${JOYEER_TESTS_DIR}/parser/ok/json_mvp.joyeer"
)
set_tests_properties(ParserMvpCliAcceptance PROPERTIES
        LABELS "parser;name-resolution;type-checking;ir-lowering"
)

add_test(NAME ParserMvpCliRejectsInvalidSource
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/err/missing_expression.joyeer
                -DEXPECTED_PATTERN=parser.expected-expression
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(ParserMvpCliRejectsInvalidSource PROPERTIES
        LABELS parser
)
