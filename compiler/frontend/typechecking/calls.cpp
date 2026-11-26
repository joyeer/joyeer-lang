#include "builder.h"
#include "compiler/frontend/intrinsics.h"
#include "compiler/frontend/nameresolution.h"

#include <algorithm>
#include <utility>

namespace joyeer::typing {

// `f(a, &b)` or `(s.f)(a)` where the callee is a value of function type (spec 5.6.1): positional
// arguments, each checked against the type's parameter effect and type.
TypeId TypeCheckingBuilder::checkFunctionValueCall(
        const syntax::CallExprSyntax::Ptr& expression,
        TypeId calleeType,
        ReceiverMarker receiverMarker) {
    // Copied out: checking the arguments can grow the type table and invalidate the record.
    const auto parameters = model->typeContext.type(calleeType)->arguments;
    const auto effects = model->typeContext.type(calleeType)->effects;
    const auto result = parameters.back();
    if (receiverMarker != ReceiverMarker::none) {
        report(TypeCheckingDiagnosticId::invalidAccessMarker, expression->span,
               "'&' and 'consume' mark the arguments of a call through a function value, not the call",
               "remove the marker before the call");
    }
    for (const auto& argument : expression->arguments) {
        if (argument->label == nullptr) continue;
        report(TypeCheckingDiagnosticId::labeledFunctionValueCall, argument->label->span,
               "a call through a function value takes positional arguments; remove '" +
                       argument->label->rawValue + ":'",
               "a function type has no parameter labels");
    }
    if (expression->arguments.size() != effects.size()) {
        report(TypeCheckingDiagnosticId::invalidCallArguments, expression->span,
               "a function value of type '" + model->typeContext.displayName(calleeType) + "' takes " +
                       std::to_string(effects.size()) + " argument(s), but " +
                       std::to_string(expression->arguments.size()) + " were given");
    }
    std::vector<CallAccess> accesses;
    for (size_t index = 0; index < expression->arguments.size(); ++index) {
        const auto& argument = expression->arguments[index];
        const auto expected = index < effects.size()
                ? std::optional<TypeId>(parameters[index])
                : std::optional<TypeId>();
        const auto actual = checkExpression(argument->value, expected);
        if (index >= effects.size()) continue;
        checkArgumentAccess(effects[index], *argument);
        collectCallArgumentAccess(*argument, effects[index], accesses);
        if (actual.has_value()) requireAssignable(*actual, parameters[index], argument->value->span);
    }
    // The callee was evaluated to a copy first and holds no access (spec 4.4), so only the
    // arguments can conflict.
    validateCallExclusivity(accesses);
    return result;
}

// True for `receiver.name(...)`, ignoring parentheses.
bool TypeCheckingBuilder::isMemberCall(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isMemberCall(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                        ->expression);
    }
    return expression->kind == syntax::Kind::callExpr &&
            std::static_pointer_cast<syntax::CallExprSyntax>(expression)
                    ->callee->kind == syntax::Kind::memberExpr;
}

// How a call accesses its receiver: builtin members by their mutability,
// user methods by their declared receiver effect.
std::optional<syntax::AccessEffect> TypeCheckingBuilder::receiverEffectOf(const semantic::Symbol* target) const {
    if (target == nullptr) return std::nullopt;
    if (target->kind == semantic::SymbolKind::builtinMember) {
        if (target->callable.has_value() && target->callable->receiver.has_value()) {
            return target->callable->receiver->access;
        }
        return target->isMutable
                ? syntax::AccessEffect::inout
                : syntax::AccessEffect::borrowing;
    }
    if (target->kind == semantic::SymbolKind::method && target->callable.has_value() &&
        target->callable->receiver.has_value()) {
        return target->callable->receiver->access;
    }
    return std::nullopt;
}

