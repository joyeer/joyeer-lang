#include "native_module.h"
#include "native_ir.h"
#include "ir_wire.h"

namespace {
void reportIRDiagnostic(JoyeerNativeBackendIRDiagnosticCallback callback, void* context,
        const joyeer::llvmbackend::Diagnostic& diagnostic) {
    if (callback == nullptr) return;
    const JoyeerNativeBackendIRDiagnostic result {
        static_cast<uint32_t>(diagnostic.id), diagnostic.span.sourceId,
        diagnostic.span.offset, diagnostic.span.length,
        diagnostic.function.value_or(UINT32_MAX), diagnostic.block.value_or(UINT32_MAX),
        diagnostic.message.data(), diagnostic.message.size(),
    };
    callback(context, &result);
}
}

extern "C" JoyeerNativeBackendStatus joyeer_native_backend_build_module(
        const JoyeerNativeBackendModuleOptions* options, JoyeerNativeBackendModule** result,
        JoyeerNativeBackendIRDiagnosticCallback callback, void* context) {
    if (result != nullptr) *result = nullptr;
    if (options == nullptr || result == nullptr ||
            options->abiVersion != JOYEER_NATIVE_BACKEND_ABI_VERSION ||
            options->structSize < sizeof(*options) || options->irVersion != 1 ||
            options->joyeerIR == nullptr || options->joyeerIRSize == 0 ||
            options->optimizationLevel > JOYEER_NATIVE_BACKEND_O3 ||
            options->emitLineTables > 1 || options->emitVariables > 1 || options->codeView > 1) {
        reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {},
                "invalid native backend module options"});
        return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
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
            return JOYEER_NATIVE_BACKEND_INVALID_IR;
        }
        auto module = std::make_unique<JoyeerNativeBackendModule>();
        joyeer::llvmbackend::native::Builder builder(module->storage, source, {
            options->emitLineTables != 0,
            options->codeView != 0 ? joyeer::DebugInfoFormat::codeView : joyeer::DebugInfoFormat::dwarf,
            static_cast<joyeer::OptimizationLevel>(options->optimizationLevel),
            options->emitVariables != 0,
        });
        const auto diagnostics = builder.build();
        for (const auto& diagnostic : diagnostics) reportIRDiagnostic(callback, context, diagnostic);
        if (!diagnostics.empty()) return JOYEER_NATIVE_BACKEND_INVALID_IR;
        module->hasEntryPoint = builder.hasEntryPoint;
        *result = module.release();
        return JOYEER_NATIVE_BACKEND_SUCCESS;
    } catch (const std::exception& error) {
        reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {}, error.what()});
    } catch (...) {
        reportIRDiagnostic(callback, context, {joyeer::llvmbackend::DiagnosticId::invalidModule, {}, {}, {},
                "unknown native module construction failure"});
    }
    return JOYEER_NATIVE_BACKEND_INTERNAL_ERROR;
}

extern "C" void joyeer_native_backend_destroy_module(JoyeerNativeBackendModule* module) {
    delete module;
}

extern "C" int joyeer_native_backend_module_has_entry(const JoyeerNativeBackendModule* module) {
    return module != nullptr && module->hasEntryPoint;
}

extern "C" JoyeerNativeBackendStatus joyeer_native_backend_print_module(
        const JoyeerNativeBackendModule* module, JoyeerNativeBackendTextCallback callback, void* context) {
    if (module == nullptr || callback == nullptr) return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
    try {
        std::string text;
        llvm::raw_string_ostream output(text);
        module->storage.module->print(output, nullptr);
        callback(context, text.data(), text.size());
        return JOYEER_NATIVE_BACKEND_SUCCESS;
    } catch (...) {
        return JOYEER_NATIVE_BACKEND_INTERNAL_ERROR;
    }
}