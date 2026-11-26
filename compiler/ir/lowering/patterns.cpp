// `match`: a switch over the arms' patterns, then each arm with its bindings. A pattern that
// binds a noncopyable payload moves it from the scrutinee's storage. A live owned temporary
// can also give up its bound copyable payloads without cloning.

#include "internal.h"

namespace joyeer::lowering::detail {

// True when a pattern binds a noncopyable value, so the match must move it out of the
// scrutinee instead of copying it.
bool Builder::patternBindsMovablePayload(
        const syntax::PatternPtr& pattern, bool moveCopyable) const {
    if (pattern == nullptr) return false;
    if (pattern->kind == syntax::Kind::bindingPattern) {
        const auto type = typeOf(pattern);
        return type.has_value() &&
                (!copyableInBody(pattern, *type) || (moveCopyable && requiresDestroy(*type)));
    }
    if (pattern->kind != syntax::Kind::enumCasePattern) return false;
    const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
    for (const auto& argument : enumPattern->arguments) {
        if (patternBindsMovablePayload(argument->pattern, moveCopyable)) return true;
    }
    return false;
}

std::optional<ir::Value> Builder::lowerMatch(const syntax::MatchExprSyntax::Ptr& expression) {
    bool movesNoncopyable = false;
    for (const auto& arm : expression->arms) {
        movesNoncopyable = movesNoncopyable || patternBindsMovablePayload(arm->pattern, false);
    }
    auto operand = expression->scrutinee;
    bool consumesPlace = false;
    while (true) {
        if (operand->kind == syntax::Kind::parenthesizedExpr) {
            operand = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(operand)->expression;
        } else if (operand->kind == syntax::Kind::accessExpr) {
            const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(operand);
            consumesPlace |= access->marker != nullptr && access->marker->kind == kwConsume;
            operand = access->operand;
        } else {
            break;
        }
    }
    const bool addressable = isAddressable(operand);
    bool moveCopyable = false;
    std::optional<ir::Value> scrutineeAddress;
    std::optional<ir::Value> scrutinee;
    if (movesNoncopyable && addressable) {
        if (!consumesPlace) {
            report(
                    DiagnosticId::ownershipViolation,
                    expression->scrutinee->span,
                    "only 'consume' can move a noncopyable payload out of a place");
            return std::nullopt;
        }
        scrutineeAddress = lowerAddress(operand);
        if (!scrutineeAddress.has_value()) return std::nullopt;
    } else {
        scrutinee = lowerExpression(expression->scrutinee);
        if (!scrutinee.has_value()) return std::nullopt;
        // A non-addressable projection can still be borrowed. Check ownership before acquiring,
        // or spilling it for a move would introduce a clone of the whole scrutinee.
        if (!addressable && ownershipOf(*scrutinee) == ValueOwnership::owned &&
            liveOwnedTemporaries.contains(scrutinee->id)) {
            for (const auto& arm : expression->arms) {
                if (arm->pattern != nullptr && arm->pattern->kind == syntax::Kind::enumCasePattern &&
                    patternBindsMovablePayload(arm->pattern, true)) {
                    moveCopyable = true;
                    break;
                }
            }
        }
        if (movesNoncopyable || moveCopyable) {
            const auto owned = acquireOwned(*scrutinee, expression->scrutinee->span);
            if (!owned.has_value()) return std::nullopt;
            scrutineeAddress = emitValue(
                    ir::Opcode::stackAllocate,
                    owned->type,
                    ir::ValueCategory::address,
                    {},
                    expression->scrutinee->span);
            emitRawStore(*owned, *scrutineeAddress, expression->scrutinee->span);
            registerOwnedStorage(*scrutineeAddress);
        }
    }
    if (scrutineeAddress.has_value()) {
        scrutinee = emitValue(
                ir::Opcode::load,
                scrutineeAddress->type,
                ir::ValueCategory::value,
                { scrutineeAddress->id },
                expression->scrutinee->span);
    }
    const auto resultType = typeOf(expression);
    if (!scrutinee.has_value() || !resultType.has_value()) return std::nullopt;

    // A discarded value needs no result: each branch's value is discarded in its own block.
    const auto producesResult = *resultType != model->types().voidType() &&
            *resultType != model->types().neverType() &&
            !model->isValueDiscarded(expression);
    const auto resultAddress = producesResult
            ? std::optional<ir::Value>(emitValue(
                    ir::Opcode::stackAllocate,
                    *resultType,
                    ir::ValueCategory::address,
                    {},
                    expression->span))
            : std::optional<ir::Value>();

    // A wildcard, binding or unit arm matches every value, so no arm after it can run: the dispatch
    // ends with it, as the IR verifier requires of a top-level wildcard, and the arms after it, which
    // type checking has already checked, are not lowered.
    size_t liveArms = expression->arms.size();
    for (size_t index = 0; index < expression->arms.size(); ++index) {
        if (catchesEveryValue(expression->arms[index]->pattern)) {
            liveArms = index + 1;
            break;
        }
    }

    std::vector<ir::BlockId> armBlocks;
    for (size_t index = 0; index < liveArms; ++index) {
        armBlocks.push_back(createBlock("match.arm." + std::to_string(index)));
    }
    const auto mergeBlock = createBlock("match.merge");

    auto dispatch = makeInstruction(ir::Opcode::switchPattern, expression->span);
    dispatch.operands = { scrutinee->id };
    for (size_t index = 0; index < liveArms; ++index) {
        const auto pattern = lowerPattern(expression->arms[index]->pattern);
        if (!pattern.has_value()) return std::nullopt;
        dispatch.switchCases.push_back(ir::SwitchCase { *pattern, armBlocks[index] });
    }
    emit(std::move(dispatch));

    bool hasMergePredecessor = false;
    for (size_t index = 0; index < liveArms; ++index) {
        const auto& arm = expression->arms[index];
        switchToBlock(armBlocks[index]);
        DebugScopeGuard armDebugScope(
                currentDebugScope,
                debugScopeForNode(arm, currentFunctionId));
        pushScope();
        bindPattern(arm->pattern, *scrutinee, scrutineeAddress, moveCopyable);
        const auto value = lowerExpression(arm->body);
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            continue;
        }
        if (resultAddress.has_value()) {
            if (value.has_value()) {
                emitStore(*value, *resultAddress, arm->body->span);
            } else {
                report(
                        DiagnosticId::missingType,
                        arm->body->span,
                        "value-producing match arm did not lower a value");
            }
        }
        cleanupAndPopScope(arm->span);
        emitBranch(mergeBlock, arm->span);
        hasMergePredecessor = true;
    }

