#ifndef __joyeer_compiler_irlowering_h__
#define __joyeer_compiler_irlowering_h__

#include "joyeer/compiler/typechecking.h"
#include "joyeer/ir/ir.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::lowering {

enum class DiagnosticId {
    unsupportedSyntax,
    missingType,
    missingSymbol,
    missingReturn,
    ownershipViolation,
    verificationFailed,
};

struct Diagnostic {
    DiagnosticId id;
    SourceSpan span;
    std::string message;
};

struct Result {
    std::shared_ptr<ir::Module> module;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

class Lowerer {
public:
    [[nodiscard]] Result lower(
            const typing::TypeCheckedModel::Ptr& model,
            std::string sourceName = {},
            std::optional<ir::SourceInfo> sourceInfo = std::nullopt) const;
};

[[nodiscard]] const char* diagnosticName(DiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<Diagnostic>& diagnostics);

} // namespace joyeer::lowering

#endif
