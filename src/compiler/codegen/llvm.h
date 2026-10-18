#ifndef __joyeer_backend_llvm_h__
#define __joyeer_backend_llvm_h__

#include "compiler/driver/options.h"
#include "compiler/ir/ir.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>

struct JoyeerBackendModule;

namespace joyeer::llvmbackend {

using DebugInfoFormat = joyeer::DebugInfoFormat;

struct EmitOptions {
    bool emitLineTables = false;
    DebugInfoFormat debugInfoFormat = DebugInfoFormat::dwarf;
    OptimizationLevel optimizationLevel = OptimizationLevel::O2;
    bool emitVariables = false;
    // Inspection callers keep text by default; native builds need only a handle.
    bool emitText = true;
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
    // Shared compiler-side ownership invokes the backend's C destructor.
    // The handle exposes no LLVM objects or cross-library C++ allocation.
    std::shared_ptr<JoyeerBackendModule> module;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

// Builds verified LLVM objects behind the versioned Joyeer C ABI. Result::text
// is LLVM's portable serialization; native code generation uses Result::module.
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
