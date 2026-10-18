#include "llvm.h"
#include "backend/backend.h"
#include "backend/ir_wire.h"

#include <sstream>

namespace joyeer::llvmbackend {

namespace {
struct CallbackState {
    Result& result;
    bool failed = false;
};

// Callback storage belongs to the compiler. Never let an allocation exception
// unwind through the backend's C ABI or ask it to free compiler-owned strings.
void collectDiagnostic(void* context, const JoyeerBackendIRDiagnostic* diagnostic) noexcept {
    auto& state = *static_cast<CallbackState*>(context);
    try {
        state.result.diagnostics.push_back({static_cast<DiagnosticId>(diagnostic->id),
                {diagnostic->offset, diagnostic->length, diagnostic->sourceId},
                diagnostic->function == UINT32_MAX ? std::nullopt : std::optional(diagnostic->function),
                diagnostic->block == UINT32_MAX ? std::nullopt : std::optional(diagnostic->block),
                std::string(diagnostic->message, diagnostic->messageSize)});
    } catch (...) {
        state.failed = true;
    }
}

void collectText(void* context, const char* text, size_t size) noexcept {
    auto& state = *static_cast<CallbackState*>(context);
    try {
        state.result.text.assign(text, size);
    } catch (...) {
        state.failed = true;
    }
}
}

Result Emitter::emit(const ir::Module& module, const EmitOptions& options) const {
    Result result;
    const auto verification = ir::Verifier().verify(module);
    if (!verification.succeeded()) {
        for (const auto& error : verification.errors) {
            result.diagnostics.push_back({DiagnosticId::invalidModule, {}, error.function,
                    error.block, std::string(ir::verificationErrorName(error.id)) + ": " + error.message});
        }
        return result;
    }
    // Serialize the backend-neutral IR, not LLVM syntax. LLVMContext, Module,
    // IRBuilder and DIBuilder live entirely inside joyeer-backend.
    wire::Writer writer;
    writer(module);
    const JoyeerBackendModuleOptions buildOptions {
        JOYEER_BACKEND_ABI_VERSION, sizeof(JoyeerBackendModuleOptions), wire::version,
        writer.bytes.data(), writer.bytes.size(),
        static_cast<JoyeerBackendOptimizationLevel>(options.optimizationLevel),
        options.emitLineTables, options.emitVariables, options.debugInfoFormat == DebugInfoFormat::codeView,
    };
    JoyeerBackendModule* nativeModule = nullptr;
    CallbackState state {result};
    const auto status = joyeer_backend_build_module(&buildOptions,
            &nativeModule, collectDiagnostic, &state);
    result.module = std::shared_ptr<JoyeerBackendModule>(nativeModule, joyeer_backend_destroy_module);
    if (status != JOYEER_BACKEND_SUCCESS || state.failed || nativeModule == nullptr) {
        if (result.diagnostics.empty()) {
            result.diagnostics.push_back({DiagnosticId::invalidModule, {}, {}, {}, "native LLVM module construction failed"});
        }
        result.module.reset();
        return result;
    }
    result.hasEntryPoint = joyeer_backend_module_has_entry(nativeModule) != 0;
    // Text is an inspection artifact printed from LLVM objects. Native builds
    // retain the handle and do not parse this text back into another module.
        if (options.emitText &&
            (joyeer_backend_print_module(nativeModule, collectText, &state) != JOYEER_BACKEND_SUCCESS || state.failed)) {
        result.diagnostics.push_back({DiagnosticId::invalidModule, {}, {}, {}, "native LLVM module serialization failed"});
        result.text.clear();
        result.module.reset();
    }
    return result;
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::invalidModule: return "llvm.invalid-module";
        case DiagnosticId::unsupportedType: return "llvm.unsupported-type";
        case DiagnosticId::unsupportedInstruction: return "llvm.unsupported-instruction";
        case DiagnosticId::unsupportedExternal: return "llvm.unsupported-external";
        case DiagnosticId::invalidEntryPoint: return "llvm.invalid-entry-point";
    }
    return "llvm.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length;
        if (diagnostic.function.has_value()) out << " function=" << *diagnostic.function;
        if (diagnostic.block.has_value()) out << " block=" << *diagnostic.block;
        out << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::llvmbackend