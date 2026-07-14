add_joyeer_library(JoyeerLLVMBackend STATIC
        llvm.cpp
)

target_link_libraries(JoyeerLLVMBackend PUBLIC JoyeerIR)
