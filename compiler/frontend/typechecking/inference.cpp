#include "builder.h"

#include <algorithm>

namespace joyeer::typing {

// `Tree.Leaf`: a payloadless case of a generic enum, whose type arguments only context can supply.
bool TypeCheckingBuilder::isOpenCaseReference(const syntax::ExprPtr& value) const {
    if (value == nullptr) return false;
    if (value->kind == syntax::Kind::contextualCaseExpr) return true;
    if (value->kind != syntax::Kind::memberExpr) return false;
    const auto referenced = model->referencedSymbol(value);
    const auto* symbol = referenced.has_value() ? model->semanticModelValue->symbol(*referenced) : nullptr;
    return symbol != nullptr && symbol->kind == semantic::SymbolKind::enumCase &&
            (!symbol->callable.has_value() || symbol->callable->parameters.empty()) &&
            symbol->containingSymbol.has_value() && model->genericParameters(*symbol->containingSymbol) != nullptr;
}

// Spec 20.4.1: literals are unified after typed arguments; `nil`, `[]`, `[:]`, `.Case` say nothing.
TypeCheckingBuilder::ArgumentInformation TypeCheckingBuilder::argumentInformation(const syntax::ExprPtr& value) const {
    if (value == nullptr) return ArgumentInformation::none;
    switch (value->kind) {
        case syntax::Kind::parenthesizedExpr:
            return argumentInformation(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(value)->expression);
        case syntax::Kind::literalExpr: {
            const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(value);
            if (literal->literal != nullptr && literal->literal->kind == nilLiteral) {
                return ArgumentInformation::none;
            }
            return ArgumentInformation::literal;
        }
        case syntax::Kind::prefixExpr: {
            const auto prefix = std::static_pointer_cast<syntax::PrefixExprSyntax>(value);
            if (prefix->castTarget == nullptr && prefix->op != nullptr && prefix->op->kind == minus &&
                prefix->operand != nullptr && prefix->operand->kind == syntax::Kind::literalExpr) {
                return ArgumentInformation::literal;
            }
            return ArgumentInformation::typed;
        }
        case syntax::Kind::memberExpr:
            return isOpenCaseReference(value) ? ArgumentInformation::none : ArgumentInformation::typed;
        case syntax::Kind::arrayExpr: {
            const auto& elements = std::static_pointer_cast<syntax::ArrayExprSyntax>(value)->elements;
            return std::all_of(elements.begin(), elements.end(),
                               [&](const auto& element) { return isOpenCaseReference(element); })
                    ? ArgumentInformation::none : ArgumentInformation::typed;
        }
        case syntax::Kind::dictionaryExpr:
            return std::static_pointer_cast<syntax::DictionaryExprSyntax>(value)->entries.empty()
                    ? ArgumentInformation::none : ArgumentInformation::typed;
        case syntax::Kind::contextualCaseExpr:
            return ArgumentInformation::none;
        case syntax::Kind::callExpr: {
            const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(value);
            return call->callee != nullptr && call->callee->kind == syntax::Kind::contextualCaseExpr
                    ? ArgumentInformation::none : ArgumentInformation::typed;
        }
        default:
            return ArgumentInformation::typed;
    }
}

bool TypeCheckingBuilder::mentionsAny(TypeId type, const std::vector<semantic::SymbolId>& parameters) const {
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return false;
    if (record->kind == TypeKind::typeParameter) {
        return std::find(parameters.begin(), parameters.end(), record->symbol) != parameters.end();
    }
    const auto arguments = record->arguments;
    return std::any_of(arguments.begin(), arguments.end(),
            [&](TypeId argument) { return mentionsAny(argument, parameters); });
}

// Structural unification of a parameter type with an argument type (spec 20.4.1). A mismatch
// binds nothing; the ordinary argument check reports it afterwards.
void TypeCheckingBuilder::unify(TypeId parameter, TypeId argument, Inference& inference, SourceSpan span) {
    if (argument == model->typeContext.errorType()) return;
    const auto* record = model->typeContext.type(parameter);
    if (record == nullptr) return;
    if (record->kind == TypeKind::typeParameter &&
        std::find(inference.parameters.begin(), inference.parameters.end(), record->symbol) !=
                inference.parameters.end()) {
        const auto symbol = record->symbol;
        const auto bound = inference.bindings.find(symbol);
        if (bound == inference.bindings.end()) {
            inference.bindings.emplace(symbol, argument);
        } else if (bound->second != argument && !inference.conflicted.contains(symbol)) {
            inference.conflicted.insert(symbol);
            report(TypeCheckingDiagnosticId::conflictingTypeArgument, span,
                   "type parameter '" + symbolName(symbol) + "' is inferred as both '" +
                           model->typeContext.displayName(bound->second) + "' and '" +
                           model->typeContext.displayName(argument) + "'",
                   "every use of one type parameter must have one type (spec 20.4.1)");
        }
        return;
    }
    if (!mentionsAny(parameter, inference.parameters)) return;
    const auto* actual = model->typeContext.type(argument);
    if (actual == nullptr || actual->kind != record->kind || actual->symbol != record->symbol ||
        actual->arguments.size() != record->arguments.size() || actual->effects != record->effects) {
        return;
    }
    const auto parameterArguments = record->arguments;
    const auto argumentArguments = actual->arguments;
    for (size_t index = 0; index < parameterArguments.size(); ++index) {
        unify(parameterArguments[index], argumentArguments[index], inference, span);
    }
}

// Spec 20.12.1: after the non-literal arguments, the expected type of a construction binds the
// parameters still open, so `var b: Box<Double> = Box(value: 1)` is a `Box<Double>`. It never
// overrides what an argument determined, and one `?` around the expected type is looked through.
void TypeCheckingBuilder::bindFromExpectedType(
        const semantic::Symbol& targetSymbol,
        const std::vector<semantic::SymbolId>& parameters,
        std::optional<TypeId> expectedResult,
        Inference& inference) {
    if (!expectedResult.has_value() || !targetSymbol.containingSymbol.has_value()) return;
    const auto* expected = model->typeContext.type(*expectedResult);
    // Spec 5.4.1: constructing the optional role enum itself never looks through a layer.
    const bool constructsOptional = targetSymbol.containingSymbol == model->semanticModelValue->roleSymbol(semantic::LibraryRole::optional);
    if (!constructsOptional && model->typeContext.isOptional(expected) && expected->arguments.size() == 1) {
        expected = model->typeContext.type(expected->arguments[0]);
    }
    if (expected == nullptr || (expected->kind != TypeKind::structure && expected->kind != TypeKind::enumeration) ||
        expected->symbol != *targetSymbol.containingSymbol || expected->arguments.size() != parameters.size()) {
        return;
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (inference.bindings.contains(parameters[index])) continue;
        if (expected->arguments[index] == model->typeContext.errorType()) continue;
        inference.bindings.emplace(parameters[index], expected->arguments[index]);
        inference.fromExpected.insert(parameters[index]);
    }
}

// Infers the type arguments of a call of a generic function, checks their constraints, records
// the call, and returns the signature with the arguments substituted. Arguments checked here
// are marked in `checked`, so the caller does not check them again.
TypedCallableSignature TypeCheckingBuilder::inferGenericCall(
        const syntax::CallExprSyntax::Ptr& expression,
        semantic::SymbolId target,
        const semantic::Symbol& targetSymbol,
        const TypedCallableSignature& signature,
        const std::vector<semantic::SymbolId>& parameters,
        std::vector<std::optional<std::optional<TypeId>>>& checked,
        std::optional<TypeId> expectedResult) {
    Inference inference { parameters, {}, {} };
    std::vector<std::optional<size_t>> parameterIndices;
    for (size_t index = 0; index < expression->arguments.size(); ++index) {
        parameterIndices.push_back(
                parameterIndexForArgument(targetSymbol, *expression->arguments[index], index));
    }
    const auto parameterType = [&](size_t index) -> std::optional<TypeId> {
        const auto parameter = parameterIndices[index];
        if (!parameter.has_value() || *parameter >= signature.parameters.size()) return std::nullopt;
        return signature.parameters[*parameter];
    };
    std::vector<size_t> order;
    for (size_t index = 0; index < expression->arguments.size(); ++index) order.push_back(index);
    const auto floatingLiteral = [&](size_t index) {
        auto value = expression->arguments[index]->value;
        while (value != nullptr && value->kind == syntax::Kind::parenthesizedExpr) {
            value = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(value)->expression;
        }
        if (value != nullptr && value->kind == syntax::Kind::prefixExpr) {
            value = std::static_pointer_cast<syntax::PrefixExprSyntax>(value)->operand;
        }
        return value != nullptr && value->kind == syntax::Kind::literalExpr &&
                std::static_pointer_cast<syntax::LiteralExprSyntax>(value)->literal->kind == floatLiteral;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
        return floatingLiteral(left) && !floatingLiteral(right);
    });
    for (const auto pass : { ArgumentInformation::typed, ArgumentInformation::literal }) {
        if (pass == ArgumentInformation::literal) bindFromExpectedType(
                targetSymbol, parameters, expectedResult, inference);
        for (const auto index : order) {
            const auto& argument = expression->arguments[index];
            const auto type = parameterType(index);
            if (!type.has_value() || !mentionsAny(*type, parameters) ||
                argumentInformation(argument->value) != pass) {
                continue;
            }
            const auto expected = model->typeContext.substitute(*type, inference.bindings);
            const bool taskInput = targetSymbol.libraryRole == semantic::LibraryRole::taskStart &&
                    parameterIndices[index] == std::optional<size_t>(1) &&
                    !mentionsAny(expected, parameters);
            const auto actual = checkExpression(
                    argument->value,
                    taskInput || (pass == ArgumentInformation::literal && !mentionsAny(expected, parameters))
                            ? std::optional<TypeId>(expected) : std::optional<TypeId>());
            checked[index] = actual;
            // A literal checked against a type the expected type fixed may be assignable to it
            // without being it (`Box<Int?> = Box(value: 1)`); the argument check that follows decides.
            const bool fixedByExpectedType = pass == ArgumentInformation::literal &&
                    !mentionsAny(expected, parameters) &&
                    std::any_of(parameters.begin(), parameters.end(), [&](const auto parameter) {
                        return inference.fromExpected.contains(parameter) && mentionsAny(*type, { parameter });
                    });
            if (actual.has_value() && *actual != model->typeContext.neverType() &&
                !fixedByExpectedType && !taskInput) {
                unify(*type, *actual, inference, argument->value->span);
            }
        }
    }
    std::vector<TypeId> arguments;
    for (const auto parameter : parameters) {
        const auto bound = inference.bindings.find(parameter);
        if (bound != inference.bindings.end()) {
            arguments.push_back(inference.conflicted.contains(parameter)
                    ? model->typeContext.errorType() : bound->second);
            continue;
        }
        report(TypeCheckingDiagnosticId::cannotInferTypeParameter, expression->span,
               "cannot infer type parameter '" + symbolName(parameter) + "' of '" + targetSymbol.name +
                       "' from the arguments",
               "pass an argument whose type determines it; 'nil', '[]', '.Case' and the result type do not "
               "(spec 20.4.1)");
        arguments.push_back(model->typeContext.errorType());
    }
    std::unordered_map<semantic::SymbolId, TypeId> substitution;
    for (size_t index = 0; index < parameters.size(); ++index) {
        substitution.emplace(parameters[index], arguments[index]);
    }
    checkTypeArguments(expression, targetSymbol, parameters, arguments);
    // Constructing a generic type needs a layout, found from its type, not a function instance.
    if (targetSymbol.kind == semantic::SymbolKind::function) {
        model->genericCalls[*model->semanticModelValue->nodeId(expression)] = GenericCall {
            target, arguments, currentFunction, expression->span };
    }
    TypedCallableSignature result = signature;
    for (auto& parameter : result.parameters) parameter = model->typeContext.substitute(parameter, substitution);
    result.result = model->typeContext.substitute(result.result, substitution);
    return result;
}