TypeId TypeCheckingBuilder::checkCall(
        const syntax::CallExprSyntax::Ptr& expression,
        ReceiverMarker receiverMarker,
        std::optional<TypeId> expectedResult) {
    // A `place_address` call is accepted only as a direct argument of another call (decision 0026), and not of
    // an initializer or enum case, which would store the address beyond the call (stage 2b review).
    const auto outerTarget = model->callTarget(expression);
    const auto* outerSymbol = outerTarget.has_value() ? model->semanticModelValue->symbol(*outerTarget) : nullptr;
    const bool outerStores = outerSymbol != nullptr && storesItsArguments(*outerSymbol);
    for (const auto& argument : expression->arguments) {
        if (outerStores) break;
        auto direct = argument->value;
        while (direct != nullptr && direct->kind == syntax::Kind::parenthesizedExpr) {
            direct = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(direct)->expression;
        }
        if (direct != nullptr && direct->kind == syntax::Kind::callExpr) permittedPlaceAddress.insert(direct.get());
    }
    const auto* savedCallee = currentCallee;
    currentCallee = expression->callee.get();
    const auto calleeType = checkExpression(expression->callee).value_or(
            model->typeContext.errorType());
    currentCallee = savedCallee;
    auto target = model->callTarget(expression);
    if (!target.has_value() && isFunctionType(calleeType)) {
        const auto callee = model->referencedSymbol(expression->callee);
        const auto* calleeSymbol = callee.has_value() ? model->semanticModelValue->symbol(*callee) : nullptr;
        if (expression->callee->kind == syntax::Kind::memberExpr &&
            !model->semanticModelValue->isModuleQualified(expression->callee)) {
            // `s.f(x)` always names a method (spec 5.4).
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
            const auto name = member->member == nullptr ? std::string() : member->member->rawValue;
            report(TypeCheckingDiagnosticId::notCallable, expression->callee->span,
                   "'" + name + "' is a stored function value, not a method; 'value." + name +
                           "(...)' always calls a method",
                   "call the stored function with a parenthesized callee: '(value." + name + ")(...)'");
            for (const auto& argument : expression->arguments) checkExpression(argument->value);
            return model->typeContext.errorType();
        }
        if (expression->callee->kind != syntax::Kind::nameExpr &&
            expression->callee->kind != syntax::Kind::parenthesizedExpr &&
            expression->callee->kind != syntax::Kind::propagateExpr &&
            expression->callee->kind != syntax::Kind::memberExpr) {
            report(TypeCheckingDiagnosticId::notCallable, expression->callee->span,
                   "a call through a function value needs a plain name or a parenthesized callee",
                   "put the callee in parentheses, as in '(table[i])(x)' or '(make())(x)'");
            for (const auto& argument : expression->arguments) checkExpression(argument->value);
            return model->typeContext.errorType();
        }
        if (calleeSymbol == nullptr || !calleeSymbol->callable.has_value()) {
            return checkFunctionValueCall(expression, calleeType, receiverMarker);
        }
    }
    if (!target.has_value()) {
        const auto callee = model->referencedSymbol(expression->callee);
        if (callee.has_value()) {
            const auto* symbol = model->semanticModelValue->symbol(*callee);
            if (symbol != nullptr && symbol->kind == semantic::SymbolKind::structure &&
                symbol->synthesizedInitializer.has_value()) {
                target = symbol->synthesizedInitializer;
            } else if (symbol != nullptr && symbol->callable.has_value()) {
                target = callee;
            }
        }
        if (target.has_value()) {
            if (!requireAccessible(*target, expression->span)) {
                return model->typeContext.errorType();
            }
            recordResolvedCallTarget(expression, *target);
            for (auto& diagnostic : semantic::validateCallArguments(
                    *expression, *model->semanticModelValue->symbol(*target))) {
                report(
                        TypeCheckingDiagnosticId::invalidCallArguments,
                        diagnostic.span,
                        std::move(diagnostic.message));
            }
        }
    }

    const auto* signature = target.has_value() ? model->callable(*target) : nullptr;
    // A generic call infers its type arguments (spec 20.4.1), and a requirement call on `T`
    // replaces the protocol's `Self` with `T` (spec 20.5); either way the argument checks below
    // use the substituted signature.
    TypedCallableSignature substitutedSignature;
    std::vector<std::optional<std::optional<TypeId>>> preChecked(expression->arguments.size());
    const auto* calledSymbol = target.has_value() ? model->semanticModelValue->symbol(*target) : nullptr;
    if (isEnumCase(calledSymbol)) {
        for (const auto& argument : expression->arguments) permitMove(argument->value);
    }
    if (calledSymbol != nullptr && calledSymbol->libraryRole == semantic::LibraryRole::taskStart) {
        model->spawnCallSpans.push_back(expression->span);
        if (!validateTaskTarget(expression)) return model->typeContext.errorType();
    }
    if (calledSymbol != nullptr && calledSymbol->libraryRole == semantic::LibraryRole::channelCreate) {
        const auto before = diagnostics.size();
        (void) checkChannel(expression, *calledSymbol);
        if (diagnostics.size() != before) return model->typeContext.voidType();
    }
    if (signature != nullptr && calledSymbol != nullptr) {
        const auto* genericParameters = model->genericParameters(*target);
        const auto* owner = calledSymbol->containingSymbol.has_value()
                ? model->semanticModelValue->symbol(*calledSymbol->containingSymbol)
                : nullptr;
        const bool constructs = calledSymbol->kind == semantic::SymbolKind::synthesizedInitializer ||
                calledSymbol->kind == semantic::SymbolKind::enumCase;
        if (genericParameters != nullptr &&
            (calledSymbol->kind == semantic::SymbolKind::function || constructs)) {
            substitutedSignature = inferGenericCall(
                    expression, *target, *calledSymbol, *signature, *genericParameters, preChecked,
                    constructs ? expectedResult : std::nullopt);
            signature = &substitutedSignature;
        } else if (owner != nullptr && !owner->typeParameters.empty() &&
                   calledSymbol->kind == semantic::SymbolKind::method &&
                   expression->callee->kind == syntax::Kind::memberExpr) {
            // A method of a generic type: its parameters are the receiver's type arguments.
            const auto base = model->typeOf(
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base);
            const auto substitution = baseSubstitution(
                    base.value_or(model->typeContext.errorType()), parametersOfMember(*target));
            if (const auto* receiver = base.has_value() ? model->typeContext.type(*base) : nullptr;
                receiver != nullptr && !receiver->arguments.empty() &&
                model->genericParameters(*target) != nullptr) {
                model->genericCalls[*model->semanticModelValue->nodeId(expression)] = GenericCall {
                    *target, receiver->arguments, currentFunction, expression->span };
            }
            substitutedSignature = *signature;
            for (auto& parameter : substitutedSignature.parameters) {
                parameter = model->typeContext.substitute(parameter, substitution);
            }
            substitutedSignature.result = model->typeContext.substitute(substitutedSignature.result, substitution);
            signature = &substitutedSignature;
        } else if (owner != nullptr && owner->kind == semantic::SymbolKind::protocol &&
                   expression->callee->kind == syntax::Kind::memberExpr) {
            const auto base = model->typeOf(
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base);
            const std::unordered_map<semantic::SymbolId, TypeId> self {
                { owner->id, base.value_or(model->typeContext.errorType()) } };
            if (base.has_value() && currentFunction.has_value() && model->typeContext.mentionsTypeParameter(*base)) {
                requirementCalls.push_back({ *currentFunction, *target, *base, expression->span });
            }
            substitutedSignature = *signature;
            for (auto& parameter : substitutedSignature.parameters) {
                parameter = model->typeContext.substitute(parameter, self);
            }
            substitutedSignature.result = model->typeContext.substitute(substitutedSignature.result, self);
            signature = &substitutedSignature;
        }
    }
    if (target.has_value() && model->nativeBinding(*target) != nullptr &&
        !model->semanticModelValue->isRuntimeSource(expression->span)) {
        report(TypeCheckingDiagnosticId::unauthorizedNativeBinding, expression->span,
               "direct calls to C bindings require a runtime compilation unit; use a checked Joyeer wrapper");
    }
    const auto* semanticTarget = target.has_value()
            ? model->semanticModelValue->symbol(*target)
            : nullptr;
    recordProcessLaunchUse(semanticTarget, expression->span);
    if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
        semanticTarget->name == "print" && model->semanticModelValue->isCoreSource(expression->span)) {
        report(TypeCheckingDiagnosticId::managedTypeInCoreUnit, expression->span,
               "a runtime core unit must not call print, which is implemented by the runtime");
    }
    if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
        intrinsic::isLayoutIntrinsic(semanticTarget->name)) {
        return checkLayoutIntrinsic(expression, *semanticTarget);
    }
    if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::method) {
        // A method needs a receiver value; `Type.method(...)` has none.
        const auto base = expression->callee->kind == syntax::Kind::memberExpr
                ? std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base
                : syntax::ExprPtr();
        const auto baseSymbol = base == nullptr
                ? std::optional<semantic::SymbolId>()
                : model->semanticModelValue->referencedSymbol(base);
        const auto* baseDeclaration = baseSymbol.has_value()
                ? model->semanticModelValue->symbol(*baseSymbol)
                : nullptr;
        if (base == nullptr ||
            (baseDeclaration != nullptr &&
             (baseDeclaration->kind == semantic::SymbolKind::structure ||
              baseDeclaration->kind == semantic::SymbolKind::enumeration ||
              baseDeclaration->kind == semantic::SymbolKind::typeParameter ||
              baseDeclaration->kind == semantic::SymbolKind::protocol ||
              baseDeclaration->kind == semantic::SymbolKind::builtinType))) {
            report(
                    TypeCheckingDiagnosticId::invalidCallArguments,
                    expression->callee->span,
                    "method '" + semanticTarget->name + "' must be called on a value");
        }
    }

    const auto receiverEffect = receiverEffectOf(semanticTarget);
    const auto requiredReceiver = receiverEffect.value_or(syntax::AccessEffect::borrowing);
    const auto suppliedReceiver = receiverMarker == ReceiverMarker::inout
            ? syntax::AccessEffect::inout
            : receiverMarker == ReceiverMarker::consume
                    ? syntax::AccessEffect::consuming
                    : syntax::AccessEffect::borrowing;
    const auto member = receiverEffect.has_value() && expression->callee->kind == syntax::Kind::memberExpr
            ? std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)
            : nullptr;
    bool missingStoredReceiver = false;
    bool elementReceiver = false;
    if (member != nullptr && requiredReceiver == syntax::AccessEffect::consuming &&
        semanticTarget != nullptr &&
        (semanticTarget->kind == semantic::SymbolKind::builtinMember ||
         semanticTarget->libraryRole == semantic::LibraryRole::storedConsumer ||
         semanticTarget->libraryRole == semantic::LibraryRole::containerEnd)) {
        const auto path = storagePath(member->base);
        const auto* root = path.has_value() ? model->semanticModelValue->symbol(path->root) : nullptr;
        const auto* scope = root != nullptr ? model->semanticModelValue->scope(root->ownerScope) : nullptr;
        elementReceiver = path.has_value() &&
                std::any_of(path->projections.begin(), path->projections.end(),
                            [](const auto& field) { return !field.has_value(); });
        missingStoredReceiver = !path.has_value() || elementReceiver ||
                (scope != nullptr && (scope->kind == semantic::ScopeKind::file ||
                                      scope->kind == semantic::ScopeKind::module));
    }
    // An unknown member already has its own diagnostic; a marker complaint would only cascade.
    // A stored consumer cannot use a temporary even if the marker is corrected.
    if (requiredReceiver != suppliedReceiver && semanticTarget != nullptr && !missingStoredReceiver) {
        const bool consumeInvolved = requiredReceiver == syntax::AccessEffect::consuming ||
                suppliedReceiver == syntax::AccessEffect::consuming;
        report(
                consumeInvolved
                        ? TypeCheckingDiagnosticId::invalidConsumeArgument
                        : TypeCheckingDiagnosticId::invalidAccessMarker,
                expression->callee->span,
                requiredReceiver == syntax::AccessEffect::inout
                        ? "mutating method call requires '&' on the receiver"
                        : requiredReceiver == syntax::AccessEffect::consuming
                                ? "consuming method call requires 'consume' on the receiver"
                                : suppliedReceiver == syntax::AccessEffect::inout
                                        ? "non-mutating call must not use '&' on the receiver"
                                        : "non-consuming call must not use 'consume' on the receiver",
                requiredReceiver == syntax::AccessEffect::consuming
                        ? std::optional<std::string>(
                                  "write 'consume' before the receiver, as in 'let v = consume x." +
                                  semanticTarget->name +
                                  "()'; to use a result as an argument or before '?', bind or parenthesize it first")
                        : std::nullopt);

    }

    std::vector<CallAccess> accesses;
    if (member != nullptr) {
        if (requiredReceiver == syntax::AccessEffect::inout) {
            const auto storage = analyzeStorage(member->base);
            if (!storage.writable) {
                report(
                        TypeCheckingDiagnosticId::assignmentToImmutable,
                        member->base->span,
                        "cannot call mutating method through immutable binding '" +
                                storage.immutableName + "'");
            }
        } else if (missingStoredReceiver) {
            report(
                    TypeCheckingDiagnosticId::invalidConsumeArgument,
                    member->base->span,
                    "'" + semanticTarget->name + "' needs a value stored in a variable, "
                    "parameter or field, not a temporary, an array element or a top-level constant",
                    elementReceiver
                            ? "remove the element with '&array.removeLast()' and call '" +
                                  semanticTarget->name + "' on the result"
                            : "store the value with 'let handle = ...', then write 'consume handle." +
                                  semanticTarget->name + "()'");
        } else if (requiredReceiver == syntax::AccessEffect::consuming &&
                   !isConsumable(member->base)) {
            report(
                    TypeCheckingDiagnosticId::invalidConsumeArgument,
                    member->base->span,
                    "consuming receiver must be an owning local, consuming parameter, or temporary",
                    "declare the parameter 'consuming' or the method 'consuming', or bind the "
                    "value to a local first");
        }
    }
    collectCallReceiverAccess(expression, semanticTarget, accesses);

    std::optional<TypeId> receiverResultType;
    if (semanticTarget != nullptr &&
        semanticTarget->kind == semantic::SymbolKind::builtinMember &&
        expression->callee->kind == syntax::Kind::memberExpr) {
        const auto member =
                std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
        const auto base = model->typeOf(member->base);
        const auto* baseType = base.has_value()
                ? model->typeContext.type(*base)
                : nullptr;
        if (semanticTarget->name == "utf8" &&
                   baseType != nullptr && baseType->kind == TypeKind::string) {
            receiverResultType = model->typeContext.arrayType(model->typeContext.uint8Type());
            model->symbolTypes[semanticTarget->id] = *receiverResultType;
            model->callables.at(semanticTarget->id).result = *receiverResultType;
        }
    }
    // `d.get(key:)` of a library dictionary returns a copy of the stored value (decision 0027): keyed on the
    // callee's role, a noncopyable value is rejected here, before any instance of the method is demanded.
    if (semanticTarget != nullptr && semanticTarget->libraryRole == semantic::LibraryRole::dictGet &&
        expression->callee->kind == syntax::Kind::memberExpr) {
        const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
        const auto base = model->typeOf(member->base);
        const auto* baseType = base.has_value() ? model->typeContext.type(*base) : nullptr;
        if (baseType != nullptr && baseType->arguments.size() == 2) {
            const auto valueType = baseType->arguments[1];
            if (const auto reason = model->noncopyableReason(valueType)) {
                report(TypeCheckingDiagnosticId::copyOfNoncopyable, expression->span,
                       "'get' returns a copy of the stored value, and '" +
                               model->typeContext.displayName(valueType) + "' is noncopyable: " + *reason,
                       "a noncopyable element cannot be read without removing it, which is not supported yet");
            }
        }
    }
    const bool literalAddressCall = target.has_value() &&
            model->nativeBinding(*target) != nullptr &&
            intrinsic::readsLiteral(model->nativeBinding(*target)->symbol);
    for (size_t index = 0; index < expression->arguments.size(); ++index) {
        const auto& argument = expression->arguments[index];
        if (literalAddressCall && index == 0) {
            // The data is read at compile time: it must be a string literal.
            const auto literal = argument->value != nullptr &&
                    argument->value->kind == syntax::Kind::literalExpr
                    ? std::static_pointer_cast<syntax::LiteralExprSyntax>(argument->value)
                    : nullptr;
            if (literal == nullptr || literal->literal == nullptr ||
                literal->literal->kind != stringLiteral) {
                report(TypeCheckingDiagnosticId::invalidCallArguments,
                       argument->value != nullptr ? argument->value->span : argument->span,
                       model->nativeBinding(*target)->symbol + " requires a string literal argument");
            } else {
                literalAddressArgument = argument->value.get();
            }
        }
        const auto parameterIndex = semanticTarget == nullptr
                ? std::optional<size_t>()
                : parameterIndexForArgument(*semanticTarget, *argument, index);
        const auto expected = signature != nullptr && parameterIndex.has_value() &&
                  *parameterIndex < signature->parameters.size()
                ? std::optional<TypeId>(signature->parameters[*parameterIndex])
                : std::optional<TypeId>();
        const auto actual = preChecked[index].has_value()
                ? *preChecked[index]
                : checkExpression(argument->value, expected);
        // A memberwise initializer, an enum case and `append` store their argument, so it
        // is an owned context; every other parameter borrows or is marked `consume`.
        const auto roleOwnedArgument = semanticTarget != nullptr && parameterIndex.has_value() &&
                model->isOwnedArgumentContext(semanticTarget->id, *parameterIndex);
        const auto storesArgument = semanticTarget != nullptr &&
                (storesItsArguments(*semanticTarget) || roleOwnedArgument);
        if (storesArgument) requireStoredArgument(*argument, actual);
        if (roleOwnedArgument &&
            (argument->accessMarker == nullptr || consumesArgument(*argument))) {
            collectCallArgumentAccess(*argument,
                    consumesArgument(*argument) ? syntax::AccessEffect::consuming
                                                : syntax::AccessEffect::borrowing, accesses);
        } else if (semanticTarget != nullptr && parameterIndex.has_value() &&
            !(storesArgument && consumesArgument(*argument))) {
            checkCallArgumentAccess(*semanticTarget, *parameterIndex, *argument);
            const auto effect = semanticTarget->callable->parameters[*parameterIndex].access;
            collectCallArgumentAccess(*argument, effect, accesses, !isEnumCase(semanticTarget));
        }
        if (expected.has_value() && actual.has_value()) {
            requireAssignable(*actual, *expected, argument->value->span);
        }
        if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
            semanticTarget->name == "print" && actual.has_value() && (containsFunctionType(*actual) ||
                                                   model->typeContext.mentionsTypeParameter(*actual))) {
            report(TypeCheckingDiagnosticId::typeMismatch, argument->value->span,
                   "'print' cannot print a value of type '" +
                           model->typeContext.displayName(*actual) + "'",
                   "print a number, a Bool or a String");
        }
    }
    validateCallExclusivity(accesses);

    if (signature == nullptr && calleeType != model->typeContext.errorType()) {
        const auto callee = model->referencedSymbol(expression->callee);
        const auto* symbol = callee.has_value()
                ? model->semanticModelValue->symbol(*callee)
                : nullptr;
        report(
                TypeCheckingDiagnosticId::notCallable,
                expression->callee->span,
                (symbol == nullptr ? std::string("this expression") : "value '" + symbol->name + "'") +
                        " of type '" + model->typeContext.displayName(calleeType) +
                        "' is not callable",
                isOptionalFunction(calleeType)
                        ? std::optional<std::string>(
                                  "unwrap it first with a 'match', or write 'value?(...)' to return nil from the "
                                  "enclosing function when it is nil")
                        : std::nullopt);
    }
    return receiverResultType.value_or(
            signature == nullptr ? model->typeContext.errorType() : signature->result);
}

