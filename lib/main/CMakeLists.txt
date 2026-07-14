
add_joyeer_executable(joyeer 
        main.cpp
        driver.cpp
        )

add_dependencies(joyeer JoyeerRuntime)
add_dependencies(joyeer JoyeerCompiler)
add_dependencies(joyeer JoyeerVM)
add_dependencies(joyeer JoyeerDiagnostic)
add_dependencies(joyeer JoyeerNativeRuntime)

target_compile_definitions(joyeer PRIVATE
        JOYEER_CLANG_EXECUTABLE_PATH="${JOYEER_CLANG_EXECUTABLE}"
        JOYEER_NATIVE_RUNTIME_PATH="$<TARGET_FILE:JoyeerNativeRuntime>"
)

target_link_libraries(joyeer
        JoyeerDiagnostic
        JoyeerRuntime
        JoyeerCompiler
        JoyeerVM
        JoyeerLLVMBackend
)
