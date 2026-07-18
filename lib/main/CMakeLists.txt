
add_joyeer_library(JoyeerCLI STATIC
        arguments.cpp
)

target_link_libraries(JoyeerCLI PUBLIC JoyeerDiagnostic)

add_joyeer_executable(joyeer 
        main.cpp
        driver.cpp
        )

add_dependencies(joyeer JoyeerCLI)
add_dependencies(joyeer JoyeerCompiler)
add_dependencies(joyeer JoyeerDiagnostic)
add_dependencies(joyeer JoyeerNativeRuntime)

target_compile_definitions(joyeer PRIVATE
        JOYEER_CLANG_EXECUTABLE_PATH="${JOYEER_CLANG_EXECUTABLE}"
        JOYEER_NATIVE_RUNTIME_PATH="$<TARGET_FILE:JoyeerNativeRuntime>"
)

target_link_libraries(joyeer
        JoyeerCLI
        JoyeerCompiler
        JoyeerLLVMBackend
        JoyeerDiagnostic
)
