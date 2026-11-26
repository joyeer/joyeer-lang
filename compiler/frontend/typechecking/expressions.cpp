#include "builder.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace joyeer::typing {

std::optional<TypeId> TypeCheckingBuilder::checkExpression(
        const syntax::ExprPtr& expression,
        std::optional<TypeId> expected,
        ReceiverMarker receiverMarker) {
    if (expression == nullptr) return std::nullopt;
    markDeferredReferenceHandled(expression);

    auto result = model->typeContext.errorType();
    switch (expression->kind) {
        case syntax::Kind::errorExpr:
            break;
        case syntax::Kind::unitExpr:
            result = model->typeContext.voidType();
            break;
        case syntax::Kind::literalExpr:
            result = checkLiteral(
                    std::static_pointer_cast<syntax::LiteralExprSyntax>(expression),
                    expected);
            break;
        case syntax::Kind::nameExpr:
            result = checkName(std::static_pointer_cast<syntax::NameExprSyntax>(expression));
            break;
        case syntax::Kind::parenthesizedExpr:
            result = checkExpression(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression,
                    expected).value_or(model->typeContext.errorType());
            break;
        case syntax::Kind::prefixExpr:
            result = checkPrefix(
                std::static_pointer_cast<syntax::PrefixExprSyntax>(expression), expected);
            break;
        case syntax::Kind::accessExpr: {
            const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
            const auto consumes = access->marker != nullptr &&
                    access->marker->kind == kwConsume;
            if (consumes && !isMemberCall(access->operand)) {
                if (!permittedMoves.contains(expression.get())) {
                    report(
                        TypeCheckingDiagnosticId::invalidConsumeArgument,
                        expression->span,
                        "'consume' is only valid on an argument, the receiver of a consuming "
                        "method call, or where a value is stored (a binding, an assignment, "
                        "or an aggregate element)");
                } else if (!isConsumable(access->operand) && !isSubscript(access->operand)) {
                    report(
                        TypeCheckingDiagnosticId::invalidConsumeArgument,
                        access->operand->span,
                        "consuming argument must be an owning local, consuming parameter, "
                        "or temporary");
                }
            }
            const auto diagnosticsBeforeOperand = diagnostics.size();
            result = checkExpression(
                    access->operand,
                    expected,
                    consumes ? ReceiverMarker::consume : ReceiverMarker::inout)
                    .value_or(model->typeContext.errorType());
            // A subscript is judged once its base is typed: a String byte reports only itself, since a
            // consumable base would not help.
            if (consumes && !isMemberCall(access->operand) && permittedMoves.contains(expression.get()) &&
                isSubscript(access->operand) &&
                !(diagnosticsBeforeOperand == diagnostics.size() && rejectConsumedStringByte(access->operand)) &&
                !isStringByte(access->operand) && !isConsumable(access->operand)) {
                report(
                    TypeCheckingDiagnosticId::invalidConsumeArgument,
                    access->operand->span,
                    "consuming argument must be an owning local, consuming parameter, "
                    "or temporary");
            }
            break;
        }
        case syntax::Kind::propagateExpr:
            result = checkPropagation(
                    std::static_pointer_cast<syntax::PropagateExprSyntax>(expression),
                    expected);
            break;
        case syntax::Kind::assignmentExpr: {
            const auto assignment =
                    std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
            const auto target = checkExpression(assignment->target);
            checkAssignmentAccess(assignment->target);
            if (insideDeinit && assignsWholeSelf(assignment->target)) {
                report(
                        TypeCheckingDiagnosticId::assignmentToSelfInDeinit,
                        assignment->target->span,
                        "a 'deinit' cannot replace 'self'",
                        "assign to the fields of 'self' instead; replacing the whole value "
                        "would run a destructor on a value that is being destroyed");
            }
            if (assignment->op != nullptr && assignment->op->kind != equal) {
                result = checkCompoundAssignment(assignment, target);
                break;
            }
            permitMove(assignment->value);
            const auto value = checkExpression(assignment->value, target);
            requireNoImplicitCopy(assignment->value, value);
            if (target.has_value() && value.has_value()) {
                requireAssignable(*value, *target, assignment->value->span);
            }
            result = model->typeContext.voidType();
            break;
        }
        case syntax::Kind::memberExpr:
            result = checkMember(
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression), expected);
            break;
        case syntax::Kind::callExpr:
            result = checkCall(
                    std::static_pointer_cast<syntax::CallExprSyntax>(expression),
                    receiverMarker,
                    expected);
            break;
        case syntax::Kind::arrayExpr:
            result = checkArray(
                    std::static_pointer_cast<syntax::ArrayExprSyntax>(expression),
                    expected);
            break;
        case syntax::Kind::dictionaryExpr:
            result = checkDictionary(
                    std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression),
                    expected);
            break;
        case syntax::Kind::contextualCaseExpr: {
            result = checkContextualCase(
                std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression),
                expected);
            break;
        }
        case syntax::Kind::blockExpr:
            result = checkBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression), expected,
                    model->isValueDiscarded(expression));
            break;
        case syntax::Kind::ifExpr: {
            const auto conditional = std::static_pointer_cast<syntax::IfExprSyntax>(expression);
            const auto condition = checkExpression(conditional->condition).value_or(
                model->typeContext.errorType());
            requireAssignable(
                condition,
                model->typeContext.boolType(),
                conditional->condition->span);
            // An `if` without `else` is `Void`, so its branch value is always discarded.
            const auto discarded = model->isValueDiscarded(expression);
            const auto thenType = checkBlock(conditional->thenBranch, expected,
                    discarded || conditional->elseBranch == nullptr);
            if (conditional->elseBranch == nullptr) {
                result = model->typeContext.voidType();
                break;
            }
            if (discarded) markValueDiscarded(conditional->elseBranch);
            const auto elseType = checkExpression(conditional->elseBranch, expected).value_or(
                model->typeContext.errorType());
            result = commonType(thenType, elseType, conditional->elseBranch->span);
            if (!discarded && producesBranchValue(result)) {
                requireNoImplicitCopy(conditional->thenBranch, thenType);
                requireNoImplicitCopy(conditional->elseBranch, elseType);
            }
            break;
        }
        case syntax::Kind::returnExpr: {
            const auto returned = std::static_pointer_cast<syntax::ReturnExprSyntax>(expression);
            const auto required = currentReturnType.value_or(model->typeContext.voidType());
            const auto actual = returned->value == nullptr
                ? model->typeContext.voidType()
                : checkExpression(returned->value, required).value_or(
                    model->typeContext.errorType());
            // Returning an owning local or a consuming parameter moves it out.
            if (returned->value != nullptr) {
                requireNoImplicitCopy(returned->value, actual, true);
            }
            requireAssignable(
                actual,
                required,
                returned->value == nullptr ? returned->span : returned->value->span);
            result = model->typeContext.neverType();
            break;
        }
        case syntax::Kind::matchExpr: {
            result = checkMatch(
                    std::static_pointer_cast<syntax::MatchExprSyntax>(expression),
                    expected);
            break;
        }
        case syntax::Kind::binaryExpr: {
            result = checkBinary(
                    std::static_pointer_cast<syntax::BinaryExprSyntax>(expression), expected);
            break;
        }
        case syntax::Kind::subscriptExpr:
            result = checkSubscript(
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression));
            break;
        default:
            break;
    }

    recordNodeType(expression, result);
    return result;
}

