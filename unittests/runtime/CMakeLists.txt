
add_executable(RuntimeTests
        descriptor_test.cpp
        heaps_test.cpp
)

target_link_libraries(RuntimeTests
        GTest::gtest_main
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
)

gtest_discover_tests(RuntimeTests)
