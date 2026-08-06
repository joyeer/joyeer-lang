add_joyeer_library(JoyeerLLVMBackend STATIC
        llvm.cpp
        linker.cpp
)

target_link_libraries(JoyeerLLVMBackend PUBLIC JoyeerIR)

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
llvm_map_components_to_libnames(JOYEER_NATIVE_LLVM_LIBRARIES
        AsmParser
        IRReader
        Passes
        native
)
target_link_libraries(JoyeerNativeBackend PRIVATE
        ${JOYEER_NATIVE_LLVM_LIBRARIES}
)
if(WIN32)
        target_link_libraries(JoyeerNativeBackend PRIVATE
                lldCOFF
        )
elseif(APPLE)
        target_link_libraries(JoyeerNativeBackend PRIVATE
                lldMachO
        )
else()
        target_include_directories(JoyeerNativeBackend SYSTEM PRIVATE
                ${CLANG_INCLUDE_DIRS}
        )
        target_link_libraries(JoyeerNativeBackend PRIVATE
                clangDriver
                lldELF
        )
endif()
set_target_properties(JoyeerNativeBackend PROPERTIES
        OUTPUT_NAME joyeer-backend
)
if(WIN32)
        set_target_properties(JoyeerNativeBackend PROPERTIES
                MSVC_RUNTIME_LIBRARY MultiThreaded
        )
endif()

target_link_libraries(JoyeerLLVMBackend PRIVATE JoyeerNativeBackend)

if(WIN32)
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
