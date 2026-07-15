#ifndef __joyeer_compiler_semanticanalysis_h__
#define __joyeer_compiler_semanticanalysis_h__

#include "joyeer/compiler/typechecking.h"

#include <string>
#include <vector>

namespace joyeer::analysis {

enum class Severity {
    warning,
    error,
};

enum class DiagnosticId {
    missingReturn,
    unreachableCode,
    useBeforeInitialization,
    useAfterConsume,
    unusedBinding,
};

struct Diagnostic {
    DiagnosticId id;
    Severity severity;
    SourceSpan span;
    std::string message;
};

struct Result {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const;
};

class Analyzer {
public:
    [[nodiscard]] Result analyze(
            const typing::TypeCheckedModel::Ptr& model) const;
};

[[nodiscard]] const char* diagnosticName(DiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<Diagnostic>& diagnostics);

} // namespace joyeer::analysis

#endif
