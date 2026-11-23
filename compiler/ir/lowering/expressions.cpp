// The expression dispatcher and the expressions that need no place: literals, names, function
// values, prefix and binary operators (String operators call the collections prelude's String role
// functions), `as?`, short-circuit `&&` and `||`, and member reads.

#include "internal.h"

#include <bit>

namespace joyeer::lowering::detail {

std::optional<ir::Value> Builder::lowerExpression(const syntax::ExprPtr& expression) {
    if (expression == nullptr) return std::nullopt;
    switch (expression->kind) {
        case syntax::Kind::unitExpr:
            return emitUnit(expression->span);
        case syntax::Kind::literalExpr:
            return lowerLiteral(std::static_pointer_cast<syntax::LiteralExprSyntax>(expression));
        case syntax::Kind::nameExpr:
            return lowerName(expression);
        case syntax::Kind::parenthesizedExpr:
            return lowerExpression(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        case syntax::Kind::prefixExpr:
            return lowerPrefix(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression));
        case syntax::Kind::accessExpr: {
            const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
            // `consume x` in an owned context moves the place out (decision 0015).
            if (access->marker != nullptr && access->marker->kind == kwConsume &&
                isAddressable(access->operand)) {
                return lowerTake(access->operand);
            }
            return lowerExpression(access->operand);
        }
        case syntax::Kind::propagateExpr:
            return lowerPropagation(
                    std::static_pointer_cast<syntax::PropagateExprSyntax>(expression));
        case syntax::Kind::binaryExpr:
            return lowerBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
        case syntax::Kind::assignmentExpr:
            return lowerAssignment(
                    std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression));
        case syntax::Kind::memberExpr:
            return lowerMember(std::static_pointer_cast<syntax::MemberExprSyntax>(expression));
        case syntax::Kind::callExpr:
            return lowerCall(std::static_pointer_cast<syntax::CallExprSyntax>(expression));
        case syntax::Kind::subscriptExpr:
            return lowerSubscript(std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression));
        case syntax::Kind::arrayExpr:
            return lowerArray(std::static_pointer_cast<syntax::ArrayExprSyntax>(expression));
        case syntax::Kind::dictionaryExpr:
            return lowerDictionary(
                    std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression));
        case syntax::Kind::contextualCaseExpr:
            return lowerContextualCase(
                    std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression));
        case syntax::Kind::returnExpr:
            lowerReturn(std::static_pointer_cast<syntax::ReturnExprSyntax>(expression));
            return std::nullopt;
        case syntax::Kind::blockExpr:
            return lowerBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression));
        case syntax::Kind::ifExpr:
            return lowerIf(std::static_pointer_cast<syntax::IfExprSyntax>(expression));
        case syntax::Kind::matchExpr:
            return lowerMatch(std::static_pointer_cast<syntax::MatchExprSyntax>(expression));
        default:
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    std::string("IR lowering is not implemented for ") +
                            syntax::kindName(expression->kind));
            return std::nullopt;
    }
}

std::optional<ir::Value> Builder::lowerLiteral(
        const syntax::LiteralExprSyntax::Ptr& expression) {
    const auto type = typeOf(expression);
    if (!type.has_value() || expression->literal == nullptr) {
        report(DiagnosticId::missingType, expression->span, "literal has no type");
        return std::nullopt;
    }

    ir::Opcode opcode;
    switch (expression->literal->kind) {
        case decimalLiteral:
            opcode = typing::isFloating(model->types().type(*type)->kind)
                    ? ir::Opcode::floatingConstant : ir::Opcode::literal;
            break;
        case floatLiteral: opcode = ir::Opcode::floatingConstant; break;
        case booleanLiteral: opcode = ir::Opcode::literal; break;
        case stringLiteral: opcode = ir::Opcode::stringConstant; break;
        case byteLiteral: opcode = ir::Opcode::literal; break;
        case nilLiteral: {
            const auto none = findRoleCase(*type, ir::EnumerationRole::optional, true);
            if (!none.has_value()) {
                report(
                        DiagnosticId::missingSymbol,
                        expression->span,
                        "nil type has no Optional.None case in IR");
                return std::nullopt;
            }
            return emitEnumConstruction(*type, *none, {}, expression->span);
        }
        default:
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "literal kind is not supported by primitive IR lowering");
            return std::nullopt;
    }

    auto instruction = makeInstruction(opcode, expression->span);
    instruction.result = makeValue(*type, ir::ValueCategory::value);
    if (expression->literal->kind == booleanLiteral) {
        instruction.integerValue = expression->literal->rawValue == Literals::TRUE ? 1 : 0;
    } else if (opcode == ir::Opcode::floatingConstant || expression->literal->kind == stringLiteral) {
        instruction.text = expression->literal->rawValue;
    } else {
        instruction.integerValue = expression->literal->intValue;
    }
    const auto result = *instruction.result;
    emit(std::move(instruction));
    recordValue(
        result,
        expression->literal->kind == stringLiteral
            ? ValueOwnership::borrowed
            : ValueOwnership::trivial);
    return result;
}

