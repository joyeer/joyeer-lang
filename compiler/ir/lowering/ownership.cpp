// Ownership and destruction. Every value lowering produces is recorded as trivial, borrowed or
// owned. Owned temporaries and owning storage are registered as cleanups on the innermost scope.
// Acquiring an owned temporary removes it from `liveOwnedTemporaries`, so that cleanup skips it:
// its new owner destroys it. Leaving a scope, a loop iteration or the function destroys whatever
// is still live, newest first.

#include "internal.h"

namespace joyeer::lowering::detail {

bool Builder::requiresDestroy(typing::TypeId type) const {
    return destruction->requiresDestruction(type);
}

void Builder::pushScope() {
    scopes.push_back(ScopeFrame {});
}

void Builder::registerOwnedStorage(ir::Value address) {
    assert(!scopes.empty());
    if (requiresDestroy(address.type)) {
        scopes.back().cleanups.push_back(Cleanup { CleanupKind::storage, address });
    }
}

void Builder::recordValue(ir::Value value, ValueOwnership valueOwnership) {
    if (!requiresDestroy(value.type)) valueOwnership = ValueOwnership::trivial;
    ownership[value.id] = valueOwnership;
    if (valueOwnership == ValueOwnership::owned) {
        assert(!scopes.empty());
        liveOwnedTemporaries.insert(value.id);
        scopes.back().cleanups.push_back(Cleanup { CleanupKind::temporary, value });
    }
}

Builder::ValueOwnership Builder::ownershipOf(ir::Value value) const {
    if (!requiresDestroy(value.type)) return ValueOwnership::trivial;
    const auto found = ownership.find(value.id);
    return found == ownership.end() ? ValueOwnership::borrowed : found->second;
}

void Builder::adoptInParentScope(ir::Value value) {
    if (!requiresDestroy(value.type)) return;
    assert(!scopes.empty());
    liveOwnedTemporaries.insert(value.id);
    ownership[value.id] = ValueOwnership::owned;
    scopes.back().cleanups.push_back(Cleanup { CleanupKind::temporary, value });
}

std::optional<ir::Value> Builder::prepareOwned(ir::Value value, SourceSpan span) {
    if (!requiresDestroy(value.type)) return value;
    if (ownershipOf(value) == ValueOwnership::owned) {
        if (!liveOwnedTemporaries.contains(value.id)) {
            report(
                    DiagnosticId::ownershipViolation,
                    span,
                    "owned value is used after its ownership was transferred");
            return std::nullopt;
        }
        return value;
    }
    // In an instance, a copy of a value whose type is noncopyable in the generic body is a
    // type-checking gap, whatever the concrete type is (decision 0021).
    if (currentNoncopyableSpans != nullptr && currentNoncopyableSpans->contains(spanKey(span))) {
        report(
                DiagnosticId::ownershipViolation,
                span,
                "a value whose type is noncopyable in the generic body would be copied in '" +
                        currentInstance->name + "'");
        return std::nullopt;
    }
    // Type checking rejects every source-level copy of a noncopyable value, so reaching
    // here means a lowering path moves where it should not copy.
    if (!model->isCopyable(value.type)) {
        report(
                DiagnosticId::ownershipViolation,
                span,
                "a noncopyable value would be copied");
        return std::nullopt;
    }
    return emitValue(
            ir::Opcode::copyValue,
            value.type,
            ir::ValueCategory::value,
            { value.id },
            span);
}

std::optional<ir::Value> Builder::acquireOwned(ir::Value value, SourceSpan span) {
    const auto prepared = prepareOwned(value, span);
    if (prepared.has_value()) liveOwnedTemporaries.erase(prepared->id);
    return prepared;
}

// Moves the value out of the place `expression` names, leaving the place empty.
std::optional<ir::Value> Builder::lowerTake(const syntax::ExprPtr& expression) {
    const auto address = lowerAddress(expression);
    if (!address.has_value()) return std::nullopt;
    return takeFromPlace(*address, expression->span);
}

ir::Value Builder::takeFromPlace(ir::Value address, SourceSpan span) {
    return emitValue(
            ir::Opcode::take,
            address.type,
            ir::ValueCategory::value,
            { address.id },
            span);
}

bool Builder::movesArgument(const syntax::CallArgumentSyntax& argument) {
    return argument.accessMarker != nullptr && argument.accessMarker->kind == kwConsume;
}

std::optional<ir::Value> Builder::prepareOperand(
        const syntax::ExprPtr& expression,
        typing::TypeId destination,
        bool moved) {
    auto value = moved && isAddressable(expression)
            ? lowerTake(expression)
            : lowerExpression(expression);
    if (!value.has_value()) return std::nullopt;
    value = coerce(*value, destination, expression->span);
    if (!value.has_value()) return std::nullopt;
    return prepareOwned(*value, expression->span);
}

std::optional<Builder::ConstructionOperand> Builder::prepareConstructionOperand(
        const syntax::ExprPtr& expression, typing::TypeId destination, bool marked) {
    if (const auto source = model->linearConstructionMoveSource(expression, marked)) {
        const auto address = lowerAddress(source);
        if (!address.has_value()) return std::nullopt;
        return ConstructionOperand { *address, source->span, true };
    }
    const auto value = prepareOperand(expression, destination, marked);
    if (!value.has_value()) return std::nullopt;
    return ConstructionOperand { *value, expression->span, false };
}

std::optional<ir::Value> Builder::commitConstructionOperand(
        ConstructionOperand operand, typing::TypeId destination) {
    if (!operand.deferredTake) return operand.value;
    return coerce(takeFromPlace(operand.value, operand.span), destination, operand.span);
}

bool Builder::transferOperands(
        ir::Instruction& instruction,
        const std::vector<ir::Value>& operands,
        SourceSpan span) {
    for (const auto& operand : operands) {
        const auto value = acquireOwned(operand, span);
        if (!value.has_value()) return false;
        instruction.operands.push_back(value->id);
    }
    return true;
}

void Builder::emitDestroyStorage(ir::Value address, SourceSpan span) {
    if (!requiresDestroy(address.type)) return;
    auto instruction = makeInstruction(ir::Opcode::destroy, span, true);
    instruction.operands = { address.id };
    emit(std::move(instruction));
}

void Builder::emitDestroyTemporary(ir::Value value, SourceSpan span) {
    const auto address = emitValue(
            ir::Opcode::stackAllocate,
            value.type,
            ir::ValueCategory::address,
            {},
        span,
        std::nullopt,
        true);
    emitRawStore(value, address, span, std::nullopt, true);
    emitDestroyStorage(address, span);
}

void Builder::emitScopeCleanup(const ScopeFrame& scope, SourceSpan span, bool mutateState) {
    for (auto cleanup = scope.cleanups.rbegin(); cleanup != scope.cleanups.rend(); ++cleanup) {
        if (cleanup->kind == CleanupKind::storage) {
            emitDestroyStorage(cleanup->value, span);
            continue;
        }
        if (!liveOwnedTemporaries.contains(cleanup->value.id)) continue;
        emitDestroyTemporary(cleanup->value, span);
        if (mutateState) liveOwnedTemporaries.erase(cleanup->value.id);
    }
}

void Builder::cleanupAndPopScope(SourceSpan span) {
    assert(!scopes.empty());
    emitScopeCleanup(scopes.back(), span, true);
    scopes.pop_back();
}

void Builder::discardScopeAfterTerminator() {
    assert(!scopes.empty());
    scopes.pop_back();
}

void Builder::emitAllScopeCleanupForReturn(SourceSpan span) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        emitScopeCleanup(*scope, span, false);
    }
}

} // namespace joyeer::lowering::detail
