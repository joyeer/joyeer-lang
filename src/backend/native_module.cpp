#include "native_module.h"
#include "native_ir.h"
#include "ir_wire.h"

namespace {
void reportIRDiagnostic(JoyeerBackendIRDiagnosticCallback callback, void* context,
        const joyeer::llvmbackend::Diagnostic& diagnostic) {
    if (callback == nullptr) return;
    const JoyeerBackendIRDiagnostic result {
        static_cast<uint32_t>(diagnostic.id), diagnostic.span.sourceId,
        diagnostic.span.offset, diagnostic.span.length,
        diagnostic.function.value_or(UINT32_MAX), diagnostic.block.value_or(UINT32_MAX),
        diagnostic.message.data(), diagnostic.message.size(),
    };
    callback(context, &result);
}
}

extern "C" JoyeerBackendStatus joyeer_backend_build_module(
        const JoyeerBackendModuleOptions* options, JoyeerBackendModule** result,
        JoyeerBackendIRDiagnosticCallback callback, void* context) {
    if (result != nullptr) *result = nullptr;
    if (options == nullptr || result == nullptr ||
            options->abiVersion != JOYEER_BACKEND_ABI_VERSION ||
            options->structSize < sizeof(*options) ||
            options->irVersion != joyeer::llvmbackend::wire::version ||
            options->joyeerIR == nullptr || options->joyeerIRSize == 0 ||
            options->optimizationLevel > JOYEER_BACKEND_O3 ||
            options->emitLineTables > 1 || options->emitVariables > 1 || options->codeView > 1) {
        reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {},
                "invalid native backend module options"});
        return JOYEER_BACKEND_INVALID_ARGUMENT;
    }
    // Decode into backend-owned values. No frontend pointers or STL ownership
    // survive this call; the returned module owns only its LLVM context and IR.
    try {
        joyeer::ir::Module source;
        joyeer::llvmbackend::wire::Reader reader({options->joyeerIR, options->joyeerIRSize});
        reader(source);
        if (!reader.succeeded()) {
            reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {},
                    "malformed Joyeer IR wire payload"});
            return JOYEER_BACKEND_INVALID_IR;
        }
        auto module = std::make_unique<JoyeerBackendModule>();
        joyeer::llvmbackend::native::Builder builder(module->storage, source, {
            options->emitLineTables != 0,
            options->codeView != 0 ? joyeer::DebugInfoFormat::codeView : joyeer::DebugInfoFormat::dwarf,
            static_cast<joyeer::OptimizationLevel>(options->optimizationLevel),
            options->emitVariables != 0,
        });
        const auto diagnostics = builder.build();
        for (const auto& diagnostic : diagnostics) reportIRDiagnostic(callback, context, diagnostic);
        if (!diagnostics.empty()) return JOYEER_BACKEND_INVALID_IR;
        module->hasEntryPoint = builder.hasEntryPoint;
        *result = module.release();
        return JOYEER_BACKEND_SUCCESS;
    } catch (const std::exception& error) {
        reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {}, error.what()});
    } catch (...) {
        reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {},
                "unknown native module construction failure"});
    }
    return JOYEER_BACKEND_INTERNAL_ERROR;
}

extern "C" void joyeer_backend_destroy_module(JoyeerBackendModule* module) {
    delete module;
}

extern "C" int joyeer_backend_module_has_entry(const JoyeerBackendModule* module) {
    return module != nullptr && module->hasEntryPoint;
}

extern "C" JoyeerBackendStatus joyeer_backend_print_module(
        const JoyeerBackendModule* module, JoyeerBackendTextCallback callback, void* context) {
    if (module == nullptr || callback == nullptr) return JOYEER_BACKEND_INVALID_ARGUMENT;
    try {
        std::string text;
        llvm::raw_string_ostream output(text);
        module->storage.module->print(output, nullptr);
        callback(context, text.data(), text.size());
        return JOYEER_BACKEND_SUCCESS;
    } catch (...) {
        return JOYEER_BACKEND_INTERNAL_ERROR;
    }
}