add_joyeer_library(JoyeerLLVMBackend STATIC
        llvm.cpp
        linker.cpp
)

target_link_libraries(JoyeerLLVMBackend PUBLIC JoyeerIR)

if(JOYEER_USE_LLVM_SDK)
        llvm_map_components_to_libnames(JOYEER_LLVM_LIBRARIES
                        Analysis
                        CodeGen
                        Core
                        IRReader
                        MC
                        Object
                        Passes
                        Support
                        Target
                        native
        )

        if(WIN32)
                set(JOYEER_LLD_DRIVER lldCOFF)
        elseif(APPLE)
                set(JOYEER_LLD_DRIVER lldMachO)
        else()
                set(JOYEER_LLD_DRIVER lldELF)
        endif()

        separate_arguments(JOYEER_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

        add_library(JoyeerLLVMSDK INTERFACE)
        target_include_directories(JoyeerLLVMSDK SYSTEM INTERFACE
                        ${LLVM_INCLUDE_DIRS}
                        ${LLD_INCLUDE_DIRS}
        )
        target_compile_options(JoyeerLLVMSDK INTERFACE
                        ${JOYEER_LLVM_DEFINITIONS}
        )
        target_link_libraries(JoyeerLLVMSDK INTERFACE
                        ${JOYEER_LLVM_LIBRARIES}
                        ${JOYEER_LLD_DRIVER}
        )

        target_link_libraries(JoyeerLLVMBackend PRIVATE JoyeerLLVMSDK)

        if(JOYEER_VERIFY_LLVM_SDK)
                add_executable(JoyeerLLVMSDKProbe
                                llvm_sdk_probe.cpp
                )
                target_link_libraries(JoyeerLLVMSDKProbe PRIVATE JoyeerLLVMSDK)
                add_test(NAME JoyeerLLVMSDKProbe COMMAND JoyeerLLVMSDKProbe)
                set_tests_properties(JoyeerLLVMSDKProbe PROPERTIES LABELS llvm-sdk)
        endif()
endif()