TypeId TypeCheckingBuilder::checkNumericLiteral(const Token::Ptr& literal, std::optional<TypeId> expected,
        bool negative) {
    auto result = literal->kind == floatLiteral
            ? model->typeContext.doubleType() : model->typeContext.intType();
    if (expected.has_value()) {
        auto* contextual = model->typeContext.type(*expected);
        if (model->typeContext.isOptional(contextual)) {
            expected = contextual->arguments.front();
            contextual = model->typeContext.type(*expected);
        }
        if (contextual != nullptr && isNumeric(contextual->kind)) result = *expected;
    }
    const auto kind = model->typeContext.type(result)->kind;
    bool valid = false;
    if (isFloating(kind)) {
        const auto& text = literal->rawValue;
        const auto fits = [&text](auto value) {
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                    value, std::chars_format::general);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                    std::isfinite(value);
        };
        valid = kind == TypeKind::float32 ? fits(float{}) : fits(double{});
    } else if (literal->kind == decimalLiteral && (!negative || isSignedInteger(kind))) {
        const auto bits = scalarBitWidth(kind);
        const auto maximum = isSignedInteger(kind)
                ? (uint64_t{1} << (bits - 1)) - (negative ? 0 : 1)
                : bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
        valid = static_cast<uint64_t>(literal->intValue) <= maximum;
    }
    if (!valid) {
        report(TypeCheckingDiagnosticId::invalidNumericLiteral, literal->span,
                "numeric literal is not representable as '" +
                        model->typeContext.displayName(result) + "'");
        return model->typeContext.errorType();
    }
    return result;
}