// A function named where it is not called (spec 5.6.1).
std::optional<ir::Value> Builder::lowerFunctionAddress(semantic::SymbolId symbol, const syntax::NodePtr& expression) {
    const auto type = typeOf(expression);
    const auto function = demandFunction(symbol);
    if (!function.has_value() || !type.has_value()) {
        report(DiagnosticId::missingSymbol, expression->span,
               "function value has no IR function declaration");
        return std::nullopt;
    }
    auto instruction = makeInstruction(ir::Opcode::functionAddress, expression->span);
    instruction.callee = *function;
    instruction.result = makeValue(*type, ir::ValueCategory::value);
    const auto result = *instruction.result;
    emit(std::move(instruction));
    recordValue(result, ValueOwnership::trivial);
    return result;
}

bool Builder::namesFunctionValue(const syntax::NodePtr& expression, std::optional<semantic::SymbolId> symbol) const {
    if (!symbol.has_value()) return false;
    const auto* declaration = model->semanticModel()->symbol(*symbol);
    const auto type = typeOf(expression);
    const auto* record = type.has_value() ? model->types().type(*type) : nullptr;
    return declaration != nullptr && declaration->kind == semantic::SymbolKind::function &&
            record != nullptr && record->kind == typing::TypeKind::function;
}

std::optional<ir::Value> Builder::lowerName(const syntax::NodePtr& expression) {
    const auto symbol = model->referencedSymbol(expression);
    if (namesFunctionValue(expression, symbol)) return lowerFunctionAddress(*symbol, expression);
    if (symbol.has_value() && topLevelBindings.contains(*symbol)) {
        // A rejected declaration was already reported; do not add a second error.
        if (!constantDeclarations.contains(*symbol)) return std::nullopt;
        return lowerConstant(*symbol, expression->span);
    }
    if (symbol.has_value()) {
        if (const auto bound = boundParameters.find(*symbol); bound != boundParameters.end()) return bound->second;
    }
    if (!symbol.has_value() || !slots.contains(*symbol)) {
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "name does not refer to a lowered local storage slot");
        return std::nullopt;
    }
    const auto address = slots.at(*symbol);
    return emitValue(
            ir::Opcode::load,
            address.type,
            ir::ValueCategory::value,
            { address.id },
            expression->span,
            symbol);
}

