// ModuleVerifier: the order of the module-wide phases, the reporting helpers every phase
// uses, and the function table. The phases themselves are in definitions.cpp and debuginfo.cpp.

#include "internal.h"

#include <algorithm>
#include <utility>

namespace joyeer::ir::verifier {

ModuleVerifier::ModuleVerifier(const Module& module, VerificationResult& result)
    : module(module), result(result), destruction(module) {}

void ModuleVerifier::run() {
    // Source tables first: debug locations checked by later phases need them.
    verifySourceFiles();

    // Types and definitions (definitions.cpp). Each phase may read the tables earlier ones built.
    indexTypes();
    verifyTypeNames();
    computeNoncopyableTypes();
    computeNonConcreteTypes();
    verifyConstants();
    verifyStructures();
    verifyEnumerations();
    verifyEnumerationRoles();
    indexFunctions();
    verifyDeinitializers();
    verifyEntryTypes();
    verifyElementAddressors();
    verifyCopiers();
    verifyLiteralFactories();

    // Module-wide debug metadata (debuginfo.cpp).
    verifyDebugScopes();
    verifyDebugVariables();

    // Function signatures and bodies, in module order (function.cpp).
    for (const auto& function : module.functions) {
        FunctionVerifier(*this, function).run();
    }
}

void ModuleVerifier::report(
        VerificationErrorId id,
        std::optional<FunctionId> function,
        std::optional<BlockId> block,
        std::optional<size_t> instruction,
        std::string message) {
    result.errors.push_back(VerificationError {
        id,
        function,
        block,
        instruction,
        std::move(message),
    });
}

void ModuleVerifier::verifyConcreteType(TypeId type,
                                        std::optional<FunctionId> function,
                                        std::optional<BlockId> block,
                                        std::optional<size_t> instruction,
                                        const std::string& owner) {
    if (!nonConcreteTypes.contains(type)) return;
    report(VerificationErrorId::typeMismatch, function, block, instruction,
           owner + " uses non-concrete type '" + types.at(type)->name + "'");
}

const EnumCaseDefinition* ModuleVerifier::enumCaseFor(TypeId type, semantic::SymbolId symbol) const {
    const auto enumeration = enumerations.find(type);
    if (enumeration == enumerations.end()) return nullptr;
    const auto found = std::find_if(
            enumeration->second->cases.begin(),
            enumeration->second->cases.end(),
            [symbol](const auto& enumCase) { return enumCase.symbol == symbol; });
    return found == enumeration->second->cases.end() ? nullptr : &*found;
}

void ModuleVerifier::indexFunctions() {
    for (const auto& function : module.functions) {
        if (!functions.emplace(function.id, &function).second) {
            report(
                    VerificationErrorId::duplicateId,
                    function.id,
                    std::nullopt,
                    std::nullopt,
                    "duplicate function id " + std::to_string(function.id));
        }
    }
}

} // namespace joyeer::ir::verifier