// When `type` is a user declaration that shares its name with a role-bound prelude enum, the
// help that tells an author the sugar did not bind to it (decision 0024). `only` limits it to shadows of one role.
std::optional<std::string> TypeCheckingBuilder::roleShadowHelp(
        std::optional<TypeId> type, std::optional<semantic::LibraryRole> only) const {
    const auto* record = type.has_value() ? model->typeContext.type(*type) : nullptr;
    if (record == nullptr || (record->kind != TypeKind::enumeration && record->kind != TypeKind::structure)) {
        return std::nullopt;
    }
    const auto* declaration = model->semanticModelValue->symbol(record->symbol);
    if (declaration == nullptr || declaration->libraryRole != semantic::LibraryRole::none) return std::nullopt;
    for (const auto role : {semantic::LibraryRole::optional, semantic::LibraryRole::result}) {
        if (only.has_value() && role != *only) continue;
        const auto roleSymbol = model->semanticModelValue->roleSymbol(role);
        const auto* roleDeclaration = roleSymbol.has_value() ? model->semanticModelValue->symbol(*roleSymbol) : nullptr;
        if (roleDeclaration != nullptr && roleDeclaration->name == declaration->name) {
            return "'" + declaration->name + "' here is the declaration in your program, not the prelude's; " +
                   (role == semantic::LibraryRole::optional
                            ? "'T?', 'nil' and postfix '?' work only on the prelude's Optional"
                            : "postfix '?' works only on the prelude's Result") +
                   ". Rename your type, or remove it to use the prelude's";
        }
    }
    return std::nullopt;
}

TypeId TypeCheckingBuilder::checkLiteral(
        const syntax::LiteralExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    if (expression->literal == nullptr) return model->typeContext.errorType();
    switch (expression->literal->kind) {
        case decimalLiteral:
        case floatLiteral: return checkNumericLiteral(expression->literal, expected);
        case booleanLiteral: return model->typeContext.boolType();
        case stringLiteral: return model->typeContext.stringType();
        case byteLiteral: return model->typeContext.uint8Type();
        case nilLiteral: {
            if (expected.has_value()) {
                const auto* expectedType = model->typeContext.type(*expected);
                if (model->typeContext.isOptional(expectedType)) {
                    return *expected;
                }
            }
            report(
                    TypeCheckingDiagnosticId::missingContextualType,
                    expression->span,
                    "'nil' requires an optional contextual type",
                    roleShadowHelp(expected));
            return model->typeContext.errorType();
        }
        default:
            return model->typeContext.errorType();
    }
}

TypeId TypeCheckingBuilder::checkPropagation(
        const syntax::PropagateExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    std::optional<TypeId> operandExpected;
    if (expected.has_value() &&
        *expected != model->typeContext.errorType() &&
        *expected != model->typeContext.anyType() &&
        currentReturnType.has_value()) {
        const auto* returnType = model->typeContext.type(*currentReturnType);
        if (model->typeContext.isResult(returnType)) {
            const auto errorType = returnType->arguments[1];
            operandExpected = model->typeContext.resultType(*expected, errorType);
        } else if (model->typeContext.isOptional(returnType)) {
            operandExpected = model->typeContext.optionalType(*expected);
        }
    }
    permitMove(expression->operand);
    const auto operand = checkExpression(expression->operand, operandExpected).value_or(
            model->typeContext.errorType());
    if (operand == model->typeContext.errorType()) return operand;

    const auto* operandType = model->typeContext.type(operand);
    if (operandType == nullptr ||
        (!model->typeContext.isResult(operandType) &&
         !model->typeContext.isOptional(operandType))) {
        report(
                TypeCheckingDiagnosticId::invalidPropagationOperand,
                expression->span,
                "postfix '?' requires a Result or Optional operand",
                roleShadowHelp(operand));
        return model->typeContext.errorType();
    }

    const auto* returnType = currentReturnType.has_value()
            ? model->typeContext.type(*currentReturnType)
            : nullptr;
    auto result = operandType->arguments[0];
    const bool resultOperand = model->typeContext.isResult(operandType);
    // Both sides must be instances of the same role (decision 0024), not merely the same enum.
    const bool sameRole = returnType != nullptr &&
            (resultOperand ? model->typeContext.isResult(returnType) : model->typeContext.isOptional(returnType));
    if (!sameRole) {
        report(
                TypeCheckingDiagnosticId::invalidPropagationContext,
                expression->span,
                resultOperand
                        ? "postfix '?' on Result requires a Result-returning function"
                        : "postfix '?' on Optional requires an Optional-returning function",
                roleShadowHelp(currentReturnType));
        result = model->typeContext.errorType();
    } else if (resultOperand &&
               operandType->arguments[1] != returnType->arguments[1]) {
        report(
                TypeCheckingDiagnosticId::mismatchedPropagationError,
                expression->span,
                "postfix '?' requires the same error type: operand has '" +
                        model->typeContext.displayName(operandType->arguments[1]) +
                        "' but function returns '" +
                        model->typeContext.displayName(returnType->arguments[1]) + "'");
        result = model->typeContext.errorType();
    }
    requireNoImplicitCopy(expression->operand, operand);
    return result;
}

