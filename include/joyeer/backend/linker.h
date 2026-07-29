#ifndef __joyeer_backend_linker_h__
#define __joyeer_backend_linker_h__

#include "joyeer/compiler/options.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
    std::filesystem::path clangExecutable;
    std::filesystem::path runtimeLibrary;
    std::string msvcRuntimeLibrary;
    std::filesystem::path sdkRoot;
    std::filesystem::path outputFile;
    OptimizationLevel optimizationLevel = OptimizationLevel::O2;
    DebugInfoOptions debugInfo;
    std::filesystem::path sourceFile;
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
    [[nodiscard]] LinkResult link(
            const std::string& llvmIR,
            bool hasEntryPoint,
            const LinkOptions& options) const;
};

[[nodiscard]] const char* diagnosticName(LinkDiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<LinkDiagnostic>& diagnostics);

} // namespace joyeer::native

#endif
