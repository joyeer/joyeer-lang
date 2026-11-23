// The instruction builder: the current function and block, new values and blocks, instructions
// with their debug locations, branches, stores, and the conversions lowering may insert.

#include "internal.h"

namespace joyeer::lowering::detail {

void Builder::emitRawStore(
        ir::Value value,
        ir::Value address,
        SourceSpan span,
        std::optional<semantic::SymbolId> symbol,
        bool implicitCode,
        std::optional<ir::DebugVariableId> debugVariable) {
    auto instruction = makeInstruction(ir::Opcode::store, span, implicitCode);
    instruction.operands = { value.id, address.id };
    instruction.symbol = symbol;
    if (debugVariable.has_value()) {
        instruction.debugVariableBindings.push_back(ir::DebugVariableBinding {
            *debugVariable,
            address.id,
        });
    }
    emit(std::move(instruction));
}

ir::Value Builder::emitBoolLiteral(bool value, SourceSpan span) {
    auto constant = makeInstruction(ir::Opcode::literal, span);
    constant.integerValue = value ? 1 : 0;
    constant.result = makeValue(model->types().boolType(), ir::ValueCategory::value);
    const auto result = *constant.result;
    emit(std::move(constant));
    recordValue(result, ValueOwnership::trivial);
    return result;
}

ir::Value Builder::emitUnit(SourceSpan span) {
    return emitValue(
            ir::Opcode::literal,
            model->types().voidType(),
            ir::ValueCategory::value,
            {},
            span);
}

ir::Value Builder::emitValue(
        Operation operation,
        typing::TypeId type,
        ir::ValueCategory category,
        std::vector<ir::ValueId> operands,
        SourceSpan span,
        std::optional<semantic::SymbolId> symbol,
        bool implicitCode,
        std::optional<ir::DebugVariableId> debugVariable) {
    const auto opcode = operation.opcode;
    auto instruction = makeInstruction(operation, span, implicitCode);
    instruction.result = makeValue(type, category);
    instruction.operands = std::move(operands);
    instruction.symbol = symbol;
    const auto result = *instruction.result;
    if (debugVariable.has_value()) {
        instruction.debugVariableBindings.push_back(ir::DebugVariableBinding {
            *debugVariable,
            result.id,
        });
    }
    emit(std::move(instruction));
    if (category == ir::ValueCategory::value) {
        auto valueOwnership = ValueOwnership::borrowed;
        if (opcode == ir::Opcode::copyValue || opcode == ir::Opcode::take) {
            valueOwnership = ValueOwnership::owned;
        }
        recordValue(result, valueOwnership);
    }
    return result;
}

void Builder::emitStore(
        ir::Value value,
        ir::Value address,
        SourceSpan span,
        std::optional<semantic::SymbolId> symbol) {
    auto converted = coerce(value, address.type, span);
    if (!converted.has_value()) return;
    if (requiresDestroy(address.type)) {
        converted = acquireOwned(*converted, span);
        if (!converted.has_value()) return;
    }
    emitRawStore(*converted, address, span, symbol);
}

std::optional<ir::Value> Builder::coerce(
        ir::Value value,
        typing::TypeId destination,
        SourceSpan span) {
    if (value.type == destination || destination == model->types().anyType()) return value;
    const auto* destinationType = model->types().type(destination);
    if (model->types().isOptional(destinationType) &&
        destinationType->arguments.size() == 1 &&
        destinationType->arguments[0] == value.type) {
        const auto some = findRoleCase(destination, ir::EnumerationRole::optional, false);
        if (some.has_value()) {
            return emitEnumConstruction(destination, *some, { value }, span);
        }
    }
    report(
            DiagnosticId::missingType,
            span,
            "typed conversion from '" + model->types().displayName(value.type) +
                    "' to '" + model->types().displayName(destination) +
                    "' has no IR representation");
    return std::nullopt;
}

ir::BlockId Builder::createBlock(std::string name) {
    auto& blocks = currentFunction().blocks;
    const auto id = static_cast<ir::BlockId>(blocks.size());
    blocks.push_back(ir::BasicBlock { id, std::move(name), {} });
    return id;
}

void Builder::switchToBlock(ir::BlockId block) {
    assert(block < currentFunction().blocks.size());
    currentBlockId = block;
}

void Builder::emitBranch(ir::BlockId target, SourceSpan span) {
    auto instruction = makeInstruction(ir::Opcode::branch, span);
    instruction.targets = { target };
    emit(std::move(instruction));
}

void Builder::emitConditionalBranch(
        ir::Value condition,
        ir::BlockId trueBlock,
        ir::BlockId falseBlock,
        SourceSpan span) {
    auto instruction = makeInstruction(ir::Opcode::conditionalBranch, span);
    instruction.operands = { condition.id };
    instruction.targets = { trueBlock, falseBlock };
    emit(std::move(instruction));
}

ir::Instruction Builder::makeInstruction(
        Operation operation,
        SourceSpan span,
        bool implicitCode) const {
    auto instruction = ir::Instruction { operation.opcode };
    instruction.modifier = operation.modifier;
    instruction.span = span;
    if (module->sourceFile(0) != nullptr) {
        instruction.debugLocation = ir::DebugLocation {
            span,
            implicitCode,
            currentDebugScope,
        };
    }
    return instruction;
}

ir::Value Builder::makeValue(typing::TypeId type, ir::ValueCategory category) {
    return ir::Value { nextValue++, type, category };
}

void Builder::emit(ir::Instruction instruction) {
    currentBlock().instructions.push_back(std::move(instruction));
}

bool Builder::currentBlockTerminated() const {
    const auto& instructions = currentBlock().instructions;
    return !instructions.empty() && ir::isTerminator(instructions.back().opcode);
}

ir::Function& Builder::currentFunction() {
    assert(currentFunctionId < module->functions.size());
    return module->functions[currentFunctionId];
}

const ir::Function& Builder::currentFunction() const {
    assert(currentFunctionId < module->functions.size());
    return module->functions[currentFunctionId];
}

ir::BasicBlock& Builder::currentBlock() {
    auto& blocks = currentFunction().blocks;
    assert(currentBlockId < blocks.size());
    return blocks[currentBlockId];
}

const ir::BasicBlock& Builder::currentBlock() const {
    const auto& blocks = currentFunction().blocks;
    assert(currentBlockId < blocks.size());
    return blocks[currentBlockId];
}

} // namespace joyeer::lowering::detail
