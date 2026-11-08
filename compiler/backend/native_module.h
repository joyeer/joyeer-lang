#ifndef JOYEER_BACKEND_NATIVE_MODULE_H
#define JOYEER_BACKEND_NATIVE_MODULE_H

#include "backend.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <memory>

namespace joyeer::llvmbackend::native {
// Context must outlive its Module. Keep this storage separate from the
// Joyeer lowering session so code generation does not depend on frontend types.
struct ModuleStorage {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module;
};
}

// The public C header forward-declares this handle. Its layout and LLVM
// ownership are private to this shared library, including on Windows /MT.
struct JoyeerBackendModule {
    joyeer::llvmbackend::native::ModuleStorage storage;
    bool hasEntryPoint = false;
};

#endif