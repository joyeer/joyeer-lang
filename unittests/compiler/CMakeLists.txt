add_executable(CompilerLexerTests
        lexer_test.cpp
)

add_executable(CompilerDiagnosticTests
        diagnostic_test.cpp
)

target_link_libraries(CompilerDiagnosticTests
        GTest::gtest_main
        JoyeerDiagnostic
)

gtest_discover_tests(CompilerDiagnosticTests
        PROPERTIES LABELS diagnostics
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

add_executable(CompilerSemanticAnalysisTests
        semanticanalysis_test.cpp
)

target_link_libraries(CompilerSemanticAnalysisTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
)

gtest_discover_tests(CompilerSemanticAnalysisTests
        PROPERTIES LABELS semantic-analysis
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

    add_test(NAME LLVMBackendCompilerJsonMvpValidation
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/ok/json_mvp.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-json-mvp.ll
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyCompilerLLVMOutput.cmake
    )
    set_tests_properties(LLVMBackendCompilerJsonMvpValidation PROPERTIES
            LABELS "llvm-backend;ir-lowering;type-checking"
    )

    add_test(NAME NativeExecutableHello
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-hello${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableHello PROPERTIES
            LABELS "native;llvm-backend;native-runtime"
    )

    add_test(NAME NativeExecutableOwnership
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/ownership.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-ownership${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=left!\nleft!\nbeta\nvalue\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableOwnership PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership"
    )

    add_test(NAME NativeExecutableTrailingReturn
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/trailing_return.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-trailing-return${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\ntrailing\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableTrailingReturn PROPERTIES
            LABELS "native;llvm-backend;native-runtime;semantic-analysis"
    )

    add_test(NAME NativeExecutableDeferredInitialization
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/deferred_initialization.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-deferred-initialization${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\ninitialized\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableDeferredInitialization PROPERTIES
            LABELS "native;llvm-backend;native-runtime;semantic-analysis;ownership"
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

add_test(NAME SemanticAnalysisCliRejectsMissingReturn
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/missing_return.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.missing-return
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SemanticAnalysisCliRejectsMissingReturn PROPERTIES
        LABELS semantic-analysis
)

add_test(NAME SemanticAnalysisCliRejectsUseBeforeInitialization
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/use_before_initialization.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.use-before-initialization
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SemanticAnalysisCliRejectsUseBeforeInitialization PROPERTIES
        LABELS semantic-analysis
)

add_test(NAME SemanticAnalysisCliReportsUnreachableWarning
        COMMAND $<TARGET_FILE:joyeer>
                --lang=v0.1
                "${JOYEER_TESTS_DIR}/semantic-analysis/unreachable_warning.joyeer"
)
set_tests_properties(SemanticAnalysisCliReportsUnreachableWarning PROPERTIES
        LABELS semantic-analysis
        PASS_REGULAR_EXPRESSION semantic-analysis.unreachable-code
)

add_test(NAME SemanticAnalysisCliReportsUnusedBindingWarning
        COMMAND $<TARGET_FILE:joyeer>
                --lang=v0.1
                "${JOYEER_TESTS_DIR}/semantic-analysis/unused_warning.joyeer"
)
set_tests_properties(SemanticAnalysisCliReportsUnusedBindingWarning PROPERTIES
        LABELS semantic-analysis
        PASS_REGULAR_EXPRESSION semantic-analysis.unused-binding
)

add_test(NAME NativeLinkerRejectsMissingMain
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/missing_main.joyeer
                -DEXPECTED_PATTERN=linker.missing-entry-point
                "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/missing-main${CMAKE_EXECUTABLE_SUFFIX}"
                -DOUTPUT_TO_REMOVE=${CMAKE_CURRENT_BINARY_DIR}/missing-main${CMAKE_EXECUTABLE_SUFFIX}
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(NativeLinkerRejectsMissingMain PROPERTIES
        LABELS "native;llvm-backend"
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
