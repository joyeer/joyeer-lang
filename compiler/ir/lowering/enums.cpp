// Enum values: case lookup in the collected definitions, construction with payloads, payload
// extraction, and `?` propagation of Optional and Result (decision 0024).

#include "internal.h"

#include <algorithm>

namespace joyeer::lowering::detail {

std::optional<ir::Value> Builder::lowerContextualCase(
        const syntax::ContextualCaseExprSyntax::Ptr& expression) {
    const auto caseSymbol = model->referencedSymbol(expression);
    const auto type = typeOf(expression);
    if (!caseSymbol.has_value() || !type.has_value()) {
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "contextual enum case has no typed target");
        return std::nullopt;
    }
    return lowerEnumArguments(
            *type, *caseSymbol, expression->arguments, expression->span);
}

// The case a role binds (decision 0024): present/success is the first role case, absent/failure
// the second. Never found by spelling.
std::optional<semantic::SymbolId> Builder::findRoleCase(
        typing::TypeId type,
        ir::EnumerationRole role,
        bool second) const {
    const auto enumeration = std::find_if(
            module->enumerations.begin(),
            module->enumerations.end(),
            [type](const auto& candidate) { return candidate.type == type; });
    if (enumeration == module->enumerations.end() || enumeration->role != role) return std::nullopt;
    const auto index = second ? enumeration->secondRoleCase : enumeration->firstRoleCase;
    if (index >= enumeration->cases.size()) return std::nullopt;
    return enumeration->cases[index].symbol;
}

std::optional<semantic::SymbolId> Builder::findEnumCase(
        typing::TypeId type,
        const std::string& name) const {
    const auto enumeration = std::find_if(
            module->enumerations.begin(),
            module->enumerations.end(),
            [type](const auto& candidate) { return candidate.type == type; });
    if (enumeration == module->enumerations.end()) return std::nullopt;
    const auto enumCase = std::find_if(
            enumeration->cases.begin(),
            enumeration->cases.end(),
            [&name](const auto& candidate) { return candidate.name == name; });
    return enumCase == enumeration->cases.end()
            ? std::nullopt
            : std::optional<semantic::SymbolId>(enumCase->symbol);
}

const ir::EnumCaseDefinition* Builder::enumCaseDefinition(
        typing::TypeId type,
        semantic::SymbolId caseSymbol) const {
    const auto enumeration = std::find_if(
            module->enumerations.begin(),
            module->enumerations.end(),
            [type](const auto& candidate) { return candidate.type == type; });
    if (enumeration == module->enumerations.end()) return nullptr;
    const auto found = std::find_if(
            enumeration->cases.begin(),
            enumeration->cases.end(),
            [caseSymbol](const auto& candidate) { return candidate.symbol == caseSymbol; });
    return found == enumeration->cases.end() ? nullptr : &*found;
}

std::optional<ir::Value> Builder::emitEnumConstruction(
        typing::TypeId type,
        semantic::SymbolId caseSymbol,
        const std::vector<ir::Value>& payloads,
        SourceSpan span) {
    const auto* enumCase = enumCaseDefinition(type, caseSymbol);
    if (enumCase == nullptr || enumCase->payloadTypes.size() != payloads.size()) {
        report(
                DiagnosticId::missingSymbol,
                span,
                "enum construction has no matching concrete case definition");
        return std::nullopt;
    }
    auto instruction = makeInstruction(ir::Opcode::constructEnum, span);
    instruction.result = makeValue(type, ir::ValueCategory::value);
    instruction.symbol = caseSymbol;
    for (size_t index = 0; index < payloads.size(); ++index) {
        auto converted = coerce(payloads[index], enumCase->payloadTypes[index], span);
        if (!converted.has_value()) return std::nullopt;
        if (requiresDestroy(enumCase->payloadTypes[index])) {
            converted = acquireOwned(*converted, span);
            if (!converted.has_value()) return std::nullopt;
        }
        instruction.operands.push_back(converted->id);
    }
    const auto result = *instruction.result;
    emit(std::move(instruction));
    recordValue(result, ValueOwnership::owned);
    return result;
}

std::optional<ir::Value> Builder::lowerEnumArguments(
        typing::TypeId type,
        semantic::SymbolId caseSymbol,
        const std::vector<syntax::CallArgumentSyntax::Ptr>& arguments,
        SourceSpan span) {
    const auto* definition = enumCaseDefinition(type, caseSymbol);
    if (definition == nullptr || definition->payloadTypes.size() != arguments.size()) {
        report(
                DiagnosticId::missingSymbol,
                span,
                "enum construction has no matching concrete case definition");
        return std::nullopt;
    }
    std::vector<ConstructionOperand> pending;
    // Evaluate every argument before taking a reserved linear place.
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto payload = prepareConstructionOperand(
                arguments[index]->value, definition->payloadTypes[index],
                movesArgument(*arguments[index]));
        if (!payload.has_value() || currentBlockTerminated()) return std::nullopt;
        pending.push_back(*payload);
    }
    std::vector<ir::Value> payloads;
    for (size_t index = 0; index < pending.size(); ++index) {
        const auto payload = commitConstructionOperand(pending[index], definition->payloadTypes[index]);
        if (!payload.has_value()) return std::nullopt;
        payloads.push_back(*payload);
    }
    return emitEnumConstruction(type, caseSymbol, payloads, span);
}