void TypeCheckingBuilder::checkTypeArguments(
        const syntax::CallExprSyntax::Ptr& expression,
        const semantic::Symbol& targetSymbol,
        const std::vector<semantic::SymbolId>& parameters,
        const std::vector<TypeId>& arguments) {
    checkTypeArguments(expression->span, targetSymbol, parameters, arguments);
}

void TypeCheckingBuilder::checkPendingTypeArguments() {
    for (; checkedPendingTypeArguments < pendingTypeArguments.size(); ++checkedPendingTypeArguments) {
        const auto& pending = pendingTypeArguments[checkedPendingTypeArguments];
        const auto* symbol = symbolOf(pending.type);
        const auto* parameters = model->genericParameters(pending.type);
        if (symbol == nullptr || parameters == nullptr || parameters->size() != pending.arguments.size()) continue;
        checkTypeArguments(pending.span, *symbol, *parameters, pending.arguments);
    }
}

void TypeCheckingBuilder::checkTypeArguments(
        SourceSpan span,
        const semantic::Symbol& targetSymbol,
        const std::vector<semantic::SymbolId>& parameters,
        const std::vector<TypeId>& arguments) {
    // The role-bound enums take linear and `Never` arguments (decision 0024): an instance is linear
    // exactly when its payload is, and the declaration has no code that could drop one.
    if (targetSymbol.libraryRole == semantic::LibraryRole::optional ||
        targetSymbol.libraryRole == semantic::LibraryRole::result) {
        return;
    }
    const auto copyable = model->copyableProtocol();
    const auto* owner = targetSymbol.containingSymbol.has_value()
            ? symbolOf(*targetSymbol.containingSymbol) : nullptr;
    const auto role = targetSymbol.kind == semantic::SymbolKind::synthesizedInitializer && owner != nullptr
            ? owner->libraryRole : targetSymbol.libraryRole;
    const bool channel = role == semantic::LibraryRole::channelSender ||
            role == semantic::LibraryRole::channelReceiver;
    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == model->typeContext.errorType()) continue;
        if (targetSymbol.libraryRole == semantic::LibraryRole::channelCreate &&
            (argument == model->typeContext.neverType() || argument == model->typeContext.anyType() ||
             model->isLinear(argument))) {
            // The initializing output type occurrences already own the element diagnostics.
            continue;
        }
        const auto parameter = symbolName(parameters[index]);
        const auto argumentName = "'" + model->typeContext.displayName(argument) + "'";
        const auto* element = model->typeContext.type(argument);
        if (channel && element != nullptr &&
            (element->kind == TypeKind::never || element->kind == TypeKind::any)) {
            report(TypeCheckingDiagnosticId::channelElementType, span,
                   argumentName + " cannot be the element type of a channel",
                   "use a concrete type, or Void for a signal that carries no value");
            continue;
        }
        if (channel && model->isLinear(argument)) {
            report(TypeCheckingDiagnosticId::linearChannelElement, span,
                   argumentName + " is linear, so it cannot be the element type of a channel",
                   "a value still queued when the channel closes would be dropped; send the result of "
                   "'consume handle.join()' (or another value that can be destroyed) instead of the handle");
            continue;
        }
        if (argument == model->typeContext.neverType()) {
            report(TypeCheckingDiagnosticId::invalidTypeArgument, span,
                   "type parameter '" + parameter + "' of '" + targetSymbol.name + "' cannot be 'Never'",
                   "'Never' has no values, so a generic function cannot take or hold one (spec 2.9)");
            continue;
        }
        // A dictionary's linear key or value is reported once, as `linear-collection-element`.
        if (model->isLinear(argument) && !targetSymbol.acceptsLinearArguments &&
            targetSymbol.libraryRole != semantic::LibraryRole::dictType) {
            report(TypeCheckingDiagnosticId::linearTypeArgument, span,
                   "type parameter '" + parameter + "' of '" + targetSymbol.name + "' cannot be the linear type " +
                           argumentName,
                   "generic code may drop a value of a type parameter, and a linear value must be ended "
                   "(spec 4.1.1, 20.5)");
            continue;
        }
        for (const auto protocol : model->constraints(parameters[index])) {
            if (satisfies(argument, protocol)) continue;
            const bool isCopyable = copyable.has_value() && protocol == *copyable;
            std::optional<std::string> help;
            if (isCopyable) {
                if (const auto reason = model->noncopyableReason(argument)) help = *reason;
            } else if (protocol == model->equatableProtocol()) {
                // Every case is explained by what would give the type equality, or why nothing can.
                help = equatableHelp(argument, span).value_or("");
            } else if (model->typeContext.mentionsTypeParameter(argument)) {
                help = "inside a generic function, only a type parameter whose constraint lists '" +
                        symbolName(protocol) + "' satisfies it";
            } else if (model->typeContext.isOptional(argument) || model->typeContext.isResult(argument)) {
                // An optional or result type cannot be extended (decision 0024), so no extension can fix this.
                help = "an optional or result type cannot conform to '" + symbolName(protocol) +
                        "'; match the value first, as in 'match p { .Some(v) => show(value: v), .None => ... }'";
            } else {
                help = "declare 'extension " + model->typeContext.displayName(argument) + ": " +
                        symbolName(protocol) + " { ... }'";
            }
            report(TypeCheckingDiagnosticId::unsatisfiedConstraint, span,
                   "type " + argumentName + " does not satisfy '" + symbolName(protocol) +
                           "', required by type parameter '" + parameter + "' of '" + targetSymbol.name + "'",
                   help);
        }
    }
}

} // namespace joyeer::typing
