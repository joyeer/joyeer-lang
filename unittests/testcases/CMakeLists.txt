
add_executable(UnitTests 
        main.cpp
        common_descriptor_test.cpp
        heaps_test.cpp
)

TARGET_LINK_LIBRARIES(UnitTests
        GTest::GTest
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
        )

add_test(NAME UnitTests COMMAND UnitTests)