ir::Value Builder::emitExtractPayload(
        typing::TypeId payloadType,
        ir::ValueId operandId,
        semantic::SymbolId caseSymbol,
        int64_t payloadIndex,
        SourceSpan span) {
    auto instruction = makeInstruction(ir::Opcode::extractPayload, span);
    instruction.result = makeValue(payloadType, ir::ValueCategory::value);
    instruction.operands = { operandId };
    instruction.symbol = caseSymbol;
    instruction.integerValue = payloadIndex;
    const auto result = *instruction.result;
    emit(std::move(instruction));
    return result;
}

std::optional<ir::Value> Builder::lowerPropagation(
        const syntax::PropagateExprSyntax::Ptr& expression) {
    auto operand = lowerExpression(expression->operand);
    const auto resultType = typeOf(expression);
    if (!operand.has_value() || !resultType.has_value()) return std::nullopt;
    const auto* operandType = model->types().type(operand->type);
    const bool isResult = model->types().isResult(operandType);
    const auto role = isResult ? ir::EnumerationRole::result : ir::EnumerationRole::optional;
    const auto successCase = findRoleCase(operand->type, role, false);
    const auto failureCase = findRoleCase(operand->type, role, true);
    const auto returnedFailureCase = findRoleCase(currentFunction().resultType, role, true);
    if (operandType == nullptr || !successCase.has_value() ||
        !failureCase.has_value() || !returnedFailureCase.has_value()) {
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "propagation requires matching concrete enum cases");
        return std::nullopt;
    }

    operand = acquireOwned(*operand, expression->operand->span);
    if (!operand.has_value()) return std::nullopt;
    const bool neverSuccess = *resultType == model->types().neverType();
    const bool neverFailure = isResult &&
            operandType->arguments[1] == model->types().neverType();
    const bool hasPayload = *resultType != model->types().voidType() && !neverSuccess;
    const auto resultAddress = hasPayload
            ? std::optional<ir::Value>(emitValue(
                    ir::Opcode::stackAllocate,
                    *resultType,
                    ir::ValueCategory::address,
                    {},
                    expression->span))
            : std::nullopt;
    const auto successBlock = createBlock("propagate.success");
    const auto failureBlock = createBlock("propagate.failure");
    const auto mergeBlock = neverSuccess
            ? std::nullopt
            : std::optional<ir::BlockId>(createBlock("propagate.merge"));
    auto dispatch = makeInstruction(ir::Opcode::switchPattern, expression->span);
    dispatch.operands = { operand->id };
    for (const auto [caseSymbol, target] :
         { std::pair { *successCase, successBlock },
           std::pair { *failureCase, failureBlock } }) {
        const auto* definition = enumCaseDefinition(operand->type, caseSymbol);
        if (definition == nullptr) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "propagation case has no concrete definition");
            return std::nullopt;
        }
        ir::Pattern pattern;
        pattern.kind = ir::PatternKind::enumCase;
        pattern.type = operand->type;
        pattern.symbol = caseSymbol;
        for (const auto payloadType : definition->payloadTypes) {
            ir::Pattern wildcard;
            wildcard.kind = ir::PatternKind::wildcard;
            wildcard.type = payloadType;
            pattern.payloads.push_back(std::move(wildcard));
        }
        dispatch.switchCases.push_back(ir::SwitchCase { pattern, target });
    }
    emit(std::move(dispatch));

    switchToBlock(failureBlock);
    if (neverFailure) {
        emit(makeInstruction(ir::Opcode::unreachable, expression->span));
    } else {
        std::vector<ir::Value> failurePayloads;
        if (isResult) {
            const auto error = emitExtractPayload(
                    operandType->arguments[1],
                    operand->id,
                    *failureCase,
                    0,
                    expression->span);
            if (requiresDestroy(error.type)) recordValue(error, ValueOwnership::owned);
            failurePayloads.push_back(error);
        }
        const auto failure = emitEnumConstruction(
                currentFunction().resultType,
                *returnedFailureCase,
                failurePayloads,
                expression->span);
        if (!failure.has_value()) return std::nullopt;
        emitReturnValue(*failure, expression->span, expression->span);
    }

    switchToBlock(successBlock);
    if (neverSuccess) {
        emit(makeInstruction(ir::Opcode::unreachable, expression->span));
        return std::nullopt;
    }
    const auto success = emitExtractPayload(
            *resultType,
            operand->id,
            *successCase,
            0,
            expression->span);
    if (requiresDestroy(success.type)) recordValue(success, ValueOwnership::owned);
    if (resultAddress.has_value()) {
        const auto owned = acquireOwned(success, expression->span);
        if (!owned.has_value()) return std::nullopt;
        emitRawStore(*owned, *resultAddress, expression->span);
    }
    emitBranch(*mergeBlock, expression->span);

    switchToBlock(*mergeBlock);
    if (!resultAddress.has_value()) return emitUnit(expression->span);
    if (requiresDestroy(*resultType)) {
        registerOwnedStorage(*resultAddress);
        return emitValue(
                ir::Opcode::take,
                *resultType,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }
    return emitValue(
            ir::Opcode::load,
            *resultType,
            ir::ValueCategory::value,
            { resultAddress->id },
            expression->span);
}

} // namespace joyeer::lowering::detail
