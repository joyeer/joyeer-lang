
add_executable(RuntimeTests
        descriptor_test.cpp
        heaps_test.cpp
        native_runtime_test.cpp
)

target_link_libraries(RuntimeTests
        GTest::gtest_main
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
        JoyeerNativeRuntime
)

gtest_discover_tests(RuntimeTests
        PROPERTIES LABELS native-runtime
)

add_executable(RuntimeOptionsTests
        arguments_test.cpp
)

target_link_libraries(RuntimeOptionsTests
        GTest::gtest_main
        JoyeerRuntime
        JoyeerDiagnostic
)

gtest_discover_tests(RuntimeOptionsTests
        PROPERTIES LABELS "native-runtime\;optimization\;debug-info"
)