TypeId TypeCheckingBuilder::checkCast(const syntax::PrefixExprSyntax::Ptr& expression) {
    const auto target = resolveType(expression->castTarget).value_or(
            model->typeContext.errorType());
    if (target == model->typeContext.errorType()) return target;
    if (!isInteger(model->typeContext.type(target)->kind)) {
        reportInvalidOperator(expression->op, { target }, expression->span);
        return model->typeContext.errorType();
    }
    const bool tryCast = expression->op->rawValue == "as?";
    // A literal operand takes the target type, so out-of-range literals
    // are compile-time errors rather than run-time traps. `as?` accepts any
    // value and reports a range failure in its Result, so literals stay Int.
    const auto operand = isNumericLiteral(expression->operand) && !tryCast
            ? checkExpression(expression->operand, target)
            : checkExpression(expression->operand);
    const auto operandType = operand.value_or(model->typeContext.errorType());
    if (operandType == model->typeContext.errorType()) return operandType;
    if (!isInteger(model->typeContext.type(operandType)->kind)) {
        reportInvalidOperator(expression->op, { operandType, target }, expression->span);
        return model->typeContext.errorType();
    }
    if (tryCast) {
        const auto symbol = model->typeContext.builtinSymbol("ConversionError");
        const auto failure = symbol.has_value()
                ? model->typeContext.typeForSymbol(*symbol) : std::nullopt;
        if (!failure.has_value()) return model->typeContext.errorType();
        return model->typeContext.resultType(target, *failure);
    }
    return target;
}

TypeId TypeCheckingBuilder::checkPrefix(const syntax::PrefixExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    if (expression->castTarget != nullptr) return checkCast(expression);
    if (expression->op != nullptr && expression->op->kind == minus &&
            expression->operand->kind == syntax::Kind::literalExpr) {
        const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression->operand);
        if (literal->literal->kind == decimalLiteral) {
            const auto result = checkNumericLiteral(literal->literal, expected, true);
            recordNodeType(literal, result);
            return result;
        }
    }
    const auto operand = checkExpression(expression->operand, expected).value_or(
            model->typeContext.errorType());
    if (operand == model->typeContext.errorType()) return operand;
    if (expression->op != nullptr && expression->op->kind == minus &&
        (isSignedInteger(model->typeContext.type(operand)->kind) ||
         isFloating(model->typeContext.type(operand)->kind))) {
        return operand;
    }
    if (expression->op != nullptr && expression->op->kind == tilde &&
        isInteger(model->typeContext.type(operand)->kind)) {
        return operand;
    }
    if (expression->op != nullptr && expression->op->kind == bang &&
        model->typeContext.isAssignable(operand, model->typeContext.boolType())) {
        return model->typeContext.boolType();
    }
    reportInvalidOperator(expression->op, { operand }, expression->span);
    return model->typeContext.errorType();
}

bool TypeCheckingBuilder::isNumericLiteral(const syntax::ExprPtr& expression) const {
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isNumericLiteral(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::prefixExpr) {
        return isNumericLiteral(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand);
    }
    if (expression->kind != syntax::Kind::literalExpr) return false;
    const auto kind = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression)->literal->kind;
    return kind == decimalLiteral || kind == floatLiteral;
}

// Whether `type` conforms to `Equatable` (spec 20.13.3). Without the prelude protocol only the builtin
// conformers do.
bool TypeCheckingBuilder::isEquatable(TypeId type) const {
    const auto equatable = model->equatableProtocol();
    return equatable.has_value() ? satisfies(type, *equatable) : model->isBuiltinEquatable(type);
}

