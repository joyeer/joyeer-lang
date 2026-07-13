#ifndef __joyeer_compiler_nameresolution_h__
#define __joyeer_compiler_nameresolution_h__

#include "joyeer/compiler/semantic.h"

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
};

[[nodiscard]] const char* diagnosticName(NameResolutionDiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<NameResolutionDiagnostic>& diagnostics);

} // namespace joyeer::semantic

#endif