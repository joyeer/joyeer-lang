#include "builder.h"

namespace joyeer::typing {

// A type may name a constructor or qualify a member, but is not itself a value (spec 5.6.1).
bool TypeCheckingBuilder::rejectTypeAsValue(const syntax::ExprPtr& expression) {
    if (expression.get() == currentCallee || expression.get() == currentMemberBase) return false;
    const auto referenced = model->referencedSymbol(expression);
    const auto* symbol = referenced.has_value() ? model->semanticModelValue->symbol(*referenced) : nullptr;
    if (symbol == nullptr ||
        (symbol->kind != semantic::SymbolKind::structure &&
         symbol->kind != semantic::SymbolKind::enumeration &&
         symbol->kind != semantic::SymbolKind::builtinType &&
         symbol->kind != semantic::SymbolKind::typeParameter &&
         symbol->kind != semantic::SymbolKind::protocol)) return false;
    std::optional<std::string> help;
    if (symbol->kind == semantic::SymbolKind::structure) {
        help = "construct a value with its initializer, as in '" + symbol->name + "(...)'";
    } else if (symbol->kind == semantic::SymbolKind::protocol) {
        help = "declare a type parameter constrained to '" + symbol->name + "'";
    }
    report(TypeCheckingDiagnosticId::notAFunctionValue, expression->span,
           "'" + symbol->name + "' is a " +
                   (symbol->kind == semantic::SymbolKind::protocol ? "protocol" : "type") +
                   ", not a value or a function value", help);
    return true;
}

TypeId TypeCheckingBuilder::checkName(const syntax::NameExprSyntax::Ptr& expression) {
    if (expression.get() != currentCallee) {
        if (const auto value = functionValueType(expression)) return *value;
    }
    if (rejectTypeAsValue(expression)) return model->typeContext.errorType();
    return referencedValueType(expression);
}

// A function named where it is not called is a function value (spec 5.6.1). Returns nullopt
// when `expression` does not name a function, and the error type, with a diagnostic, when it
// names one that cannot be a value.
std::optional<TypeId> TypeCheckingBuilder::functionValueType(const syntax::ExprPtr& expression) {
    const auto referenced = model->referencedSymbol(expression);
    if (!referenced.has_value()) return std::nullopt;
    const auto* symbol = model->semanticModelValue->symbol(*referenced);
    if (symbol == nullptr) return std::nullopt;
    std::string reason;
    switch (symbol->kind) {
        case semantic::SymbolKind::function: break;
        case semantic::SymbolKind::builtinFunction:
            reason = "it is a builtin function";
            break;
        case semantic::SymbolKind::method:
            reason = "it is a method";
            break;
        default:
            return std::nullopt;
    }
    if (reason.empty()) {
        const auto* binding = model->nativeBinding(*referenced);
        if (binding != nullptr && binding->dataSymbol) {
            reason = "it is a '@c_data' declaration, which names an address and calls nothing";
        } else if (binding != nullptr && binding->kind == NativeBindingKind::cImport) {
            reason = "it is a '@c_extern' declaration, which uses the C calling convention";
        } else if (symbol->isRootModule && symbol->name == "main") {
            reason = "it is the program entry point";
        } else if (model->genericParameters(*referenced) != nullptr) {
            reason = "it is generic; a generic function can only be called (spec 20.4.1)";
        }
        if (reason.empty() && binding != nullptr &&
            !model->semanticModelValue->isRuntimeSource(expression->span)) {
            report(TypeCheckingDiagnosticId::unauthorizedNativeBinding, expression->span,
                   "a C binding can be used as a function value only in a runtime compilation unit",
                   "use a checked Joyeer wrapper");
            return model->typeContext.errorType();
        }
    }
    const auto* signature = model->callable(*referenced);
    if (reason.empty() && (signature == nullptr || !symbol->callable.has_value())) {
        return model->typeContext.errorType();
    }
    if (!reason.empty()) {
        report(TypeCheckingDiagnosticId::notAFunctionValue, expression->span,
               "'" + symbol->name + "' cannot be used as a function value: " + reason,
               symbol->kind == semantic::SymbolKind::method
                       ? std::optional<std::string>("call it, or write a top-level function that calls it and use that")
                       : symbol->kind == semantic::SymbolKind::function && symbol->name == "main"
                       ? std::optional<std::string>("move the work into another top-level function and use that")
                       : std::optional<std::string>("call it, or use a top-level Joyeer function"));
        return model->typeContext.errorType();
    }
    recordProcessLaunchUse(symbol, expression->span);
    std::vector<syntax::AccessEffect> effects;
    for (const auto& parameter : symbol->callable->parameters) effects.push_back(parameter.access);
    return model->typeContext.functionType(signature->parameters, effects, signature->result);
}

bool TypeCheckingBuilder::isFunctionType(TypeId type) const {
    const auto* record = model->typeContext.type(type);
    return record != nullptr && record->kind == TypeKind::function;
}

bool TypeCheckingBuilder::isOptionalFunction(TypeId type) const {
    const auto* record = model->typeContext.type(type);
    return model->typeContext.isOptional(record) && isFunctionType(record->arguments[0]);
}

TypeId TypeCheckingBuilder::referencedValueType(const syntax::NodePtr& expression) {
    const auto referenced = model->referencedSymbol(expression);
    if (!referenced.has_value()) return model->typeContext.errorType();
    const auto type = model->typeOf(*referenced);
    return type.value_or(model->typeContext.errorType());
}

bool TypeCheckingBuilder::isEnumCase(const semantic::Symbol* symbol) {
    return symbol != nullptr &&
            (symbol->kind == semantic::SymbolKind::enumCase ||
             symbol->kind == semantic::SymbolKind::builtinEnumCase);
}

const semantic::Symbol* TypeCheckingBuilder::referencedEnumCase(const syntax::NodePtr& expression) const {
    const auto id = model->referencedSymbol(expression);
    const auto* symbol = id.has_value() ? model->semanticModelValue->symbol(*id) : nullptr;
    return isEnumCase(symbol) ? symbol : nullptr;
}

TypeId TypeCheckingBuilder::checkMember(
        const syntax::MemberExprSyntax::Ptr& expression,
        std::optional<TypeId> expectedResult) {
    if (model->semanticModelValue->isModuleQualified(expression)) {
        if (expression.get() != currentCallee) {
            if (const auto value = functionValueType(expression)) return *value;
        }
        if (rejectTypeAsValue(expression)) return model->typeContext.errorType();
        return referencedValueType(expression);
    }
    const auto* savedMemberBase = currentMemberBase;
    currentMemberBase = expression->base.get();
    const auto base = checkExpression(expression->base).value_or(
            model->typeContext.errorType());
    currentMemberBase = savedMemberBase;
    auto member = model->referencedSymbol(expression);
    if (!member.has_value() && base != model->typeContext.errorType()) {
        member = lookupMember(
                base,
                expression->member == nullptr
                        ? std::string()
                        : expression->member->rawValue);
        if (member.has_value()) {
            if (!requireAccessible(*member, expression->span)) {
                return model->typeContext.errorType();
            }
            recordResolvedReference(expression, *member);
        }
    }
    if (!member.has_value()) {
        if (base != model->typeContext.errorType()) {
            const auto* baseRecord = model->typeContext.type(base);
            if (baseRecord != nullptr && baseRecord->kind == TypeKind::typeParameter) {
                markDeferredReferenceHandled(expression);
                const auto& protocols = model->constraints(baseRecord->symbol);
                std::string provided;
                for (const auto protocol : protocols) {
                    provided += (provided.empty() ? "" : ", ") + std::string("'") + symbolName(protocol) + "'";
                }
                report(TypeCheckingDiagnosticId::unknownMemberOnTypeParameter,
                       expression->member == nullptr ? expression->span : expression->member->span,
                       "type parameter '" + model->typeContext.displayName(base) + "' has no member '" +
                               (expression->member == nullptr ? std::string() : expression->member->rawValue) + "'",
                       provided.empty()
                               ? std::string("an unconstrained type parameter has no members; add a protocol to its constraint")
                               : "only the requirements of " + provided + " are available on it (spec 20.5)");
                return model->typeContext.errorType();
            }
            report(
                    TypeCheckingDiagnosticId::unknownMember,
                    expression->member == nullptr
                            ? expression->span
                            : expression->member->span,
                    "type '" + model->typeContext.displayName(base) +
                            "' has no member named '" +
                            (expression->member == nullptr
                                    ? std::string()
                                    : expression->member->rawValue) + "'");
        }
        return model->typeContext.errorType();
    }
    const auto* memberSymbol = model->semanticModelValue->symbol(*member);
    if (isEnumCase(memberSymbol) &&
        model->referencedSymbol(expression->base) != memberSymbol->containingSymbol) {
        report(TypeCheckingDiagnosticId::unknownMember, expression->span,
               "enum case '" + memberSymbol->name + "' must be qualified by its type, not a value");
        return model->typeContext.errorType();
    }
    if (expression.get() != currentCallee && memberSymbol != nullptr) {
        // Only top-level functions are values (spec 5.6.1); a member that must be called is not.
        const bool builtinMethod = memberSymbol->kind == semantic::SymbolKind::builtinMember &&
                memberSymbol->callable.has_value();
        const bool payloadCase = isEnumCase(memberSymbol) &&
                memberSymbol->callable.has_value() && !memberSymbol->callable->parameters.empty();
        if (memberSymbol->kind == semantic::SymbolKind::method || builtinMethod || payloadCase) {
            report(TypeCheckingDiagnosticId::notAFunctionValue, expression->span,
                   "'" + memberSymbol->name + "' cannot be used as a function value: " +
                           (payloadCase ? "it is an enum case; construct it with its payload"
                                        : builtinMethod ? "it is a builtin method" : "it is a method"),
                   "call it, or write a top-level function that calls it and use that");
            return model->typeContext.errorType();
        }
    }
    const auto declaredType = model->typeOf(*member).value_or(model->typeContext.errorType());
    // `Tree.Leaf` of a generic enum names no type arguments: the expected type supplies them
    // (spec 20.12.1), and without one they cannot be inferred.
    if (memberSymbol != nullptr && memberSymbol->kind == semantic::SymbolKind::enumCase &&
        (!memberSymbol->callable.has_value() || memberSymbol->callable->parameters.empty()) &&
        model->typeContext.mentionsTypeParameter(declaredType) &&
        memberSymbol->containingSymbol.has_value() &&
        model->genericParameters(*memberSymbol->containingSymbol) != nullptr &&
        expression->base != nullptr && model->semanticModelValue->referencedSymbol(expression->base).has_value() &&
        model->semanticModelValue->referencedSymbol(expression->base) == memberSymbol->containingSymbol) {
        const auto* expected = expectedResult.has_value() ? model->typeContext.type(*expectedResult) : nullptr;
        const bool constructsOptional = memberSymbol->containingSymbol == model->semanticModelValue->roleSymbol(semantic::LibraryRole::optional);
        if (!constructsOptional && model->typeContext.isOptional(expected) && expected->arguments.size() == 1) {
            expectedResult = expected->arguments[0];
            expected = model->typeContext.type(*expectedResult);
        }
        if (expected != nullptr && expected->symbol == *memberSymbol->containingSymbol &&
            !expected->arguments.empty()) {
            return *expectedResult;
        }
        report(TypeCheckingDiagnosticId::cannotInferTypeParameter, expression->span,
               "cannot infer the type arguments of '" + memberSymbol->name + "' from the case alone",
               "give the binding or result an annotation, such as 'let t: Tree<Int> = Tree.Leaf' (spec 20.12.1)");
        return model->typeContext.errorType();
    }
    return substituteForBase(declaredType, base, *member);
}

// The substitution a use of a member of `base` applies: the generic type's parameters replaced by
// the arguments of `base` (spec 20.12). Empty when `base` is not an instance of a generic type.
std::unordered_map<semantic::SymbolId, TypeId> TypeCheckingBuilder::baseSubstitution(
        TypeId base, const std::vector<semantic::SymbolId>* inScope) const {
    std::unordered_map<semantic::SymbolId, TypeId> substitution;
    const auto* record = model->typeContext.type(base);
    if (record == nullptr || record->arguments.empty() ||
        (record->kind != TypeKind::structure && record->kind != TypeKind::enumeration)) {
        return substitution;
    }
    const auto* parameters = inScope != nullptr ? inScope : model->genericParameters(record->symbol);
    if (parameters == nullptr || parameters->size() != record->arguments.size()) return substitution;
    for (size_t index = 0; index < parameters->size(); ++index) {
        substitution.emplace((*parameters)[index], record->arguments[index]);
    }
    return substitution;
}

const std::vector<semantic::SymbolId>* TypeCheckingBuilder::parametersOfMember(semantic::SymbolId member) const {
    const auto found = methodParameters.find(member);
    return found == methodParameters.end() ? nullptr : &found->second;
}

TypeId TypeCheckingBuilder::substituteForBase(TypeId type, TypeId base, semantic::SymbolId member) const {
    const auto substitution = baseSubstitution(base, parametersOfMember(member));
    return substitution.empty() ? type : model->typeContext.substitute(type, substitution);
}

std::optional<semantic::SymbolId> TypeCheckingBuilder::lookupMember(
        TypeId base,
        const std::string& name) const {
    const auto* baseType = model->typeContext.type(base);
    if (baseType == nullptr) return std::nullopt;
    const auto* typeSymbol = model->semanticModelValue->symbol(baseType->symbol);
    if (typeSymbol == nullptr || !typeSymbol->memberScope.has_value()) {
        return std::nullopt;
    }
    const auto* memberScope = model->semanticModelValue->scope(*typeSymbol->memberScope);
    if (memberScope == nullptr) return std::nullopt;
    const auto found = memberScope->values.find(name);
    if (found == memberScope->values.end()) return std::nullopt;
    return found->second;
}

bool TypeCheckingBuilder::requireAccessible(semantic::SymbolId target, SourceSpan use) {
    if (model->semanticModelValue->isAccessible(target, use)) return true;
    report(TypeCheckingDiagnosticId::inaccessibleDeclaration, use,
           "declaration '" + model->semanticModelValue->symbol(target)->name +
           "' is not accessible from this file");
    return false;
}

} // namespace joyeer::typing