// Result type of `left op right`, or nullopt if the operands are invalid.
std::optional<TypeId> TypeCheckingBuilder::binaryResult(TokenKind op, TypeId left, TypeId right) {
    switch (op) {
        case plus:
            if (isNumeric(model->typeContext.type(left)->kind) && left == right) return left;
            if (left == model->typeContext.stringType() && left == right) return left;
            break;
        case minus:
        case multiply:
        case divide:
            if (isNumeric(model->typeContext.type(left)->kind) && left == right) return left;
            break;
        case percentage:
        case ampersand:
        case verticalBar:
        case caret:
            if (isInteger(model->typeContext.type(left)->kind) && left == right) return left;
            break;
        case shiftLeft:
        case shiftRight:
            if (isInteger(model->typeContext.type(left)->kind) &&
                isInteger(model->typeContext.type(right)->kind)) {
                return left;
            }
            break;
        case less:
        case lessEqual:
        case greater:
        case greaterEqual:
            if (left == right &&
                (isNumeric(model->typeContext.type(left)->kind) ||
                 left == model->typeContext.stringType())) {
                return model->typeContext.boolType();
            }
            break;
        case equalEqual:
        case notEqual:
            // Equality is defined through `Equatable` (decision 0031, spec 20.13): both operands have one
            // type that conforms. Equality only borrows, so a noncopyable conformer compares too.
            if (left == right && isEquatable(left)) return model->typeContext.boolType();
            break;
        case andAnd:
        case orOr:
            if (model->typeContext.isAssignable(left, model->typeContext.boolType()) &&
                model->typeContext.isAssignable(right, model->typeContext.boolType())) {
                return model->typeContext.boolType();
            }
            break;
        default:
            break;
    }
    return std::nullopt;
}

TypeId TypeCheckingBuilder::checkBinary(const syntax::BinaryExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    TypeId left;
    TypeId right;
    const auto isShift = expression->op != nullptr &&
            (expression->op->kind == shiftLeft || expression->op->kind == shiftRight);
    if (isShift) {
        // The shifted value takes the contextual type; the count is independent.
        left = checkExpression(expression->left, expected).value_or(model->typeContext.errorType());
        right = checkExpression(expression->right).value_or(model->typeContext.errorType());
    } else if (expression->op != nullptr &&
               (expression->op->kind == equalEqual || expression->op->kind == notEqual)) {
        // `x == nil` and `x == .None` on an optional test presence, whatever the payload (spec 20.13.4).
        const bool leftAbsent = isAbsentLiteral(expression->left);
        if (leftAbsent != isAbsentLiteral(expression->right)) {
            const auto& value = leftAbsent ? expression->right : expression->left;
            const auto& absent = leftAbsent ? expression->left : expression->right;
            const auto valueType = checkExpression(value).value_or(model->typeContext.errorType());
            if (valueType != model->typeContext.errorType() && !model->typeContext.isOptional(valueType) &&
                !isContextualNone(absent)) {
                report(TypeCheckingDiagnosticId::invalidOperatorOperands, expression->span,
                       "'nil' cannot be compared with a value of the type '" +
                               model->typeContext.displayName(valueType) + "', which is not optional",
                       "only an optional can be absent; compare with a value of the type instead");
                return model->typeContext.errorType();
            }
            const auto absentType = checkExpression(absent, valueType).value_or(model->typeContext.errorType());
            if (valueType == model->typeContext.errorType() || absentType == model->typeContext.errorType()) {
                return model->typeContext.errorType();
            }
            if (model->typeContext.isOptional(valueType) && absentType == valueType) {
                if (const auto node = model->semanticModelValue->nodeId(expression)) {
                    model->presenceTests[*node] = leftAbsent;
                }
                return model->typeContext.boolType();
            }
            left = leftAbsent ? absentType : valueType;
            right = leftAbsent ? valueType : absentType;
        } else if (operandTypeStrength(expression->left) < operandTypeStrength(expression->right)) {
            right = checkExpression(expression->right).value_or(model->typeContext.errorType());
            left = checkExpression(expression->left, right).value_or(model->typeContext.errorType());
        } else {
            left = checkExpression(expression->left).value_or(model->typeContext.errorType());
            right = checkExpression(expression->right, left).value_or(model->typeContext.errorType());
        }
    } else if (isNumericLiteral(expression->left) && !isNumericLiteral(expression->right)) {
        right = checkExpression(expression->right, expected).value_or(model->typeContext.errorType());
        left = checkExpression(expression->left, right).value_or(model->typeContext.errorType());
    } else {
        left = checkExpression(expression->left, expected).value_or(model->typeContext.errorType());
        right = checkExpression(expression->right, left).value_or(model->typeContext.errorType());
    }
    if (left == model->typeContext.errorType() ||
        right == model->typeContext.errorType()) {
        return model->typeContext.errorType();
    }

    const auto op = expression->op == nullptr ? invalid : expression->op->kind;
    if (const auto result = binaryResult(op, left, right); result.has_value()) {
        if ((op == equalEqual || op == notEqual) && !model->isBuiltinEquatable(left)) {
            validateEqualityExclusivity(expression);
            requireEqualityWitness(expression, left);
        }
        return *result;
    }

    if ((op == equalEqual || op == notEqual) && left == right && !model->typeContext.isOptional(left) &&
        !model->typeContext.isResult(left)) {
        if (const auto path = nonEquatablePath(left)) {
            const auto* conformance = model->equatableConformance(left);
            const auto* leaf = model->typeContext.type(path->second);
            const auto help = leaf != nullptr && leaf->kind == TypeKind::typeParameter
                    ? std::optional<std::string>("add 'Equatable' to the constraint of '" +
                                                 model->typeContext.displayName(path->second) +
                                                 "' where it is declared (for an extension's parameter, in the "
                                                 "type's own declaration)")
                    : std::nullopt;
            report(TypeCheckingDiagnosticId::nonEquatableMember, expression->span,
                   "'" + model->typeContext.displayName(left) + "' does not conform to 'Equatable': its member '" +
                           path->first + "' has the type '" + model->typeContext.displayName(path->second) +
                           "', which does not conform",
                   help, std::nullopt,
                   { sourceNote(conformance->span, "the derived conformance is declared here") });
            return model->typeContext.errorType();
        }
    }
    reportInvalidOperator(expression->op, { left, right }, expression->span);
    return model->typeContext.errorType();
}

