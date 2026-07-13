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
