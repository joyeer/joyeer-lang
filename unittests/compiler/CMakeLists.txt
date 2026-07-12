add_executable(CompilerLexerTests
        lexer_test.cpp
)

target_link_libraries(CompilerLexerTests
        GTest::gtest_main
        JoyeerCompiler
        JoyeerDiagnostic
)

gtest_discover_tests(CompilerLexerTests)