std::optional<ir::Value> Builder::lowerPrefix(const syntax::PrefixExprSyntax::Ptr& expression) {
    const auto type = typeOf(expression);
    if (type.has_value() && expression->op != nullptr && expression->op->kind == minus &&
            typing::isInteger(model->types().type(*type)->kind) &&
            expression->operand->kind == syntax::Kind::literalExpr) {
        const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression->operand);
        if (literal->literal->kind == decimalLiteral) {
            auto constant = makeInstruction(ir::Opcode::literal, expression->span);
            constant.result = makeValue(*type, ir::ValueCategory::value);
            constant.integerValue = std::bit_cast<int64_t>(
                    uint64_t{0} - static_cast<uint64_t>(literal->literal->intValue));
            const auto result = *constant.result;
            emit(std::move(constant));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
    }
    const auto operand = lowerExpression(expression->operand);
    if (currentBlockTerminated()) return std::nullopt;
    if (!operand.has_value() || !type.has_value()) return std::nullopt;
    if (expression->castTarget != nullptr && expression->op->rawValue == "as?") {
        return lowerTryConversion(expression, *operand, *type);
    }
    if (expression->castTarget != nullptr) {
        if (operand->type == *type) return operand;
        return emitValue(
                expression->op->rawValue == "&as"
                        ? ir::ConversionMode::wrapping : ir::ConversionMode::checked,
                *type,
                ir::ValueCategory::value,
                { operand->id },
                expression->span);
    }
    if (expression->op != nullptr && expression->op->kind == bang) {
        auto constant = makeInstruction(ir::Opcode::literal, expression->op->span);
        constant.integerValue = 0;
        constant.result = makeValue(*type, ir::ValueCategory::value);
        const auto falseValue = *constant.result;
        emit(std::move(constant));
        return emitValue(
                ir::ComparePredicate::equal,
                *type,
                ir::ValueCategory::value,
                { operand->id, falseValue.id },
                expression->span);
    }
    if (expression->op != nullptr && expression->op->kind == tilde) {
        // ~x is x ^ all-ones; the constant is truncated to the operand width.
        auto ones = makeInstruction(ir::Opcode::literal, expression->op->span);
        const auto kind = model->types().type(*type)->kind;
        const auto bits = typing::scalarBitWidth(kind);
        ones.integerValue = typing::isSignedInteger(kind) || bits == 64
                ? -1 : static_cast<int64_t>((uint64_t{1} << bits) - 1);
        ones.result = makeValue(*type, ir::ValueCategory::value);
        const auto onesValue = *ones.result;
        emit(std::move(ones));
        return emitValue(
                ir::Opcode::bitXor,
                *type,
                ir::ValueCategory::value,
                { operand->id, onesValue.id },
                expression->span);
    }
    if (expression->op == nullptr || expression->op->kind != minus) {
        report(
                DiagnosticId::unsupportedSyntax,
                expression->span,
                "prefix operator is not supported by primitive IR lowering");
        return std::nullopt;
    }

    const bool floating = typing::isFloating(model->types().type(*type)->kind);
    auto zero = makeInstruction(floating ? ir::Opcode::floatingConstant : ir::Opcode::literal,
            expression->op->span);
    if (floating) zero.text = "-0.0";
    zero.result = makeValue(*type, ir::ValueCategory::value);
    const auto zeroValue = *zero.result;
    emit(std::move(zero));
    return emitValue(
            ir::Opcode::subtract,
            *type,
            ir::ValueCategory::value,
            { zeroValue.id, operand->id },
            expression->span);
}

std::optional<ir::Value> Builder::lowerBinary(const syntax::BinaryExprSyntax::Ptr& expression) {
    if (expression->op != nullptr &&
        (expression->op->kind == andAnd || expression->op->kind == orOr)) {
        return lowerLogical(expression);
    }
    const auto type = typeOf(expression);
    if (expression->op == nullptr || !type.has_value()) return std::nullopt;
    if (const auto nilOnLeft = model->presenceTest(expression)) {
        return lowerPresenceTest(*nilOnLeft ? expression->right : expression->left,
                                 expression->op->kind == equalEqual, expression->span);
    }
    if (expression->op->kind == equalEqual || expression->op->kind == notEqual) {
        // A conformer other than a builtin compares through its `Equatable` witness (decision 0031).
        const auto operandType = typeOf(expression->left);
        if (operandType.has_value() && !model->isBuiltinEquatable(*operandType)) {
            return lowerWitnessEquality(expression->left, expression->right, *operandType,
                                        expression->op->kind == equalEqual, *type, expression->span);
        }
    }
    return lowerBuiltinBinary(expression->op->kind, expression->left, expression->right, *type, expression->span);
}

