// Checks of direct calls, function values (decision 0020) and indirect calls against the
// callee's signature or function type.

#include "internal.h"

#include <algorithm>

namespace joyeer::ir::verifier {

namespace {

// How an argument for a parameter with `effect` is passed: `inout` and `initializing` by address
// (spec 4.2), the others by value.
ValueCategory effectCategory(syntax::AccessEffect effect) {
    return effect == syntax::AccessEffect::inout || effect == syntax::AccessEffect::initializing
            ? ValueCategory::address
            : ValueCategory::value;
}

} // namespace

// A direct call matches its callee's parameters and result; C bindings and literal factories
// need authorization or verified literal storage.
void FunctionVerifier::verifyCall(const InstructionSite& site) {
    const auto& block = site.block;
    const auto index = site.index;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto knownCallee = site.knownCallee;
    if (!instruction.targets.empty()) {
        report(
                VerificationErrorId::invalidInstruction,
                functionId,
                block.id,
                location,
                "call must not contain block targets");
    }
    if (!knownCallee) {
        report(
                VerificationErrorId::invalidReference,
                functionId,
                block.id,
                location,
                "call references an unknown function");
        return;
    }
    const auto& callee = *functions.at(*instruction.callee);
    if (!function.runtimeContext && callee.literalFactory &&
        (instruction.operands.size() != 3 ||
         !literalBlocks.contains(instruction.operands[1]))) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "a literal factory requires verified literal storage");
    }
    if (callee.nativeBinding.has_value() && !function.runtimeContext) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "direct call to a C binding requires runtime authorization");
    }
    bool matches = instruction.operands.size() == callee.parameters.size();
    const auto count = std::min(
            instruction.operands.size(),
            callee.parameters.size());
    for (size_t argument = 0; argument < count; ++argument) {
            matches &= operands[argument] != nullptr &&
                operands[argument]->category ==
                    callee.parameters[argument].value.category &&
            (callee.parameters[argument].acceptsAnyType ||
             operands[argument]->type ==
                 callee.parameters[argument].value.type);
    }
    matches &= callee.returnsValue == instruction.result.has_value();
    if (callee.returnsValue && instruction.result.has_value()) {
        matches &= instruction.result->category == ValueCategory::value &&
                instruction.result->type == callee.resultType;
    }
    if (!matches) {
        report(
                VerificationErrorId::callMismatch,
                functionId,
                block.id,
                location,
                "call arguments or result do not match callee signature");
    }
    // A call that cannot return ends its block (decision 0020).
    const auto* calleeResult = types.contains(callee.resultType) ? types.at(callee.resultType) : nullptr;
    if (calleeResult != nullptr && calleeResult->kind == typing::TypeKind::never &&
        (index + 1 >= block.instructions.size() ||
         block.instructions[index + 1].opcode != Opcode::unreachable)) {
        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
               "call of a function returning Never must be followed by unreachable");
    }
}

// A function value of exactly the callee's function type.
void FunctionVerifier::verifyFunctionAddress(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto knownCallee = site.knownCallee;
    if (!requireShape(site, 0, 0)) return;
    if (!knownCallee) {
        report(VerificationErrorId::invalidReference, functionId, block.id, location,
               "function_address references an unknown function");
        return;
    }
    const auto& callee = *functions.at(*instruction.callee);
    const auto* functionType = instruction.result.has_value() &&
                    types.contains(instruction.result->type)
            ? types.at(instruction.result->type)
            : nullptr;
    if (callee.literalFactory) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "literal factories cannot be formed as function values");
    }
    // Only a Joyeer function has the Joyeer convention; a C import, a data symbol
    // and a builtin accepting any type have no address of a function type.
    if (callee.nativeBinding.has_value() && !function.runtimeContext) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "function_address of a C binding requires runtime authorization");
    }
    // The root `main` is not a value (decision 0020).
    bool matches = !callee.isExternal && !(callee.isRootModule && callee.name == "main") &&
            (!callee.nativeBinding.has_value() ||
             (callee.nativeBinding->kind == NativeBindingKind::cExport &&
              !callee.nativeBinding->dataSymbol)) &&
            functionType != nullptr &&
            instruction.result->category == ValueCategory::value &&
            functionType->kind == typing::TypeKind::function &&
            functionType->effects.size() == callee.parameters.size() &&
            functionType->arguments.size() == callee.parameters.size() + 1;
    for (size_t parameter = 0; matches && parameter < callee.parameters.size(); ++parameter) {
        const auto& declared = callee.parameters[parameter];
        const auto effect = functionType->effects[parameter];
        matches &= !declared.acceptsAnyType &&
                declared.value.type == functionType->arguments[parameter] &&
                declared.value.category == effectCategory(effect) &&
                declared.isConsuming == (effect == syntax::AccessEffect::consuming) &&
                declared.isInitializing == (effect == syntax::AccessEffect::initializing);
    }
    if (matches) matches = callee.resultType == functionType->arguments.back();
    if (!matches) {
        report(VerificationErrorId::typeMismatch, functionId, block.id, location,
               "function_address requires a Joyeer function and a result of exactly its function type");
    }
}

// A call through a function value matches its function type.
void FunctionVerifier::verifyIndirectCall(const InstructionSite& site) {
    const auto& block = site.block;
    const auto index = site.index;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (!instruction.targets.empty() || instruction.callee.has_value() ||
        instruction.operands.empty() || operands[0] == nullptr) {
        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
               "call_indirect needs a function value operand and no callee or targets");
        return;
    }
    const auto* functionType = types.contains(operands[0]->type)
            ? types.at(operands[0]->type)
            : nullptr;
    if (operands[0]->category != ValueCategory::value || functionType == nullptr ||
        functionType->kind != typing::TypeKind::function ||
        functionType->arguments.size() != functionType->effects.size() + 1) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "call_indirect operand 0 must be a value of function type");
        return;
    }
    const auto parameterCount = functionType->effects.size();
    bool matches = instruction.operands.size() == parameterCount + 1;
    for (size_t parameter = 0; matches && parameter < parameterCount; ++parameter) {
        const auto* operand = operands[parameter + 1];
        matches &= operand != nullptr &&
                operand->category == effectCategory(functionType->effects[parameter]) &&
                operand->type == functionType->arguments[parameter];
    }
    const auto resultType = functionType->arguments.back();
    const auto* result = types.contains(resultType) ? types.at(resultType) : nullptr;
    const bool isNever = result != nullptr && result->kind == typing::TypeKind::never;
    const bool returnsValue = result != nullptr && !isNever &&
            result->kind != typing::TypeKind::voidType;
    matches &= returnsValue == instruction.result.has_value();
    if (returnsValue && instruction.result.has_value()) {
        matches &= instruction.result->category == ValueCategory::value &&
                instruction.result->type == resultType;
    }
    if (!matches) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "call_indirect arguments or result do not match the function type");
    }
    // A call that cannot return ends its block.
    if (isNever && (index + 1 >= block.instructions.size() ||
                    block.instructions[index + 1].opcode != Opcode::unreachable)) {
        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
               "call_indirect of a function returning Never must be followed by unreachable");
    }
}

} // namespace joyeer::ir::verifier
