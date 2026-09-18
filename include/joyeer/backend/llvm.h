#ifndef __joyeer_backend_llvm_h__
#define __joyeer_backend_llvm_h__

#include "joyeer/compiler/options.h"
#include "joyeer/ir/ir.h"

#include <optional>
#include <string>
#include <vector>

namespace joyeer::llvmbackend {

using DebugInfoFormat = joyeer::DebugInfoFormat;

struct EmitOptions {
    bool emitLineTables = false;
    DebugInfoFormat debugInfoFormat = DebugInfoFormat::dwarf;
    OptimizationLevel optimizationLevel = OptimizationLevel::O2;
    bool emitVariables = false;
};

enum class DiagnosticId {
    invalidModule,
    unsupportedType,
    unsupportedInstruction,
    unsupportedExternal,
    invalidEntryPoint,
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
    bool hasEntryPoint = false;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

// Emits portable textual LLVM IR. The platform backend parses, verifies,
// optimizes, and compiles this text behind the versioned Joyeer C ABI.
class Emitter {
public:
    [[nodiscard]] Result emit(
            const ir::Module& module,
            const EmitOptions& options = {}) const;
};

[[nodiscard]] const char* diagnosticName(DiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<Diagnostic>& diagnostics);

} // namespace joyeer::llvmbackend

#endif