    switchToBlock(mergeBlock);
    if (!hasMergePredecessor) {
        emit(makeInstruction(ir::Opcode::unreachable, expression->span));
        return std::nullopt;
    }
    if (!resultAddress.has_value()) return emitUnit(expression->span);
    registerOwnedStorage(*resultAddress);
    // A noncopyable result moves out of its slot; loading it would copy it.
    return emitValue(
            copyableInBody(expression, resultAddress->type) ? ir::Opcode::load : ir::Opcode::take,
            resultAddress->type,
            ir::ValueCategory::value,
            { resultAddress->id },
            expression->span);
}

// The top-level patterns that lowerPattern() makes a wildcard.
bool Builder::catchesEveryValue(const syntax::PatternPtr& pattern) {
    return pattern != nullptr &&
            (pattern->kind == syntax::Kind::wildcardPattern || pattern->kind == syntax::Kind::unitPattern ||
             pattern->kind == syntax::Kind::bindingPattern);
}

std::optional<ir::Pattern> Builder::lowerPattern(const syntax::PatternPtr& pattern) {
    if (pattern == nullptr) return std::nullopt;
    const auto type = typeOf(pattern);
    if (!type.has_value()) {
        report(DiagnosticId::missingType, pattern->span, "pattern has no type");
        return std::nullopt;
    }

    ir::Pattern result;
    result.type = *type;
    switch (pattern->kind) {
        case syntax::Kind::wildcardPattern:
        case syntax::Kind::unitPattern:
        case syntax::Kind::bindingPattern:
            result.kind = ir::PatternKind::wildcard;
            break;
        case syntax::Kind::literalPattern: {
            const auto literal = std::static_pointer_cast<syntax::LiteralPatternSyntax>(pattern);
            if (literal->literal == nullptr) return std::nullopt;
            switch (literal->literal->kind) {
                case decimalLiteral:
                    result.kind = typing::isFloating(model->types().type(*type)->kind)
                            ? ir::PatternKind::floatingLiteral : ir::PatternKind::integerLiteral;
                    result.integerValue = literal->literal->intValue;
                    if (result.kind == ir::PatternKind::floatingLiteral) {
                        result.text = literal->literal->rawValue;
                    }
                    break;
                case floatLiteral:
                    result.kind = ir::PatternKind::floatingLiteral;
                    result.text = literal->literal->rawValue;
                    break;
                case booleanLiteral:
                    result.kind = ir::PatternKind::booleanLiteral;
                    result.integerValue = literal->literal->rawValue == Literals::TRUE ? 1 : 0;
                    break;
                case stringLiteral:
                    result.kind = ir::PatternKind::stringLiteral;
                    result.text = literal->literal->rawValue;
                    break;
                case byteLiteral:
                    result.kind = ir::PatternKind::byteLiteral;
                    result.integerValue = literal->literal->intValue;
                    break;
                case nilLiteral: {
                    const auto none = findRoleCase(*type, ir::EnumerationRole::optional, true);
                    if (!none.has_value()) return std::nullopt;
                    result.kind = ir::PatternKind::enumCase;
                    result.symbol = *none;
                    break;
                }
                default:
                    report(
                            DiagnosticId::unsupportedSyntax,
                            pattern->span,
                            "literal pattern is not supported by IR lowering");
                    return std::nullopt;
            }
            break;
        }
        case syntax::Kind::enumCasePattern: {
            const auto enumPattern =
                    std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
            const auto caseSymbol = model->referencedSymbol(enumPattern);
            if (!caseSymbol.has_value()) {
                report(
                        DiagnosticId::missingSymbol,
                        pattern->span,
                        "enum pattern has no resolved case symbol");
                return std::nullopt;
            }
            result.kind = ir::PatternKind::enumCase;
            result.symbol = *caseSymbol;
            for (const auto& argument : enumPattern->arguments) {
                const auto payload = lowerPattern(argument->pattern);
                if (!payload.has_value()) return std::nullopt;
                result.payloads.push_back(*payload);
            }
            break;
        }
        default:
            report(
                    DiagnosticId::unsupportedSyntax,
                    pattern->span,
                    "pattern kind is not supported by IR lowering");
            return std::nullopt;
    }
    return result;
}