void TypeCheckingBuilder::recordProcessLaunchUse(const semantic::Symbol* symbol, SourceSpan span) {
    if (symbol == nullptr || symbol->kind != semantic::SymbolKind::function ||
        (symbol->name != "runProcess" && symbol->name != "startProcess")) return;
    const auto* module = model->semanticModelValue->moduleName(symbol->moduleId);
    if (module != nullptr && *module == "System") model->processLaunchUseSpans.push_back(span);
}

TypeId TypeCheckingBuilder::checkChannel(const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol& target) {
    const auto error = model->typeContext.errorType();
    // Labels, their order and the argument count are name resolution's ordinary call rules.
    if (expression->arguments.size() != 3) return error;
    const auto capacityType = checkExpression(expression->arguments[0]->value, model->typeContext.intType());
    if (capacityType.has_value()) {
        requireAssignable(*capacityType, model->typeContext.intType(), expression->arguments[0]->value->span);
    }
    // An integer literal capacity (optionally negated) below 1 is a compile-time error.
    auto literal = expression->arguments[0]->value;
    bool negated = false;
    if (literal != nullptr && literal->kind == syntax::Kind::prefixExpr) {
        const auto prefix = std::static_pointer_cast<syntax::PrefixExprSyntax>(literal);
        if (prefix->op != nullptr && prefix->op->kind == minus) {
            negated = true;
            literal = prefix->operand;
        }
    }
    if (literal != nullptr && literal->kind == syntax::Kind::literalExpr) {
        const auto value = std::static_pointer_cast<syntax::LiteralExprSyntax>(literal);
        if (value->literal != nullptr && value->literal->kind == decimalLiteral) {
            const bool belowOne = negated || value->literal->intValue < 1;
            if (belowOne) {
                report(TypeCheckingDiagnosticId::channelCapacity, expression->arguments[0]->value->span,
                       "a channel needs a capacity of at least 1",
                       "pass 1 or more, for example channel(capacity: 8, sender: &sender, receiver: &receiver)");
            }
        }
    }
    checkCallArgumentAccess(target, 0, *expression->arguments[0]);
    const auto diagnosticsBefore = diagnostics.size();
    std::vector<CallAccess> accesses;
    std::vector<TypeId> endTypes;
    for (size_t index = 1; index < 3; ++index) {
        const auto& argument = expression->arguments[index];
        const auto type = checkExpression(argument->value);
        checkCallArgumentAccess(target, index, *argument);
        collectCallArgumentAccess(*argument, syntax::AccessEffect::initializing, accesses);
        endTypes.push_back(type.value_or(error));
    }
    validateCallExclusivity(accesses);
    const auto* senderType = model->typeContext.type(endTypes[0]);
    const auto* receiverType = model->typeContext.type(endTypes[1]);
    const auto* sender = senderType == nullptr ? nullptr : model->semanticModelValue->symbol(senderType->symbol);
    const auto* receiver = receiverType == nullptr ? nullptr : model->semanticModelValue->symbol(receiverType->symbol);
    // An operand that already failed (a marker, a non-variable, the same variable twice) has its own diagnostic.
    if (endTypes[0] == error || endTypes[1] == error || diagnostics.size() != diagnosticsBefore) {
        return model->typeContext.voidType();
    }
    if (senderType == nullptr || receiverType == nullptr || sender == nullptr || receiver == nullptr ||
        sender->libraryRole != semantic::LibraryRole::channelSender ||
        receiver->libraryRole != semantic::LibraryRole::channelReceiver ||
        senderType->arguments.size() != 1 || receiverType->arguments.size() != 1 ||
        senderType->arguments[0] != receiverType->arguments[0]) {
        report(TypeCheckingDiagnosticId::channelEndTypes, expression->span,
               "'channel' needs a ChannelSender<T> and a ChannelReceiver<T> of the same element type, but got '" +
                       model->typeContext.displayName(endTypes[0]) + "' and '" +
                       model->typeContext.displayName(endTypes[1]) + "'",
               "declare var sender: ChannelSender<Int> and var receiver: ChannelReceiver<Int>, then "
               "channel(capacity: 8, sender: &sender, receiver: &receiver)");
    }
    return model->typeContext.voidType();
}