bool TypeCheckingBuilder::isContextualNone(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isContextualNone(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    return expression->kind == syntax::Kind::contextualCaseExpr;
}

// `nil` or `.None` with no payload, in parentheses or not (spec 20.13.4).
bool TypeCheckingBuilder::isAbsentLiteral(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isAbsentLiteral(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::literalExpr) {
        const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression)->literal;
        return literal != nullptr && literal->kind == nilLiteral;
    }
    if (expression->kind == syntax::Kind::contextualCaseExpr) {
        const auto member = std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
        return member->name != nullptr && member->name->rawValue == "None" && !member->hasPayloadClause;
    }
    return false;
}

// How much of its own type an operand of `==` has (spec 20.13.2): the weaker operand is typed by the stronger,
// whichever side it is on. 0: none (`nil`, an implicit member, an empty literal, a case of a generic enum, whose
// arguments may leave a parameter open); 1: an integer literal; 2: a floating-point literal, so `0 == 0.5`
// compares two `Double`s as `0.5 == 0` does; 3: a type of its own. An array literal is as strong as its weakest
// element.
int TypeCheckingBuilder::operandTypeStrength(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return 3;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return operandTypeStrength(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::prefixExpr &&
        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->castTarget != nullptr) {
        return 3;
    }
    if (isNumericLiteral(expression)) return isFloatLiteral(expression) ? 2 : 1;
    if (isOpenCaseReference(expression)) return 0;
    if (expression->kind == syntax::Kind::literalExpr) {
        const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression)->literal;
        return literal != nullptr && literal->kind == nilLiteral ? 0 : 3;
    }
    if (expression->kind == syntax::Kind::callExpr) {
        const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
        const auto referenced = model->referencedSymbol(call->callee);
        const auto* symbol = referenced.has_value() ? model->semanticModelValue->symbol(*referenced) : nullptr;
        // A case or initializer of a generic type: its arguments may leave a type parameter open.
        const bool genericCase = symbol != nullptr && symbol->kind == semantic::SymbolKind::enumCase &&
                symbol->containingSymbol.has_value() &&
                model->genericParameters(*symbol->containingSymbol) != nullptr;
        const bool genericInitializer = symbol != nullptr &&
                ((symbol->kind == semantic::SymbolKind::structure && model->genericParameters(symbol->id) != nullptr) ||
                 (symbol->kind == semantic::SymbolKind::synthesizedInitializer && symbol->containingSymbol.has_value() &&
                  model->genericParameters(*symbol->containingSymbol) != nullptr));
        return genericCase || genericInitializer ? 0 : 3;
    }
    if (expression->kind == syntax::Kind::arrayExpr) {
        int strength = 0;
        bool first = true;
        for (const auto& element : std::static_pointer_cast<syntax::ArrayExprSyntax>(expression)->elements) {
            const auto own = operandTypeStrength(element);
            strength = first ? own : std::min(strength, own);
            first = false;
        }
        return strength;
    }
    if (expression->kind == syntax::Kind::dictionaryExpr) {
        return std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression)->entries.empty() ? 0 : 3;
    }
    return 3;
}