// A binding owns its value from here to the end of the scope that holds it.
void Builder::storeBinding(
        const syntax::PatternPtr& pattern,
        semantic::SymbolId symbol,
        ir::Value owned) {
    const auto address = emitValue(
            ir::Opcode::stackAllocate,
            owned.type,
            ir::ValueCategory::address,
            {},
            pattern->span,
            symbol);
    const auto debugVariable = ensureDebugVariable(symbol, currentFunctionId);
    emitRawStore(owned, address, pattern->span, symbol, false, debugVariable);
    slots[symbol] = address;
    registerOwnedStorage(address);
}

// Takes the bound payloads that cannot be copied, or need not be copied for an owned temporary.
void Builder::bindMovedPayloads(const syntax::PatternPtr& pattern, ir::Value address, bool moveCopyable) {
    if (pattern == nullptr || pattern->kind != syntax::Kind::enumCasePattern) return;
    const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
    const auto caseSymbol = model->referencedSymbol(enumPattern);
    if (!caseSymbol.has_value()) return;
    for (size_t index = 0; index < enumPattern->arguments.size(); ++index) {
        const auto& payloadPattern = enumPattern->arguments[index]->pattern;
        if (payloadPattern == nullptr ||
            (payloadPattern->kind != syntax::Kind::bindingPattern &&
             payloadPattern->kind != syntax::Kind::enumCasePattern)) {
            continue;
        }
        const auto payloadType = typeOf(payloadPattern);
        if (!payloadType.has_value()) continue;
        auto instruction = makeInstruction(ir::Opcode::payloadAddress, payloadPattern->span);
        instruction.result = makeValue(*payloadType, ir::ValueCategory::address);
        instruction.operands = { address.id };
        instruction.symbol = *caseSymbol;
        instruction.integerValue = static_cast<int64_t>(index);
        const auto payloadAddress = *instruction.result;
        emit(std::move(instruction));
        if (payloadPattern->kind == syntax::Kind::bindingPattern) {
            const auto symbol = model->semanticModel()->declaredSymbol(payloadPattern);
            if (!symbol.has_value()) continue;
            if (!patternBindsMovablePayload(payloadPattern, moveCopyable)) {
                const auto loaded = emitValue(
                        ir::Opcode::load,
                        *payloadType,
                        ir::ValueCategory::value,
                        { payloadAddress.id },
                        payloadPattern->span);
                bindPattern(payloadPattern, loaded);
                continue;
            }
            const auto taken = emitValue(
                    ir::Opcode::take,
                    *payloadType,
                    ir::ValueCategory::value,
                    { payloadAddress.id },
                    payloadPattern->span);
            const auto owned = acquireOwned(taken, payloadPattern->span);
            if (owned.has_value()) storeBinding(payloadPattern, *symbol, *owned);
        } else {
            bindMovedPayloads(payloadPattern, payloadAddress, moveCopyable);
        }
    }
}

