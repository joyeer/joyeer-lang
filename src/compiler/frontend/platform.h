#ifndef __joyeer_compiler_platform_h__
#define __joyeer_compiler_platform_h__

#include "token.h"

#include <optional>
#include <string>
#include <vector>

namespace joyeer::semantic {

struct ModuleInput;

// The operating systems a program can be compiled for. `@platform` selects
// among them before name resolution.
enum class TargetPlatform {
    windows,
    linux,
    macos,
};

// The platform this compiler was built for; the backend generates code only
// for the host, so this is the default target.
[[nodiscard]] TargetPlatform hostPlatform();
[[nodiscard]] const char* platformName(TargetPlatform platform);

enum class PlatformDiagnosticId {
    invalidAnnotation,
    unknownName,
    overlappingVariants,
    signatureMismatch,
    nameCollision,
};

struct PlatformDiagnostic {
    PlatformDiagnosticId id;
    SourceSpan span;
    std::string message;
    std::string help;
    // The declaration this one conflicts with, when there is one.
    std::optional<SourceSpan> related;
    std::string relatedMessage;
};

// Removes top-level functions whose `@platform` list excludes `target` and
// strips the annotation from the rest, editing each module's syntax in place.
// Variant rules (§11.5) are checked for every module whatever the target. A
// module with diagnostics is left unmodified. Call it once per parsed tree,
// before name resolution; a second call finds no annotations left.
[[nodiscard]] std::vector<PlatformDiagnostic> selectPlatformDeclarations(
        const std::vector<ModuleInput>& modules,
        TargetPlatform target);

[[nodiscard]] const char* diagnosticName(PlatformDiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<PlatformDiagnostic>& diagnostics);

} // namespace joyeer::semantic

#endif
