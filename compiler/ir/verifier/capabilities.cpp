// Checks of the runtime-only operations: layout queries (spec 20.12.5), collection
// capabilities (decisions 0025, 0026) and conversions between Ints and addresses. User code
// reaches them only through verified literal storage (literals.cpp) or a collection's
// element addressor; everything else needs runtime authorization.

#include "internal.h"

namespace joyeer::ir::verifier {

// `typeSize` and `typeAlignment`.
void FunctionVerifier::verifyLayoutQuery(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto queried = static_cast<TypeId>(instruction.integerValue);
    const auto* result = instruction.result.has_value() && types.contains(instruction.result->type)
            ? types.at(instruction.result->type) : nullptr;
    if (!types.contains(queried) || result == nullptr ||
        result->kind != typing::TypeKind::integer ||
        instruction.result->category != ValueCategory::value) {
        report(
                VerificationErrorId::typeMismatch,
                functionId, block.id, location,
                "a size or alignment query names a known type and yields an Int");
    } else if (nonConcreteTypes.contains(queried) ||
               types.at(queried)->kind == typing::TypeKind::never) {
        report(
                VerificationErrorId::typeMismatch,
                functionId, block.id, location,
                "a size or alignment query needs a concrete type with values, not '" +
                        types.at(queried)->name + "'");
    }
    if (site.unauthorized) {
        report(
                VerificationErrorId::callMismatch,
                functionId, block.id, location,
                "a layout query requires runtime authorization");
    }
}

// `destroyRange`, `cloneAt`, `linearRangeGuard`, `slotAddress`, `stackBlock`, `cloneCallback` and
// `destroyCallback`: Int operands, a concrete (and for clones, copyable) type, and authorization
// unless the instruction is admitted literal storage.
void FunctionVerifier::verifyCollectionCapability(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const bool yieldsInt = producesValue(instruction.opcode);
    const bool guard = instruction.opcode == Opcode::linearRangeGuard;
    const bool stackBlockOp = instruction.opcode == Opcode::stackBlock;
    if (site.shapeMatches) {
        for (const auto* operand : operands) {
            const auto* operandType = operand != nullptr && types.contains(operand->type)
                    ? types.at(operand->type) : nullptr;
            if (operandType == nullptr || operandType->kind != typing::TypeKind::integer ||
                operand->category != ValueCategory::value) {
                report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                       std::string(opcodeName(instruction.opcode)) + " takes Int operands");
            }
        }
    }
    if (yieldsInt) {
        const auto* result = instruction.result.has_value() && types.contains(instruction.result->type)
                ? types.at(instruction.result->type) : nullptr;
        if (result == nullptr || result->kind != typing::TypeKind::integer ||
            instruction.result->category != ValueCategory::value) {
            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                   std::string(opcodeName(instruction.opcode)) + " yields an Int");
        }
    } else if (instruction.result.has_value()) {
        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
               std::string(opcodeName(instruction.opcode)) + " yields no value");
    }
    if (stackBlockOp && instruction.operands.size() == 1 && operands[0] != nullptr) {
        // The count is a constant, so the backend sizes the storage at compile time.
        const auto definition = valueDefinitions.find(operands[0]->id);
        const auto definingBlock = definition == valueDefinitions.end() ? blocks.end()
                : blocks.find(definition->second.first);
        const auto* defining = definingBlock != blocks.end() &&
                definition->second.second < definingBlock->second->instructions.size()
                ? &definingBlock->second->instructions[definition->second.second] : nullptr;
        if (defining == nullptr || defining->opcode != Opcode::integerConstant ||
            defining->integerValue < 0) {
            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                   "stack_block takes a non-negative constant count");
        }
    }
    if (!guard) {
        const auto queried = static_cast<TypeId>(instruction.integerValue);
        const auto* count = stackBlockOp && instruction.operands.size() == 1
                ? definingInstruction(instruction.operands[0]) : nullptr;
        const bool emptyNeverLiteral = count != nullptr &&
                count->opcode == Opcode::integerConstant && count->integerValue == 0 &&
                instruction.result.has_value() &&
                literalBlocks.contains(instruction.result->id);
        if (!types.contains(queried) || nonConcreteTypes.contains(queried) ||
            (types.at(queried)->kind == typing::TypeKind::never && !emptyNeverLiteral)) {
            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                   std::string(opcodeName(instruction.opcode)) +
                           " needs a concrete type with values");
        } else if ((instruction.opcode == Opcode::cloneAt ||
                    instruction.opcode == Opcode::cloneCallback) &&
                   noncopyableTypes.contains(queried)) {
            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                   std::string(opcodeName(instruction.opcode)) +
                           " needs a copyable type, but '" + types.at(queried)->name +
                           "' is noncopyable");
        }
    }
    const bool literalStorage = instruction.result.has_value() &&
            ((instruction.opcode == Opcode::stackBlock &&
              literalBlocks.contains(instruction.result->id)) ||
             (instruction.opcode == Opcode::slotAddress &&
              literalSlots.contains(instruction.result->id)));
    if (site.unauthorized && !literalStorage) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "a collection capability requires runtime authorization");
    }
}

