#ifndef __joyeer_compiler_nameresolution_h__
#define __joyeer_compiler_nameresolution_h__

#include "semantic.h"

#include <string>
#include <vector>

namespace joyeer::semantic {

enum class NameResolutionDiagnosticId {
    duplicateDeclaration,
    undefinedName,
    undefinedType,
    unknownMember,
    unknownEnumCase,
    notCallable,
    unexpectedArgumentClause,
    argumentCountMismatch,
    missingArgumentLabel,
    unexpectedArgumentLabel,
    argumentOutOfOrder,
    inaccessibleDeclaration,
    inaccessibleExposedType,
    importCollision,
    unknownImport,
    ambiguousImport,
    invalidExtensionTarget,
    selfOutsideType,
    reservedName,
    invalidPrelude,
};

// Which bundled prelude a test-supplied module replaces (decisions 0024 and 0026). The concurrency prelude is
// replaced by a module with `preludeContext` set and no replacement named, as before.
enum class ReplacedPrelude {
    none,
    roles,
    collections,
};

struct ModuleInput {
    std::string name;
    std::vector<syntax::SourceFileSyntax::Ptr> files;
    bool runtimeContext = false;
    // A core unit may use only scalars (spec 19.4.6); implies runtimeContext.
    bool coreContext = false;
    // Set only by the compiler, never by --runtime-module.
    bool preludeContext = false;
    // Test-only: this module replaces the named bundled prelude; it also sets preludeContext. Never set by
    // the driver.
    ReplacedPrelude replaces = ReplacedPrelude::none;
};

struct NameResolutionDiagnostic {
    NameResolutionDiagnosticId id;
    SourceSpan span;
    std::string message;
};

struct NameResolutionResult {
    SemanticModel::Ptr model;
    std::vector<NameResolutionDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

class NameResolver {
public:
    [[nodiscard]] NameResolutionResult resolve(
            const syntax::SourceFileSyntax::Ptr& root) const;
    [[nodiscard]] NameResolutionResult resolve(
            const std::vector<ModuleInput>& modules) const;

private:
    [[nodiscard]] NameResolutionResult resolveWithPrelude(
            std::vector<ModuleInput> modules, bool singleFile) const;
};

[[nodiscard]] std::vector<NameResolutionDiagnostic> validateCallArguments(
        const syntax::CallExprSyntax& call,
        const Symbol& target);

[[nodiscard]] const char* diagnosticName(NameResolutionDiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<NameResolutionDiagnostic>& diagnostics);

} // namespace joyeer::semantic

#endif