add_joyeer_library(JoyeerLLVMBackend STATIC
        llvm.cpp
        linker.cpp
)

target_link_libraries(JoyeerLLVMBackend PUBLIC JoyeerIR)

if(WIN32)
        add_joyeer_library(JoyeerNativeBackend SHARED
                native_backend.cpp
        )

        separate_arguments(JOYEER_LLVM_DEFINITIONS NATIVE_COMMAND
                "${LLVM_DEFINITIONS}")

        target_compile_definitions(JoyeerNativeBackend PRIVATE
                JOYEER_NATIVE_BACKEND_BUILD
        )
        target_compile_options(JoyeerNativeBackend PRIVATE
                ${JOYEER_LLVM_DEFINITIONS}
        )
        target_include_directories(JoyeerNativeBackend SYSTEM PRIVATE
                ${LLVM_INCLUDE_DIRS}
                ${LLD_INCLUDE_DIRS}
        )
        target_link_libraries(JoyeerNativeBackend PRIVATE
                LLVMAsmParser
                LLVMIRReader
                LLVMPasses
                LLVMX86CodeGen
                lldCOFF
        )
        set_target_properties(JoyeerNativeBackend PROPERTIES
                OUTPUT_NAME joyeer-native-backend
                MSVC_RUNTIME_LIBRARY MultiThreaded
        )

        target_link_libraries(JoyeerLLVMBackend PRIVATE JoyeerNativeBackend)

        add_test(NAME NativeBackendHasNoExternalLLVMDlls
                COMMAND ${CMAKE_COMMAND}
                        -DLLVM_READOBJ=${JOYEER_LLVM_ROOT}/bin/llvm-readobj.exe
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DBACKEND_LIBRARY=$<TARGET_FILE:JoyeerNativeBackend>
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeBackendDependencies.cmake
        )
        set_tests_properties(NativeBackendHasNoExternalLLVMDlls PROPERTIES
                LABELS "llvm-backend;native;abi;packaging"
        )
endif()