bool TypeCheckingBuilder::isFloatLiteral(const syntax::ExprPtr& expression) const {
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isFloatLiteral(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::prefixExpr) {
        return isFloatLiteral(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand);
    }
    return expression->kind == syntax::Kind::literalExpr &&
            std::static_pointer_cast<syntax::LiteralExprSyntax>(expression)->literal->kind == floatLiteral;
}

// `a == b` reaches the witness as `a.equals(other: b)` would (decision 0031), so the witness's instance is
// required in the same way: through a requirement call when the operand type mentions a type parameter, else
// through a call of the generic witness with the operand's type arguments.
void TypeCheckingBuilder::requireEqualityWitness(const syntax::BinaryExprSyntax::Ptr& expression, TypeId operand) {
    const auto requirement = model->equatableRequirement();
    if (!requirement.has_value()) return;
    if (model->typeContext.mentionsTypeParameter(operand)) {
        if (currentFunction.has_value()) {
            requirementCalls.push_back({ *currentFunction, *requirement, operand, expression->span });
        }
        return;
    }
    equalityRoots.push_back({ operand, currentFunction, expression->span });
}

// `a == b` on a conformer other than a builtin is analyzed like the call `a.equals(other: b)`: the left
// operand stays borrowed while the right is evaluated (spec 20.13.2), so the right may not write or move it.
void TypeCheckingBuilder::validateEqualityExclusivity(const syntax::BinaryExprSyntax::Ptr& expression) {
    std::vector<CallAccess> accesses;
    if (const auto path = storagePath(expression->left)) {
        collectSubscriptIndexAccesses(expression->left, accesses);
        accesses.push_back(CallAccess { *path, syntax::AccessEffect::borrowing, expression->left->span, true });
    } else {
        collectEvaluationAccesses(expression->left, accesses);
    }
    collectEvaluationAccesses(expression->right, accesses);
    validateCallExclusivity(accesses);
}

void TypeCheckingBuilder::reportInvalidOperator(
        const Token::Ptr& op,
        const std::vector<TypeId>& operands,
        SourceSpan span) {
    std::string message = "operator '" +
            (op == nullptr ? std::string("<unknown>") : op->rawValue) +
            "' cannot be applied to";
    for (size_t index = 0; index < operands.size(); ++index) {
        message += (index == 0 ? " '" : ", '") +
                model->typeContext.displayName(operands[index]) + "'";
    }
    std::optional<std::string> help;
    const bool equality = op != nullptr && (op->rawValue == "==" || op->rawValue == "!=");
    if (equality && operands.size() == 2 && operands[0] == operands[1]) {
        help = equatableHelp(operands[0], span);
    } else if (equality && operands.size() == 2) {
        // No wrapping between the operands (spec 20.13.2): `opt == 5` compares with `.Some(5)`.
        for (size_t index = 0; index < 2; ++index) {
            const auto* optional = model->typeContext.type(operands[index]);
            if (model->typeContext.isOptional(operands[index]) && optional != nullptr &&
                !optional->arguments.empty() && optional->arguments[0] == operands[1 - index]) {
                help = "the operands have different types; wrap the plain one as '.Some(...)' to compare it "
                       "with the optional, or compare with 'nil' to test for a value";
            }
        }
    }
    report(TypeCheckingDiagnosticId::invalidOperatorOperands, span, std::move(message), std::move(help));
}