// An Int turned into a typed address: runtime code, admitted literal places, or a collection's
// element place.
void FunctionVerifier::verifyIntegerAddress(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto* source = site.shapeMatches && operands[0] != nullptr && types.contains(operands[0]->type)
            ? types.at(operands[0]->type) : nullptr;
    if (source == nullptr || source->kind != typing::TypeKind::integer ||
        operands[0]->category != ValueCategory::value ||
        !instruction.result.has_value() ||
        instruction.result->category != ValueCategory::address ||
        !types.contains(instruction.result->type)) {
        report(
                VerificationErrorId::typeMismatch,
                functionId, block.id, location,
                "an integer address takes an Int and yields the address of a known type");
    } else if (types.at(instruction.result->type)->kind == typing::TypeKind::never) {
        report(
                VerificationErrorId::typeMismatch,
                functionId, block.id, location,
                "an integer address cannot address a slot of 'Never'");
    }
    // A library array's element place is the one integer address user code may form: its
    // operand is the result of a call of an element addressor (decision 0025).
    bool fromAddressor = false;
    std::optional<TypeId> addressedElement;
    if (operands.size() == 1 && operands[0] != nullptr) {
        const auto definition = valueDefinitions.find(operands[0]->id);
        const auto definingBlock = definition == valueDefinitions.end() ? blocks.end()
                : blocks.find(definition->second.first);
        const auto* defining = definingBlock != blocks.end() &&
                definition->second.second < definingBlock->second->instructions.size()
                ? &definingBlock->second->instructions[definition->second.second] : nullptr;
        const auto callee = defining != nullptr && defining->opcode == Opcode::call &&
                defining->callee.has_value() ? functions.find(*defining->callee) : functions.end();
        fromAddressor = callee != functions.end() && callee->second->elementAddressor;
        if (fromAddressor && !callee->second->parameters.empty() &&
            types.contains(callee->second->parameters[0].value.type)) {
            const auto& receiver = *types.at(callee->second->parameters[0].value.type);
            const auto collection = structures.find(receiver.id);
            const auto element = collection != structures.end()
                    ? collectionElementArgument(collection->second->role) : 0;
            if (element < receiver.arguments.size()) {
                addressedElement = receiver.arguments[element];
            }
        }
    }
    if (fromAddressor && instruction.result.has_value() &&
        instruction.result->type != addressedElement.value_or(instruction.result->type)) {
        report(VerificationErrorId::typeMismatch, functionId, block.id, location,
               "an element address must have the element type of the array it indexes");
    }
    const bool fromLiteral = instruction.result.has_value() &&
            literalPlaces.contains(instruction.result->id);
    if (site.unauthorized && !fromAddressor && !fromLiteral) {
        report(
                VerificationErrorId::callMismatch,
                functionId, block.id, location,
                "an integer address requires runtime authorization");
    }
}

// A typed slot or function address turned into an Int for the native ABI.
void FunctionVerifier::verifyPointerToInteger(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto* source = site.shapeMatches && operands[0] != nullptr &&
            types.contains(operands[0]->type) ? types.at(operands[0]->type) : nullptr;
    const auto* result = instruction.result.has_value() && types.contains(instruction.result->type)
            ? types.at(instruction.result->type) : nullptr;
    if (source == nullptr || result == nullptr || result->kind != typing::TypeKind::integer ||
        instruction.result->category != ValueCategory::value ||
        (operands[0]->category != ValueCategory::address &&
         source->kind != typing::TypeKind::function)) {
        report(VerificationErrorId::typeMismatch, functionId, block.id, location,
               "a pointer conversion takes a typed slot or function address and yields an Int");
    }
    if (site.unauthorized) {
        report(VerificationErrorId::callMismatch, functionId, block.id, location,
               "a pointer conversion requires runtime authorization");
    }
    if (source != nullptr && operands[0]->category == ValueCategory::value &&
        source->kind == typing::TypeKind::function) {
        const auto definition = valueDefinitions.find(operands[0]->id);
        const auto definingBlock = definition == valueDefinitions.end() ? blocks.end()
                : blocks.find(definition->second.first);
        const auto* address = definingBlock != blocks.end() &&
                definition->second.second < definingBlock->second->instructions.size()
                ? &definingBlock->second->instructions[definition->second.second] : nullptr;
        const auto callee = address != nullptr && address->opcode == Opcode::functionAddress &&
                address->callee.has_value() ? functions.find(*address->callee) : functions.end();
        const bool matches = source->arguments.size() == 2 &&
                source->effects == std::vector<syntax::AccessEffect> {syntax::AccessEffect::consuming} &&
                types.contains(source->arguments[0]) &&
                types.at(source->arguments[0])->kind == typing::TypeKind::integer &&
                types.contains(source->arguments[1]) &&
                types.at(source->arguments[1])->kind == typing::TypeKind::voidType &&
                callee != functions.end() && callee->second->runtimeContext &&
                !callee->second->nativeBinding.has_value() && !callee->second->isExternal &&
                !callee->second->returnsValue;
        if (!matches) {
            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                   "a runtime callback address requires a named authorized Joyeer "
                   "function with one consuming Int parameter and a Void result");
        }
    }
}

} // namespace joyeer::ir::verifier
