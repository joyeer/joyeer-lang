#ifndef __joyeer_backend_linker_h__
#define __joyeer_backend_linker_h__

#include "compiler/driver/options.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct JoyeerBackendModule;

namespace joyeer::native {

enum class LinkDiagnosticId {
    missingEntryPoint,
    missingTool,
    fileError,
    toolFailure,
};

struct LinkDiagnostic {
    LinkDiagnosticId id;
    std::string message;
};

struct LinkOptions {
    std::filesystem::path runtimeLibrary;
    std::filesystem::path sdkRoot;
    std::string sdkVersion;
    std::string deploymentTarget;
    std::filesystem::path debugSymbolTool;
    std::filesystem::path outputFile;
    OptimizationLevel optimizationLevel = OptimizationLevel::O2;
    DebugInfoOptions debugInfo;
    std::filesystem::path sourceFile;
    // Native compilation consumes objects directly; text remains a fallback
    // for standalone linker tests and explicit LLVM inspection workflows.
    std::shared_ptr<JoyeerBackendModule> module;
    std::vector<std::filesystem::path> nativeLibraries;
};

struct LinkResult {
    std::vector<LinkDiagnostic> diagnostics;
    std::optional<std::filesystem::path> debugArtifact;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

class Linker {
public:
    [[nodiscard]] LinkResult emitObject(
            const std::string& llvmIR,
            const LinkOptions& options) const;
    [[nodiscard]] LinkResult link(
            const std::string& llvmIR,
            bool hasEntryPoint,
            const LinkOptions& options) const;
};

[[nodiscard]] const char* diagnosticName(LinkDiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<LinkDiagnostic>& diagnostics);

} // namespace joyeer::native

#endif
