#ifndef __joyeer_ir_verifier_h__
#define __joyeer_ir_verifier_h__

// The structural IR verifier. Every module is verified after lowering and again inside the
// backend before LLVM emission. The implementation lives in verifier/; start with
// verifier/internal.h, which describes the phases and where each one is implemented.

#include "module.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::ir {

enum class VerificationErrorId {
    duplicateId,
    invalidReference,
    invalidInstruction,
    missingTerminator,
    instructionAfterTerminator,
    typeMismatch,
    callMismatch,
    invalidSourceLocation,
    // A `switch_pattern` case after a top-level wildcard, which no value can reach.
    unreachableSwitchCase,
    // A `switch_pattern` whose cases do not cover every value of its scrutinee.
    nonExhaustiveSwitch,
};

struct VerificationError {
    VerificationErrorId id;
    std::optional<FunctionId> function;
    std::optional<BlockId> block;
    std::optional<size_t> instruction;
    std::string message;
};

struct VerificationResult {
    std::vector<VerificationError> errors;
    // The elementary steps of the literal protocols' control-flow walks, so tests can bound their growth without
    // timing them.
    size_t literalPathWork = 0;

    [[nodiscard]] bool succeeded() const {
        return errors.empty();
    }
};

class Verifier {
public:
    [[nodiscard]] VerificationResult verify(const Module& module) const;
};

// The stable diagnostic ID of `id`, such as "ir.type-mismatch".
[[nodiscard]] const char* verificationErrorName(VerificationErrorId id);

// The source span of the instruction `error` names in `module`, or an empty span when it names none or one the
// module does not have. Lowering reports verifier errors at this span.
[[nodiscard]] SourceSpan verificationErrorSpan(const Module& module, const VerificationError& error);

} // namespace joyeer::ir

#endif
