add_library(JoyeerNativeBackend SHARED
        native_backend.cpp
        native_module.cpp
        native_types.cpp
        native_ownership.cpp
        native_functions.cpp
        native_collections.cpp
        native_host.cpp
        native_debug.cpp
        # Compile the verifier with the backend's CRT and visibility; reusing
        # compiler-core objects would mix /MD with /MT on Windows.
        ../compiler/ir/ir.cpp
)
target_include_directories(JoyeerNativeBackend PUBLIC ${JOYEER_SRC_DIR})

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
target_link_libraries(JoyeerNativeBackend PRIVATE
        lldCommon
)
set_target_properties(JoyeerNativeBackend PROPERTIES
        OUTPUT_NAME joyeer-backend
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)
if(WIN32)
        set_target_properties(JoyeerNativeBackend PROPERTIES
                MSVC_RUNTIME_LIBRARY MultiThreaded
        )
endif()

if(WIN32)
        add_test(NAME NativeBackendHasNoExternalLLVMDlls
                COMMAND ${CMAKE_COMMAND}
                        -DLLVM_READOBJ=${JOYEER_LLVM_ROOT}/bin/llvm-readobj.exe
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DBACKEND_LIBRARY=$<TARGET_FILE:JoyeerNativeBackend>
                        -DEXPECTED_MACHINE=${JOYEER_WINDOWS_MACHINE}
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeBackendDependencies.cmake
                )
        set_tests_properties(NativeBackendHasNoExternalLLVMDlls PROPERTIES
                LABELS "llvm-backend;native;abi;packaging"
        )
endif()
