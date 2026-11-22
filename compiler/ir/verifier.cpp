#include "verifier.h"

#include "compiler/ir/verifier/internal.h"

namespace joyeer::ir {

VerificationResult Verifier::verify(const Module& module) const {
    VerificationResult result;
    verifier::ModuleVerifier(module, result).run();
    return result;
}

const char* verificationErrorName(VerificationErrorId id) {
    switch (id) {
        case VerificationErrorId::duplicateId: return "ir.duplicate-id";
        case VerificationErrorId::invalidReference: return "ir.invalid-reference";
        case VerificationErrorId::invalidInstruction: return "ir.invalid-instruction";
        case VerificationErrorId::missingTerminator: return "ir.missing-terminator";
        case VerificationErrorId::instructionAfterTerminator:
            return "ir.instruction-after-terminator";
        case VerificationErrorId::typeMismatch: return "ir.type-mismatch";
        case VerificationErrorId::callMismatch: return "ir.call-mismatch";
        case VerificationErrorId::invalidSourceLocation: return "ir.invalid-source-location";
        case VerificationErrorId::unreachableSwitchCase: return "ir.unreachable-switch-case";
        case VerificationErrorId::nonExhaustiveSwitch: return "ir.non-exhaustive-switch";
    }
    return "ir.unknown";
}

SourceSpan verificationErrorSpan(const Module&, const VerificationError&) {
    return {};
}

} // namespace joyeer::ir