// `spawn(function: f, input: consume x)` (spec 21.2): `f` must name one top-level function with a
// single consuming parameter; the call has type `JoinHandle<R>` for the function's result `R`.
bool TypeCheckingBuilder::validateTaskTarget(const syntax::CallExprSyntax::Ptr& expression) {
    // Labels, their order and the argument count are checked with the ordinary call rules.
    if (expression->arguments.size() != 2) return false;
    const auto& functionArgument = expression->arguments[0];
    const auto span = functionArgument->value != nullptr ? functionArgument->value->span : functionArgument->span;

    // `function:` is read as a name, not checked as a value, so a member reference that name
    // resolution deferred (an instance's member) must be marked handled here or it is reported twice.
    auto functionExpression = functionArgument->value;
    while (functionExpression != nullptr && functionExpression->kind == syntax::Kind::parenthesizedExpr) {
        functionExpression = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(functionExpression)->expression;
    }
    if (functionExpression != nullptr) markDeferredReferenceHandled(functionExpression);
    std::optional<semantic::SymbolId> named;
    if (functionExpression != nullptr &&
        (functionExpression->kind == syntax::Kind::nameExpr ||
         functionExpression->kind == syntax::Kind::memberExpr)) {
        named = model->referencedSymbol(functionExpression);
    }

    const auto* target = named.has_value() ? model->semanticModelValue->symbol(*named) : nullptr;
    if (target == nullptr || (target->kind != semantic::SymbolKind::function &&
                              target->kind != semantic::SymbolKind::method &&
                              target->kind != semantic::SymbolKind::builtinFunction)) {
        // An unknown name is name resolution's diagnostic; this is a name that is not a function
        // (a variable, a call, any other expression).
        if (functionExpression != nullptr && !model->referencedSymbol(functionExpression).has_value() &&
            functionExpression->kind == syntax::Kind::nameExpr) {
            return false;
        }
        report(TypeCheckingDiagnosticId::spawnTargetNotFunction, span,
               "'function:' must name a top-level function; a function value held in a variable or an expression is not accepted here (spec 21.2.1)",
               "pass the name of a top-level function, for example spawn(function: work, input: consume value)");
        return false;
    }
    const bool isMain = target->isRootModule && target->name == "main";
    const bool isGeneric = model->genericParameters(*named) != nullptr;
    if (target->kind != semantic::SymbolKind::function || isMain || isGeneric ||
        model->nativeBinding(*named) != nullptr) {
        report(TypeCheckingDiagnosticId::spawnTargetUnsupported, span,
               "'" + target->name + "' cannot be a task: " +
                       (target->kind == semantic::SymbolKind::method ? "it is a method"
                        : isMain ? "it is the program entry point"
                        : isGeneric ? "it is generic (spec 20.4.1)"
                        : target->kind == semantic::SymbolKind::builtinFunction ? "it is a builtin function"
                                                                                : "it is a native binding"),
               "a task runs a top-level Joyeer function other than 'main'");
        return false;
    }
    const auto* signature = model->callable(*named);
    const auto& parameters = target->callable.has_value()
            ? target->callable->parameters
            : std::vector<semantic::CallableParameter> {};
    if (signature == nullptr || parameters.size() != 1 ||
        parameters[0].access != syntax::AccessEffect::consuming) {
        const bool wrongCount = parameters.size() != 1;
        report(TypeCheckingDiagnosticId::spawnTargetParameters, span,
               "'" + target->name + "' must have exactly one parameter, and it must be 'consuming'; it has " +
                       (wrongCount ? std::to_string(parameters.size()) + " parameter(s)"
                                   : std::string("one parameter that is not 'consuming'")),
               wrongCount
                       ? "wrap the input in a struct (an empty struct if there is none) and take it as the one consuming parameter"
                       : "declare the parameter 'consuming'; a borrowed or inout parameter cannot cross to another thread");

        return false;
    }
    if (signature->result == model->typeContext.neverType()) {
        report(TypeCheckingDiagnosticId::spawnTargetNever, span,
               "'" + target->name + "' never returns, so its task could not be joined",
               "declare a result type, or call the function directly");
        return false;
    }

    return true;
}

std::optional<size_t> TypeCheckingBuilder::parameterIndexForArgument(
        const semantic::Symbol& target,
        const syntax::CallArgumentSyntax& argument,
        size_t positionalIndex) const {
    if (!target.callable.has_value()) return std::nullopt;
    if (target.callable->kind == semantic::CallableKind::enumCase) {
        return positionalIndex < target.callable->parameters.size()
                ? std::optional<size_t>(positionalIndex)
                : std::nullopt;
    }
    if (argument.label == nullptr) return std::nullopt;
    for (size_t index = 0; index < target.callable->parameters.size(); ++index) {
        const auto& label = target.callable->parameters[index].label;
        if (label.has_value() && *label == argument.label->rawValue) return index;
    }
    return std::nullopt;
}

} // namespace joyeer::typing
