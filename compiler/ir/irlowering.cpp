// The public entry points of IR lowering. The lowering itself is in compiler/ir/lowering/; its
// internal.h describes the order of the work and which file holds each part.

#include "irlowering.h"
#include "compiler/ir/lowering/internal.h"

#include <sstream>

namespace joyeer::lowering {

Result Lowerer::lower(
        const typing::TypeCheckedModel::Ptr& model,
        std::string sourceName,
        std::optional<ir::SourceInfo> sourceInfo,
        std::vector<ir::SourceInfo> sources) const {
    return detail::Builder().build(
            model, std::move(sourceName), std::move(sourceInfo), std::move(sources));
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::unsupportedSyntax: return "ir-lowering.unsupported-syntax";
        case DiagnosticId::missingType: return "ir-lowering.missing-type";
        case DiagnosticId::missingSymbol: return "ir-lowering.missing-symbol";
        case DiagnosticId::missingReturn: return "ir-lowering.missing-return";
        case DiagnosticId::ownershipViolation: return "ir-lowering.ownership-violation";
        case DiagnosticId::verificationFailed: return "ir-lowering.verification-failed";
        case DiagnosticId::invalidConstant: return "ir-lowering.invalid-constant";
    }
    return "ir-lowering.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length
            << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::lowering