// Why `type` has no `==`, and what would give it one (spec 20.13). It suggests only a conformance the
// compiler accepts at `span`.
std::optional<std::string> TypeCheckingBuilder::equatableHelp(TypeId type, SourceSpan span) const {
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return std::nullopt;
    const auto name = model->typeContext.displayName(type);
    if (record->kind == TypeKind::typeParameter) {
        return "add 'Equatable' to the constraint of '" + name + "' where it is declared (for an extension's "
               "parameter, in the type's own declaration)";
    }
    if (record->kind == TypeKind::function) {
        return "a function type never conforms to 'Equatable' (spec 2.3.1); compare what the functions return";
    }
    // A container conforms when its components do (spec 20.13.3): name the first that does not. A component that
    // already has a conformance is explained by its failing member, never told to declare another.
    if (model->isArrayRoleType(type) || model->typeContext.isOptional(type) || model->typeContext.isResult(type)) {
        const auto arguments = record->arguments;
        for (const auto argument : arguments) {
            if (argument == model->typeContext.neverType() || isEquatable(argument)) continue;
            std::optional<std::string> inner;
            if (model->equatableConformance(argument) != nullptr) {
                if (const auto path = nonEquatablePath(argument)) {
                    inner = "its member '" + path->first + "' has the type '" +
                            model->typeContext.displayName(path->second) + "', which does not conform";
                }
            } else {
                inner = equatableHelp(argument, span);
            }
            const auto what = model->isArrayRoleType(type) ? "element" : "payload";
            std::string help = "'" + name + "' compares its " + what + "s, and its " + what + " type '" +
                    model->typeContext.displayName(argument) + "' does not conform to 'Equatable'";
            if (inner.has_value()) help += "; " + *inner;
            if (model->typeContext.isOptional(type)) help += "; to test for a value, compare with 'nil'";
            return help;
        }
    }
    if (model->isLinear(type)) {
        return "'" + name + "' is linear, and a linear type never conforms to 'Equatable' (spec 20.13.3); "
               "compare a field instead";
    }
    if (model->isDictRoleType(type)) {
        return "a dictionary never conforms to 'Equatable' (spec 20.13.3)";
    }
    const auto* symbol = model->semanticModelValue->symbol(record->symbol);
    if ((record->kind == TypeKind::structure || record->kind == TypeKind::enumeration) && symbol != nullptr &&
        symbol->libraryRole == semantic::LibraryRole::none && !model->semanticModelValue->isPreludeSource(symbol->span)) {
        if (model->equatableConformance(type) != nullptr) {
            if (const auto path = nonEquatablePath(type)) {
                return "'" + name + "' derives 'Equatable', but its member '" + path->first + "' has the type '" +
                        model->typeContext.displayName(path->second) + "', which does not conform";
            }
            return "'" + name + "' declares 'Equatable', but that conformance is rejected";
        }
        if (symbol->moduleId != model->semanticModelValue->moduleOf(span)) {
            return "'" + name + "' does not conform to 'Equatable', and only its own module can declare that "
                   "conformance (spec 20.3)";
        }
        auto header = symbol->name;
        if (!symbol->typeParameters.empty()) {
            header += "<";
            for (size_t index = 0; index < symbol->typeParameters.size(); ++index) {
                if (index != 0) header += ", ";
                header += symbolName(symbol->typeParameters[index]);
            }
            header += ">";
        }
        // An existing `equals` method becomes the witness, and members that can all conform derive one; only
        // otherwise must the witness be written (spec 20.13.3).
        bool derivable = true;
        for (const auto& member : model->equatableMembers(type)) {
            std::vector<TypeId> inProgress { type };
            if (member.type == model->typeContext.neverType()) continue;
            if (!satisfiesEquatable(member.type, inProgress, true)) { derivable = false; break; }
        }
        if (lookupMember(type, "equals").has_value() || derivable) {
            return "'" + name + "' does not conform to 'Equatable'; declare 'extension " + header +
                    ": Equatable {}'";
        }
        return "'" + name + "' does not conform to 'Equatable', and not every member can be compared; declare "
               "'extension " + header + ": Equatable { func equals(other: " + header + "): Bool { ... } }'";
    }
    return "'" + name + "' does not conform to 'Equatable' (spec 20.13)";
}

TypeId TypeCheckingBuilder::commonType(TypeId left, TypeId right, SourceSpan mismatchSpan) {
    if (left == model->typeContext.errorType() ||
        right == model->typeContext.errorType()) {
        return model->typeContext.errorType();
    }
    if (left == model->typeContext.neverType()) return right;
    if (right == model->typeContext.neverType()) return left;
    if (model->typeContext.isAssignable(left, right)) return right;
    if (model->typeContext.isAssignable(right, left)) return left;
    requireAssignable(right, left, mismatchSpan);
    return model->typeContext.errorType();
}

} // namespace joyeer::typing
