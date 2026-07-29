#include <lld/Common/Driver.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#if defined(_WIN32)
LLD_HAS_DRIVER(coff)
#elif defined(__APPLE__)
LLD_HAS_DRIVER(macho)
#else
LLD_HAS_DRIVER(elf)
#endif

int main() {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    const auto module = llvm::parseAssemblyString(
            "define i32 @joyeer_llvm_sdk_probe() { ret i32 0 }\n",
            diagnostic,
            context);
    if (!module) return 1;

#if defined(_WIN32)
    lld::Driver volatile driver = &lld::coff::link;
#elif defined(__APPLE__)
    lld::Driver volatile driver = &lld::macho::link;
#else
    lld::Driver volatile driver = &lld::elf::link;
#endif
    return driver == nullptr ? 1 : 0;
}