void Builder::bindPattern(
        const syntax::PatternPtr& pattern,
        ir::Value value,
        std::optional<ir::Value> address,
        bool moveCopyable) {
    if (pattern == nullptr) return;
    if (address.has_value() && pattern->kind == syntax::Kind::enumCasePattern &&
        patternBindsMovablePayload(pattern, moveCopyable)) {
        bindMovedPayloads(pattern, *address, moveCopyable);
        return;
    }
    if (pattern->kind == syntax::Kind::bindingPattern) {
        const auto symbol = model->semanticModel()->declaredSymbol(pattern);
        if (!symbol.has_value()) return;
        if (!copyableInBody(pattern, value.type)) {
            // Type checking rejects this; a copy here would destroy the value twice.
            report(
                    DiagnosticId::ownershipViolation,
                    pattern->span,
                    "a pattern cannot copy a noncopyable value");
            return;
        }
        if (requiresDestroy(value.type)) {
            value = emitValue(
                    ir::Opcode::copyValue,
                    value.type,
                    ir::ValueCategory::value,
                    { value.id },
                    pattern->span);
        }
        const auto owned = acquireOwned(value, pattern->span);
        if (!owned.has_value()) return;
        const auto address = emitValue(
                ir::Opcode::stackAllocate,
                value.type,
                ir::ValueCategory::address,
                {},
                pattern->span,
                symbol);
                const auto debugVariable = ensureDebugVariable(*symbol, currentFunctionId);
                emitRawStore(
                    *owned,
                    address,
                    pattern->span,
                    symbol,
                    false,
                    debugVariable);
        slots[*symbol] = address;
                registerOwnedStorage(address);
        return;
    }
    if (pattern->kind != syntax::Kind::enumCasePattern) return;

    const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
    const auto caseSymbol = model->referencedSymbol(enumPattern);
    if (!caseSymbol.has_value()) return;
    for (size_t index = 0; index < enumPattern->arguments.size(); ++index) {
        const auto& payloadPattern = enumPattern->arguments[index]->pattern;
        if (payloadPattern->kind != syntax::Kind::bindingPattern &&
            payloadPattern->kind != syntax::Kind::enumCasePattern) {
            continue;
        }
        const auto payloadType = typeOf(payloadPattern);
        if (!payloadType.has_value()) continue;
        const auto payload = emitExtractPayload(
                *payloadType,
                value.id,
                *caseSymbol,
                static_cast<int64_t>(index),
                payloadPattern->span);
        bindPattern(payloadPattern, payload);
    }
}

} // namespace joyeer::lowering::detail