// A binary operator on builtin operands. Where the checked body names a builtin type, the left operand is read
// as a value before the right is evaluated (spec 20.13.2). In a generic body the operands are a type parameter's
// values, so the left stays borrowed, as the checked body decided (decision 0021). `Void` equality is a constant
// once both operands are evaluated.
std::optional<ir::Value> Builder::lowerBuiltinBinary(
        TokenKind op, const syntax::ExprPtr& leftOperand, const syntax::ExprPtr& rightOperand,
        typing::TypeId type, SourceSpan span) {
    auto left = lowerExpression(leftOperand);
    if (!left.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto checked = model->typeOf(leftOperand);
    const bool equality = op == equalEqual || op == notEqual;
    if (!equality || !checked.has_value() || !model->types().mentionsTypeParameter(*checked)) {
        left = prepareOwned(*left, leftOperand->span);
        if (!left.has_value()) return std::nullopt;
    }
    const auto right = lowerExpression(rightOperand);
    if (!left.has_value() || !right.has_value() || currentBlockTerminated()) return std::nullopt;
    if ((op == equalEqual || op == notEqual) && left->type == model->types().voidType()) {
        return emitBoolLiteral(op == equalEqual, span);
    }
    if (isString(left->type)) {
        return emitStringOperator(op, *left, *right, type, span);
    }
    const auto opcode = binaryOpcode(op);
    if (!opcode.has_value()) {
        report(
                DiagnosticId::unsupportedSyntax,
                span,
                "binary operator is not supported by primitive IR lowering");
        return std::nullopt;
    }
    return emitValue(
            *opcode,
            type,
            ir::ValueCategory::value,
            { left->id, right->id },
            span);
}

bool Builder::isString(typing::TypeId type) const {
    const auto* record = model->types().type(type);
    return record != nullptr && record->kind == typing::TypeKind::string;
}

// A call to the collections prelude's function for a String role (decision 0028 stage 3). The operands are
// borrowed; a String or array result is owned.
std::optional<ir::Value> Builder::emitStringRole(
        semantic::LibraryRole role, const std::vector<ir::Value>& operands, typing::TypeId type,
        SourceSpan span) {
    const auto callee = stringRoleFunction(role);
    if (!callee.has_value()) {
        report(DiagnosticId::missingSymbol, span, "the String operation has no collections prelude function");
        return std::nullopt;
    }
    auto call = makeInstruction(ir::Opcode::call, span);
    call.callee = *callee;
    for (const auto& operand : operands) call.operands.push_back(operand.id);
    call.result = makeValue(type, ir::ValueCategory::value);
    const auto result = *call.result;
    emit(std::move(call));
    recordValue(result, requiresDestroy(type) ? ValueOwnership::owned : ValueOwnership::trivial);
    return result;
}

// `+`, `==`, `!=` and the orderings on Strings: `+` concatenates, `!=` is `compare eq` of the equality with
// false, and each ordering compares the -1, 0 or 1 order with 0.
std::optional<ir::Value> Builder::emitStringOperator(
        TokenKind token, ir::Value left, ir::Value right, typing::TypeId type, SourceSpan span) {
    if (token == plus) return emitStringRole(semantic::LibraryRole::stringConcat, { left, right }, type, span);
    const auto predicate = binaryOpcode(token);
    if (!predicate.has_value() || predicate->opcode != ir::Opcode::compare) {
        report(DiagnosticId::unsupportedSyntax, span, "operator is not supported on String values");
        return std::nullopt;
    }
    const auto relation = static_cast<ir::ComparePredicate>(predicate->modifier);
    const bool equality = relation == ir::ComparePredicate::equal || relation == ir::ComparePredicate::notEqual;
    const auto resultType = equality ? model->types().boolType() : model->types().intType();
    const auto computed = emitStringRole(
            equality ? semantic::LibraryRole::stringEqual : semantic::LibraryRole::stringOrder,
            { left, right }, resultType, span);
    if (!computed.has_value() || relation == ir::ComparePredicate::equal) return computed;
    auto constant = makeInstruction(ir::Opcode::literal, span);
    constant.integerValue = 0;
    constant.result = makeValue(resultType, ir::ValueCategory::value);
    const auto zero = *constant.result;
    emit(std::move(constant));
    return emitValue(equality ? ir::ComparePredicate::equal : relation, type, ir::ValueCategory::value,
                     { computed->id, zero.id }, span);
}

// `x as? T`: .Ok(converted) when the value fits in T, else .Err(.OutOfRange).
std::optional<ir::Value> Builder::lowerTryConversion(
        const syntax::PrefixExprSyntax::Ptr& expression,
        ir::Value operand,
        typing::TypeId resultType) {
    const auto* result = model->types().type(resultType);
    const auto okCase = findRoleCase(resultType, ir::EnumerationRole::result, false);
    const auto errCase = findRoleCase(resultType, ir::EnumerationRole::result, true);
    if (result == nullptr || result->arguments.size() != 2 ||
        !okCase.has_value() || !errCase.has_value()) {
        report(DiagnosticId::missingType, expression->span,
                "'as?' result is not a Result with Ok and Err cases");
        return std::nullopt;
    }
    const auto target = result->arguments[0];
    const auto errorType = result->arguments[1];
    const auto outOfRange = findEnumCase(errorType, "OutOfRange");
    if (!outOfRange.has_value()) {
        report(DiagnosticId::missingSymbol, expression->span,
                "ConversionError has no OutOfRange case in IR");
        return std::nullopt;
    }

    auto test = makeInstruction(ir::Opcode::integerFits, expression->span);
    test.result = makeValue(model->types().boolType(), ir::ValueCategory::value);
    test.operands = { operand.id };
    test.integerValue = static_cast<int64_t>(target);
    const auto fits = *test.result;
    emit(std::move(test));
    recordValue(fits, ValueOwnership::trivial);

    const auto slot = emitValue(
            ir::Opcode::stackAllocate, resultType, ir::ValueCategory::address,
            {}, expression->span, std::nullopt, true);
    const auto okBlock = createBlock("convert.ok");
    const auto errBlock = createBlock("convert.out_of_range");
    const auto mergeBlock = createBlock("convert.merge");
    emitConditionalBranch(fits, okBlock, errBlock, expression->span);

    switchToBlock(okBlock);
    const auto converted = emitValue(
            ir::ConversionMode::wrapping, target, ir::ValueCategory::value,
            { operand.id }, expression->span);
    const auto ok = emitEnumConstruction(resultType, *okCase, { converted }, expression->span);
    if (!ok.has_value()) return std::nullopt;
    emitRawStore(*ok, slot, expression->span, std::nullopt, true);
    emitBranch(mergeBlock, expression->span);

    switchToBlock(errBlock);
    const auto failure = emitEnumConstruction(errorType, *outOfRange, {}, expression->span);
    if (!failure.has_value()) return std::nullopt;
    const auto err = emitEnumConstruction(resultType, *errCase, { *failure }, expression->span);
    if (!err.has_value()) return std::nullopt;
    emitRawStore(*err, slot, expression->span, std::nullopt, true);
    emitBranch(mergeBlock, expression->span);

    switchToBlock(mergeBlock);
    return emitValue(
            ir::Opcode::load, resultType, ir::ValueCategory::value,
            { slot.id }, expression->span);
}

std::optional<ir::Value> Builder::lowerLogical(
        const syntax::BinaryExprSyntax::Ptr& expression) {
    const auto isAnd = expression->op->kind == andAnd;
    pushScope();
    const auto left = lowerExpression(expression->left);
    if (currentBlockTerminated()) {
        discardScopeAfterTerminator();
        return std::nullopt;
    }
    cleanupAndPopScope(expression->left->span);
    if (!left.has_value()) return std::nullopt;

    const auto resultAddress = emitValue(
            ir::Opcode::stackAllocate,
            model->types().boolType(),
            ir::ValueCategory::address,
            {},
            expression->span,
            std::nullopt,
            true);
    emitRawStore(*left, resultAddress, expression->left->span, std::nullopt, true);
    const auto rightBlock = createBlock(isAnd ? "and.rhs" : "or.rhs");
    const auto mergeBlock = createBlock(isAnd ? "and.merge" : "or.merge");
    emitConditionalBranch(
            *left,
            isAnd ? rightBlock : mergeBlock,
            isAnd ? mergeBlock : rightBlock,
            expression->left->span);

    switchToBlock(rightBlock);
    pushScope();
    const auto right = lowerExpression(expression->right);
    if (currentBlockTerminated()) {
        discardScopeAfterTerminator();
    } else if (right.has_value()) {
        emitRawStore(*right, resultAddress, expression->right->span, std::nullopt, true);
        cleanupAndPopScope(expression->right->span);
        emitBranch(mergeBlock, expression->span);
    } else {
        cleanupAndPopScope(expression->right->span);
        report(
                DiagnosticId::missingType,
                expression->right->span,
                "logical right operand did not lower a Boolean value");
        emit(makeInstruction(ir::Opcode::unreachable, expression->right->span, true));
    }

    switchToBlock(mergeBlock);
    return emitValue(
            ir::Opcode::load,
            model->types().boolType(),
            ir::ValueCategory::value,
            { resultAddress.id },
            expression->span);
}

std::optional<Operation> Builder::binaryOpcode(TokenKind token) const {
    switch (token) {
        case plus: return ir::Opcode::add;
        case minus: return ir::Opcode::subtract;
        case multiply: return ir::Opcode::multiply;
        case divide: return ir::Opcode::divide;
        case percentage: return ir::Opcode::remainder;
        case ampersand: return ir::Opcode::bitAnd;
        case verticalBar: return ir::Opcode::bitOr;
        case caret: return ir::Opcode::bitXor;
        case shiftLeft: return ir::Opcode::shiftLeft;
        case shiftRight: return ir::Opcode::shiftRight;
        case less: return ir::ComparePredicate::less;
        case lessEqual: return ir::ComparePredicate::lessEqual;
        case greater: return ir::ComparePredicate::greater;
        case greaterEqual: return ir::ComparePredicate::greaterEqual;
        case equalEqual: return ir::ComparePredicate::equal;
        case notEqual: return ir::ComparePredicate::notEqual;
        default: return std::nullopt;
    }
}

std::optional<ir::Value> Builder::lowerMember(const syntax::MemberExprSyntax::Ptr& expression) {
    const auto symbol = model->referencedSymbol(expression);
    const auto* semanticSymbol = symbol.has_value()
            ? model->semanticModel()->symbol(*symbol)
            : nullptr;
    const auto type = typeOf(expression);
    if (semanticSymbol == nullptr || !type.has_value()) {
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "member has no typed resolved symbol");
        return std::nullopt;
    }
    if (namesFunctionValue(expression, symbol)) return lowerFunctionAddress(*symbol, expression);
    if (topLevelBindings.contains(*symbol)) {
        if (!constantDeclarations.contains(*symbol)) return std::nullopt;
        return lowerConstant(*symbol, expression->span);
    }

    if (semanticSymbol->kind == semantic::SymbolKind::structureField) {
        if (isAddressable(expression)) {
            const auto address = lowerAddress(expression);
            if (!address.has_value()) return std::nullopt;
            return emitValue(
                    ir::Opcode::load,
                    *type,
                    ir::ValueCategory::value,
                    { address->id },
                    expression->span,
                    symbol);
        }
        const auto base = lowerExpression(expression->base);
        if (!base.has_value()) return std::nullopt;
        return emitValue(
                ir::Opcode::extractField,
                *type,
                ir::ValueCategory::value,
                { base->id },
                expression->span,
                symbol);
    }

    if (semanticSymbol->kind == semantic::SymbolKind::builtinMember &&
        semanticSymbol->name == "count") {
        const auto base = lowerExpression(expression->base);
        if (!base.has_value()) return std::nullopt;
        return emitStringRole(semantic::LibraryRole::stringCount, { *base }, *type, expression->span);
    }

    if (semanticSymbol->kind == semantic::SymbolKind::enumCase ||
        semanticSymbol->kind == semantic::SymbolKind::builtinEnumCase) {
        return emitEnumConstruction(*type, *symbol, {}, expression->span);
    }

    report(
            DiagnosticId::unsupportedSyntax,
            expression->span,
            "member kind is not supported by aggregate IR lowering");
    return std::nullopt;
}

} // namespace joyeer::lowering::detail
