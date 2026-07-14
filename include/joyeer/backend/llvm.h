#ifndef __joyeer_backend_llvm_h__
#define __joyeer_backend_llvm_h__

#include "joyeer/ir/ir.h"

#include <optional>
#include <string>
#include <vector>

namespace joyeer::llvmbackend {

enum class DiagnosticId {
    invalidModule,
    unsupportedType,
    unsupportedInstruction,
    unsupportedExternal,
};

struct Diagnostic {
    DiagnosticId id;
    SourceSpan span;
    std::optional<ir::FunctionId> function;
    std::optional<ir::BlockId> block;
    std::string message;
};

struct Result {
    std::string text;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

// Emits portable textual LLVM IR. The official Clang driver parses, verifies,
// optimizes, and compiles this text; no unstable LLVM C++ ABI is linked into
// the Joyeer compiler process.
class Emitter {
public:
    [[nodiscard]] Result emit(const ir::Module& module) const;
};

[[nodiscard]] const char* diagnosticName(DiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<Diagnostic>& diagnostics);

} // namespace joyeer::llvmbackend

#endif
