
add_executable(NativeRuntimeTests
        native_runtime_test.cpp
)

target_link_libraries(NativeRuntimeTests
        GTest::gtest_main
        JoyeerNativeRuntime
)

joyeer_discover_tests(NativeRuntimeTests
        PROPERTIES LABELS native-runtime
)

add_executable(RuntimeOptionsTests
        arguments_test.cpp
)

target_link_libraries(RuntimeOptionsTests
        GTest::gtest_main
        JoyeerCLI
        JoyeerDiagnostic
)

joyeer_discover_tests(RuntimeOptionsTests
        PROPERTIES LABELS "native-runtime\;optimization\;debug-info"
)
