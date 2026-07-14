
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
