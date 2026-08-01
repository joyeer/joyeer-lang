
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

if(WIN32)
        set(JOYEER_CONFIGURED_CLANG_EXECUTABLE "")
else()
        set(JOYEER_CONFIGURED_CLANG_EXECUTABLE "${JOYEER_CLANG_EXECUTABLE}")
endif()

target_compile_definitions(joyeer PRIVATE
        JOYEER_CLANG_EXECUTABLE_PATH="${JOYEER_CONFIGURED_CLANG_EXECUTABLE}"
        JOYEER_NATIVE_RUNTIME_PATH="$<IF:$<PLATFORM_ID:Windows>,$<TARGET_FILE_NAME:JoyeerNativeRuntime>,$<TARGET_FILE:JoyeerNativeRuntime>>"
        JOYEER_SDK_ROOT_PATH="${JOYEER_MACOS_SDK_PATH}"
)

if(WIN32)
        add_custom_command(TARGET joyeer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:JoyeerNativeRuntime>"
                        "$<TARGET_FILE_DIR:joyeer>/$<TARGET_FILE_NAME:JoyeerNativeRuntime>"
        )
endif()

target_link_libraries(joyeer
        JoyeerCLI
        JoyeerCompiler
        JoyeerLLVMBackend
        JoyeerDiagnostic
)
