// Blocks, local bindings and control flow: `if`, `while`, `break`, `continue` and `return`.
// A value-producing `if` stores each branch's value into one result slot and reads it in the
// merge block.

#include "internal.h"

namespace joyeer::lowering::detail {

std::optional<ir::Value> Builder::lowerBlock(const syntax::BlockExprSyntax::Ptr& block) {
    if (block == nullptr) return std::nullopt;
    DebugScopeGuard debugScope(
        currentDebugScope,
        debugScopeForNode(block, currentFunctionId));
    pushScope();
    std::optional<ir::Value> result;
    bool unitResult = block->items.empty();
    for (const auto& item : block->items) {
        if (currentBlockTerminated()) break;
        if (item->kind == syntax::Kind::bindingDecl) {
            lowerBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(item));
            result.reset();
            unitResult = true;
        } else if (item->kind == syntax::Kind::whileStmt) {
            lowerWhile(std::static_pointer_cast<syntax::WhileStmtSyntax>(item));
            result.reset();
            unitResult = true;
        } else if (item->kind == syntax::Kind::breakStmt ||
                   item->kind == syntax::Kind::continueStmt) {
            lowerLoopControl(item);
            result.reset();
        } else if (isExpressionKind(item->kind)) {
            result = lowerExpression(std::static_pointer_cast<syntax::ExprSyntax>(item));
            unitResult = false;
        }
    }
    if (currentBlockTerminated()) {
        discardScopeAfterTerminator();
        return std::nullopt;
    }
    // A discarded trailing value is never needed as an owned one (spec 4.1.1): a place there is
    // only read, never copied. An owned temporary still moves to the parent scope, which
    // destroys it after this block's locals.
    if (result.has_value() && requiresDestroy(result->type) &&
            ownershipOf(*result) != ValueOwnership::owned &&
            model->isValueDiscarded(block->items.back())) {
        result.reset();
        unitResult = true;
    }
    if (result.has_value() && requiresDestroy(result->type)) {
        const auto resultSpan = !block->items.empty()
                ? block->items.back()->span
                : block->span;
        result = acquireOwned(*result, resultSpan);
        if (!result.has_value()) {
            cleanupAndPopScope(block->span);
            return std::nullopt;
        }
    }
    if (unitResult) result = emitUnit(block->span);
    cleanupAndPopScope(block->span);
    if (result.has_value() && requiresDestroy(result->type)) {
        adoptInParentScope(*result);
    }
    return result;
}

void Builder::lowerBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
    const auto symbol = model->semanticModel()->declaredSymbol(declaration);
    const auto type = symbol.has_value()
            ? typeOf(*symbol)
            : std::optional<typing::TypeId>();
    if (!symbol.has_value() || !type.has_value()) {
        report(
                symbol.has_value() ? DiagnosticId::missingType : DiagnosticId::missingSymbol,
                declaration->span,
                "binding is missing a typed symbol");
        return;
    }
        const auto uninitializedDebugVariable = declaration->initializer == nullptr
            ? ensureDebugVariable(*symbol, currentFunctionId)
            : std::optional<ir::DebugVariableId>();
        const auto bindOnAllocation = declaration->initializer == nullptr &&
            !requiresDestroy(*type);
    const auto address = emitValue(
            ir::Opcode::stackAllocate,
            *type,
            ir::ValueCategory::address,
            {},
            declaration->span,
            symbol,
            false,
            bindOnAllocation
                ? uninitializedDebugVariable
                : std::optional<ir::DebugVariableId>());
    slots[*symbol] = address;
    if (declaration->initializer == nullptr) {
        if (requiresDestroy(*type)) {
            auto initialize = makeInstruction(
                    ir::Opcode::zeroInitialize,
                    declaration->span);
            initialize.operands = { address.id };
            if (uninitializedDebugVariable.has_value()) {
                initialize.debugVariableBindings.push_back(ir::DebugVariableBinding {
                    *uninitializedDebugVariable,
                    address.id,
                });
            }
            emit(std::move(initialize));
            registerOwnedStorage(address);
        }
        return;
    }
    auto initializer = lowerExpression(declaration->initializer);
    if (!initializer.has_value()) return;
    initializer = coerce(*initializer, *type, declaration->initializer->span);
    if (!initializer.has_value()) return;
    if (requiresDestroy(*type)) {
        initializer = acquireOwned(*initializer, declaration->initializer->span);
        if (!initializer.has_value()) return;
    }
    const auto debugVariable = ensureDebugVariable(*symbol, currentFunctionId);
    emitRawStore(
        *initializer,
        address,
        declaration->span,
        symbol,
        false,
        debugVariable);
    if (requiresDestroy(*type)) {
        registerOwnedStorage(address);
    }
}

