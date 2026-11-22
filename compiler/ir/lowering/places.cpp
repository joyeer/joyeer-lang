// Places: assignment, compound assignment, and the addresses of names, fields and elements.
// Spec 4.4.3 fixes the order: every index operand, then the right side, and only then the
// element address, so the right side cannot leave that address dangling. `prepareAddress`
// splits a place into the part evaluated now and the address computed later.

#include "internal.h"

namespace joyeer::lowering::detail {

std::optional<ir::Value> Builder::lowerAssignment(
        const syntax::AssignmentExprSyntax::Ptr& expression) {
    if (expression->op != nullptr && expression->op->kind != equal) {
        return lowerCompoundAssignment(expression);
    }
    auto target = expression->target;
    while (target != nullptr &&
           (target->kind == syntax::Kind::accessExpr ||
            target->kind == syntax::Kind::parenthesizedExpr)) {
        target = target->kind == syntax::Kind::accessExpr
                ? std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand
                : std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression;
    }
    if (target != nullptr && target->kind == syntax::Kind::subscriptExpr) {
        const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
        const auto baseType = typeOf(subscript->base);
        if (baseType.has_value() && model->types().isDictionary(*baseType)) {
            return lowerDictionarySet(expression, subscript, *baseType);
        }
    }
    const auto deferredAddress = prepareAddress(expression->target,
            !targetIndexesCannotDisturb(expression->target) || !cannotDisturb(expression->value));
    if (!deferredAddress.has_value() || currentBlockTerminated()) return std::nullopt;
    auto value = lowerExpression(expression->value);
    if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto address = (*deferredAddress)();
    if (address.has_value() && value.has_value()) {
        value = coerce(*value, address->type, expression->value->span);
        if (!value.has_value()) return std::nullopt;
        if (requiresDestroy(address->type)) {
            value = acquireOwned(*value, expression->value->span);
            if (!value.has_value()) return std::nullopt;
            emitDestroyStorage(*address, expression->target->span);
        }
        emitRawStore(*value, *address, expression->span);
        return emitUnit(expression->span);
    }
    return std::nullopt;
}

// `target op= value`: any index is evaluated once, then the right side,
// and only then is the element address computed, the old value read,
// combined with the right side, and stored back.
std::optional<ir::Value> Builder::lowerCompoundAssignment(
        const syntax::AssignmentExprSyntax::Ptr& expression) {
    const auto deferredAddress = prepareAddress(expression->target,
            !targetIndexesCannotDisturb(expression->target) || !cannotDisturb(expression->value));
    if (!deferredAddress.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto right = lowerExpression(expression->value);
    if (!right.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto address = (*deferredAddress)();
    if (!address.has_value()) return std::nullopt;
    const auto old = emitValue(
            ir::Opcode::load,
            address->type,
            ir::ValueCategory::value,
            { address->id },
            expression->target->span);
    std::optional<ir::Value> combined;
    if (isString(address->type)) {
        combined = emitStringOperator(compoundBaseOperator(expression->op->kind), old, *right, address->type,
                                      expression->span);
    } else if (const auto opcode = binaryOpcode(compoundBaseOperator(expression->op->kind))) {
        combined = emitValue(*opcode, address->type, ir::ValueCategory::value, { old.id, right->id },
                             expression->span);
    } else {
        report(DiagnosticId::unsupportedSyntax, expression->span,
                "compound assignment operator is not supported by primitive IR lowering");
    }
    if (!combined.has_value()) return std::nullopt;
    auto result = *combined;
    if (requiresDestroy(address->type)) {
        const auto owned = acquireOwned(result, expression->value->span);
        if (!owned.has_value()) return std::nullopt;
        result = *owned;
        emitDestroyStorage(*address, expression->target->span);
    }
    emitRawStore(result, *address, expression->span);
    return emitUnit(expression->span);
}

// Whether evaluating `expression` provably runs no code that could change storage read earlier in the same
// expression: a name, a literal, or a field or element read of such operands. Anything else (a call, `&`,
// `consume`, an operator, a block) may disturb, so operands evaluated before it are owned and element addresses
// are taken after it (spec 4.4.3's order).
bool Builder::cannotDisturb(const syntax::ExprPtr& expression) {
    if (expression == nullptr) return true;
    switch (expression->kind) {
        case syntax::Kind::nameExpr:
        case syntax::Kind::literalExpr:
        case syntax::Kind::unitExpr:
            return true;
        case syntax::Kind::parenthesizedExpr:
            return cannotDisturb(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        case syntax::Kind::memberExpr:
            return cannotDisturb(std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base);
        case syntax::Kind::subscriptExpr: {
            const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
            return cannotDisturb(subscript->base) && cannotDisturb(subscript->index);
        }
        default:
            return false;
    }
}

// Whether every index operand along an assignment target's path cannot disturb (see `cannotDisturb`).
bool Builder::targetIndexesCannotDisturb(const syntax::ExprPtr& target) {
    if (target == nullptr) return true;
    switch (target->kind) {
        case syntax::Kind::accessExpr:
            return targetIndexesCannotDisturb(std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand);
        case syntax::Kind::parenthesizedExpr:
            return targetIndexesCannotDisturb(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression);
        case syntax::Kind::memberExpr:
            return targetIndexesCannotDisturb(std::static_pointer_cast<syntax::MemberExprSyntax>(target)->base);
        case syntax::Kind::subscriptExpr: {
            const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
            return targetIndexesCannotDisturb(subscript->base) && cannotDisturb(subscript->index);
        }
        default:
            return true;
    }
}

std::optional<ir::Value> Builder::lowerAddress(const syntax::ExprPtr& expression) {
    if (expression == nullptr) return std::nullopt;
    if (expression->kind == syntax::Kind::accessExpr) {
        return lowerAddress(
                std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
    }
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return lowerAddress(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::nameExpr) {
        const auto symbol = model->referencedSymbol(expression);
        if (symbol.has_value() && slots.contains(*symbol)) return slots.at(*symbol);
    }
    if (expression->kind == syntax::Kind::memberExpr) {
        const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
        const auto symbol = model->referencedSymbol(member);
        const auto* semanticSymbol = symbol.has_value()
                ? model->semanticModel()->symbol(*symbol)
                : nullptr;
        if (semanticSymbol != nullptr &&
            semanticSymbol->kind == semantic::SymbolKind::structureField) {
            const auto base = lowerAddress(member->base);
            const auto type = typeOf(member);
            if (!base.has_value() || !type.has_value()) return std::nullopt;
            return emitValue(
                    ir::Opcode::fieldAddress,
                    *type,
                    ir::ValueCategory::address,
                    { base->id },
                    member->span,
                    symbol);
        }
    }
    if (expression->kind == syntax::Kind::subscriptExpr) {
        const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(
                expression);
        if (!cannotDisturb(subscript->index)) {
            // The index may reallocate the base's storage or reassign a key: evaluate every index first (spec
            // 4.4.3's order), then address.
            const auto deferred = prepareAddress(expression, true);
            if (!deferred.has_value() || currentBlockTerminated()) return std::nullopt;
            return (*deferred)();
        }
        const auto base = lowerAddress(subscript->base);
        const auto index = lowerExpression(subscript->index);
        const auto type = typeOf(subscript);
        if (!base.has_value() || !index.has_value() || !type.has_value()) {
            return std::nullopt;
        }
        if (const auto baseType = typeOf(subscript->base); baseType.has_value() && isRoleSubscriptBase(*baseType)) {
            return emitRoleElementAddress(*baseType, *base, *index, *type, subscript->span);
        }
        // Only a collection role's element has an address; type checking rejects writing, passing `inout` or
        // consuming a String byte.
        report(DiagnosticId::unsupportedSyntax, subscript->span,
               "only an array or dictionary element has an address");
        return std::nullopt;
    }
    report(
            DiagnosticId::unsupportedSyntax,
            expression->span,
            "assignment target is not supported by primitive IR lowering");
    return std::nullopt;
}

std::optional<Builder::DeferredAddress> Builder::prepareAddress(const syntax::ExprPtr& expression, bool ownKeys) {
    if (expression == nullptr) return std::nullopt;
    if (expression->kind == syntax::Kind::accessExpr) {
        return prepareAddress(
                std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand, ownKeys);
    }
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return prepareAddress(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression, ownKeys);
    }
    if (expression->kind == syntax::Kind::subscriptExpr) {
        const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(
                expression);
        auto base = prepareAddress(subscript->base, ownKeys);
        if (!base.has_value()) return std::nullopt;
        auto index = lowerExpression(subscript->index);
        if (index.has_value() && ownKeys) index = prepareOwned(*index, subscript->index->span);
        const auto type = typeOf(subscript);
        if (!index.has_value() || !type.has_value()) return std::nullopt;
        const auto roleBase = typeOf(subscript->base);
        if (!roleBase.has_value() || !isRoleSubscriptBase(*roleBase)) {
            // Only a collection role's element has an address; type checking rejects writing, passing `inout`
            // or consuming a String byte.
            report(DiagnosticId::unsupportedSyntax, subscript->span,
                   "only an array or dictionary element has an address");
            return std::nullopt;
        }
        return DeferredAddress([this, base = std::move(*base), index = *index, type = *type, roleBase = *roleBase,
                                span = subscript->span]() -> std::optional<ir::Value> {
            const auto baseAddress = base();
            if (!baseAddress.has_value()) return std::nullopt;
            return emitRoleElementAddress(roleBase, *baseAddress, index, type, span);
        });
    }
    if (expression->kind == syntax::Kind::memberExpr) {
        const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
        const auto symbol = model->referencedSymbol(member);
        const auto* semanticSymbol = symbol.has_value()
                ? model->semanticModel()->symbol(*symbol)
                : nullptr;
        const auto type = typeOf(member);
        if (semanticSymbol != nullptr &&
            semanticSymbol->kind == semantic::SymbolKind::structureField &&
            type.has_value()) {
            auto base = prepareAddress(member->base, ownKeys);
            if (!base.has_value()) return std::nullopt;
            return DeferredAddress([this, base = std::move(*base), type = *type, symbol,
                                    span = member->span]() -> std::optional<ir::Value> {
                const auto baseAddress = base();
                if (!baseAddress.has_value()) return std::nullopt;
                return emitValue(
                        ir::Opcode::fieldAddress,
                        type,
                        ir::ValueCategory::address,
                        { baseAddress->id },
                        span,
                        symbol);
            });
        }
    }
    return DeferredAddress([this, expression] { return lowerAddress(expression); });
}

bool Builder::isAddressable(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    switch (expression->kind) {
        case syntax::Kind::nameExpr: {
            const auto symbol = model->referencedSymbol(expression);
            return symbol.has_value() && slots.contains(*symbol);
        }
        case syntax::Kind::accessExpr:
            return isAddressable(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        case syntax::Kind::parenthesizedExpr:
            return isAddressable(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        case syntax::Kind::memberExpr:
            return isAddressable(
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base);
        case syntax::Kind::subscriptExpr:
            return isAddressable(
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression)->base);
        default:
            return false;
    }
}

} // namespace joyeer::lowering::detail