std::optional<ir::Value> Builder::lowerIf(const syntax::IfExprSyntax::Ptr& expression) {
    const auto condition = lowerExpression(expression->condition);
    const auto resultType = typeOf(expression);
    if (!condition.has_value() || !resultType.has_value()) return std::nullopt;

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

    const auto thenBlock = createBlock("if.then");
    const auto elseBlock = createBlock("if.else");
    const auto mergeBlock = createBlock("if.merge");
    emitConditionalBranch(*condition, thenBlock, elseBlock, expression->condition->span);

    bool hasMergePredecessor = false;
    switchToBlock(thenBlock);
    pushScope();
    const auto thenValue = lowerBlock(expression->thenBranch);
    if (!currentBlockTerminated()) {
        if (resultAddress.has_value()) {
            if (thenValue.has_value()) {
                emitStore(*thenValue, *resultAddress, expression->thenBranch->span);
            } else {
                report(
                        DiagnosticId::missingType,
                        expression->thenBranch->span,
                        "value-producing if branch did not lower a value");
            }
        }
        cleanupAndPopScope(expression->thenBranch->span);
        emitBranch(mergeBlock, expression->thenBranch->span);
        hasMergePredecessor = true;
    } else {
        discardScopeAfterTerminator();
    }

    switchToBlock(elseBlock);
    pushScope();
    const auto elseValue = lowerExpression(expression->elseBranch);
    if (!currentBlockTerminated()) {
        if (resultAddress.has_value()) {
            if (elseValue.has_value()) {
                emitStore(*elseValue, *resultAddress, expression->elseBranch->span);
            } else {
                report(
                        DiagnosticId::missingType,
                        expression->span,
                        "value-producing else branch did not lower a value");
            }
        }
        cleanupAndPopScope(expression->span);
        emitBranch(mergeBlock, expression->span);
        hasMergePredecessor = true;
    } else {
        discardScopeAfterTerminator();
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

void Builder::lowerLoopControl(const syntax::NodePtr& statement) {
    if (loops.empty()) {
        report(DiagnosticId::unsupportedSyntax, statement->span,
                "loop control requires an enclosing loop");
        return;
    }
    auto& loop = loops.back();
    for (size_t depth = scopes.size(); depth > loop.scopeDepth; --depth) {
        emitScopeCleanup(scopes[depth - 1], statement->span, false);
    }
    const auto isBreak = statement->kind == syntax::Kind::breakStmt;
    if (isBreak) loop.hasBreak = true;
    emitBranch(isBreak ? loop.exit : loop.header, statement->span);
}

void Builder::lowerWhile(const syntax::WhileStmtSyntax::Ptr& statement) {
    const auto headerBlock = createBlock("while.header");
    const auto bodyBlock = createBlock("while.body");
    const auto exitBlock = createBlock("while.exit");
    loops.push_back({ headerBlock, exitBlock, scopes.size() });
    emitBranch(headerBlock, statement->span);

    switchToBlock(headerBlock);
    pushScope();
    const auto condition = lowerExpression(statement->condition);
    if (currentBlockTerminated()) {
        discardScopeAfterTerminator();
        switchToBlock(bodyBlock);
        emit(makeInstruction(ir::Opcode::unreachable, statement->body->span, true));
        switchToBlock(exitBlock);
        if (!loops.back().hasBreak) {
            emit(makeInstruction(ir::Opcode::unreachable, statement->span, true));
        }
        loops.pop_back();
        return;
    }
    if (condition.has_value()) {
        cleanupAndPopScope(statement->condition->span);
        emitConditionalBranch(
                *condition,
                bodyBlock,
                exitBlock,
                statement->condition->span);
    } else {
        cleanupAndPopScope(statement->condition->span);
        report(
                DiagnosticId::missingType,
                statement->condition->span,
                "while condition did not lower a Boolean value");
        emit(makeInstruction(ir::Opcode::unreachable, statement->condition->span));
    }

    switchToBlock(bodyBlock);
    pushScope();
    lowerBlock(statement->body);
    if (!currentBlockTerminated()) {
        cleanupAndPopScope(statement->body->span);
        emitBranch(headerBlock, statement->body->span);
    } else {
        discardScopeAfterTerminator();
    }

    switchToBlock(exitBlock);
    loops.pop_back();
}

void Builder::lowerReturn(const syntax::ReturnExprSyntax::Ptr& expression) {
    if (expression->value == nullptr) {
        emitAllScopeCleanupForReturn(expression->span);
        emit(makeInstruction(ir::Opcode::ret, expression->span));
        return;
    }
    auto value = lowerReturnedValue(expression->value);
    if (!value.has_value()) return;
    value = coerce(*value, currentFunction().resultType, expression->value->span);
    if (!value.has_value()) return;
    emitReturnValue(*value, expression->span, expression->value->span);
}

// Returning an owning local of a noncopyable type moves it out (decision 0015): a copy
// would leave the local to be destroyed, running the `deinit` of a value that escaped.
std::optional<ir::Value> Builder::lowerReturnedValue(const syntax::ExprPtr& expression) {
    const auto type = typeOf(expression);
    if (!type.has_value() || copyableInBody(expression, *type) || !isAddressable(expression)) {
        return lowerExpression(expression);
    }
    const auto address = lowerAddress(expression);
    if (!address.has_value()) return std::nullopt;
    return emitValue(
            ir::Opcode::take,
            address->type,
            ir::ValueCategory::value,
            { address->id },
            expression->span);
}

void Builder::emitReturnValue(ir::Value value, SourceSpan span, SourceSpan valueSpan) {
    if (requiresDestroy(value.type)) {
        const auto owned = acquireOwned(value, valueSpan);
        if (!owned.has_value()) return;
        value = *owned;
    }
    emitAllScopeCleanupForReturn(span);
    if (currentFunction().resultType == model->types().voidType()) {
        emit(makeInstruction(ir::Opcode::ret, span));
        return;
    }
    auto instruction = makeInstruction(ir::Opcode::ret, span);
    instruction.operands = { value.id };
    emit(std::move(instruction));
}

bool Builder::isExpressionKind(syntax::Kind kind) const {
    switch (kind) {
        case syntax::Kind::errorExpr:
        case syntax::Kind::nameExpr:
        case syntax::Kind::literalExpr:
        case syntax::Kind::unitExpr:
        case syntax::Kind::parenthesizedExpr:
        case syntax::Kind::prefixExpr:
        case syntax::Kind::accessExpr:
        case syntax::Kind::propagateExpr:
        case syntax::Kind::binaryExpr:
        case syntax::Kind::assignmentExpr:
        case syntax::Kind::memberExpr:
        case syntax::Kind::callExpr:
        case syntax::Kind::subscriptExpr:
        case syntax::Kind::arrayExpr:
        case syntax::Kind::dictionaryExpr:
        case syntax::Kind::contextualCaseExpr:
        case syntax::Kind::blockExpr:
        case syntax::Kind::ifExpr:
        case syntax::Kind::returnExpr:
        case syntax::Kind::matchExpr:
            return true;
        default:
            return false;
    }
}

} // namespace joyeer::lowering::detail
