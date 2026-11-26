#include "builder.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace joyeer::typing {

TypeId TypeCheckingBuilder::checkContextualCase(
        const syntax::ContextualCaseExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    if (!expected.has_value() || *expected == model->typeContext.errorType()) {
        for (const auto& argument : expression->arguments) checkExpression(argument->value);
        if (!expected.has_value()) {
            report(
                    TypeCheckingDiagnosticId::missingContextualType,
                    expression->span,
                    "enum case '." +
                            (expression->name == nullptr
                                    ? std::string()
                                    : expression->name->rawValue) +
                            "' requires a contextual enum type");
        }
        return model->typeContext.errorType();
    }

    const auto name = expression->name == nullptr
            ? std::string()
            : expression->name->rawValue;
    const auto caseSymbol = lookupMember(*expected, name);
    const auto signature = caseSymbol.has_value()
            ? enumCaseSignature(*caseSymbol, *expected)
            : std::optional<TypedCallableSignature>();
    if (!caseSymbol.has_value() || !signature.has_value()) {
        for (const auto& argument : expression->arguments) checkExpression(argument->value);
        report(
                TypeCheckingDiagnosticId::unknownEnumCase,
                expression->name == nullptr ? expression->span : expression->name->span,
                "type '" + model->typeContext.displayName(*expected) +
                        "' has no enum case named '" + name + "'");
        return model->typeContext.errorType();
    }

    if (!requireAccessible(*caseSymbol, expression->span)) {
        return model->typeContext.errorType();
    }
    recordResolvedReference(expression, *caseSymbol);
    recordResolvedCallTarget(expression, *caseSymbol);
    validateContextualCaseArguments(*expression, *caseSymbol, *signature);
    return *expected;
}

std::optional<TypedCallableSignature> TypeCheckingBuilder::enumCaseSignature(
        semantic::SymbolId caseSymbol,
        TypeId enumerationType) const {
    const auto* symbol = model->semanticModelValue->symbol(caseSymbol);
    if (symbol == nullptr ||
        (symbol->kind != semantic::SymbolKind::enumCase &&
         symbol->kind != semantic::SymbolKind::builtinEnumCase)) {
        return std::nullopt;
    }

    if (symbol->kind == semantic::SymbolKind::enumCase) {
        const auto* signature = model->callable(caseSymbol);
        if (signature == nullptr) return std::nullopt;
        if (signature->result == enumerationType) return *signature;
        // A case of a generic enum, seen from one of its instances (spec 20.12): the declaration's
        // parameters are replaced by the instance's arguments.
        const auto* declared = model->typeContext.type(signature->result);
        const auto* instance = model->typeContext.type(enumerationType);
        if (declared == nullptr || instance == nullptr || declared->symbol != instance->symbol ||
            declared->arguments.empty() || instance->arguments.size() != declared->arguments.size()) {
            return std::nullopt;
        }
        const auto substitution = baseSubstitution(enumerationType);
        TypedCallableSignature substituted = *signature;
        for (auto& parameter : substituted.parameters) {
            parameter = model->typeContext.substitute(parameter, substitution);
        }
        substituted.result = enumerationType;
        return substituted;
    }

    const auto* type = model->typeContext.type(enumerationType);
    if (type == nullptr || !symbol->containingSymbol.has_value() ||
        *symbol->containingSymbol != type->symbol || !symbol->callable.has_value()) {
        return std::nullopt;
    }

    TypedCallableSignature signature {
        semantic::CallableKind::enumCase,
        symbol->callable->acceptsArgumentClause,
        {},
        enumerationType,
    };
    if (type->kind == TypeKind::enumeration) {
        for (const auto& parameter : symbol->callable->parameters) {
            if (!parameter.type.has_value()) return std::nullopt;
            const auto payload = model->typeContext.typeForSymbol(*parameter.type);
            if (!payload.has_value()) return std::nullopt;
            signature.parameters.push_back(*payload);
        }
    } else {
        return std::nullopt;
    }
    return signature;
}

void TypeCheckingBuilder::validateContextualCaseArguments(
        const syntax::ContextualCaseExprSyntax& expression,
        semantic::SymbolId caseSymbol,
        const TypedCallableSignature& signature) {
    const auto* symbol = model->semanticModelValue->symbol(caseSymbol);
    assert(symbol != nullptr && symbol->callable.has_value());
    const auto& semanticSignature = *symbol->callable;

    if (expression.arguments.size() != signature.parameters.size() ||
        expression.hasPayloadClause != signature.acceptsArgumentClause) {
        report(
                TypeCheckingDiagnosticId::enumCaseArgumentMismatch,
                expression.span,
                "enum case '" + symbol->name + "' expects " +
                        std::to_string(signature.parameters.size()) +
                        " payload argument(s), but got " +
                        std::to_string(expression.arguments.size()));
    }

    for (size_t index = 0; index < expression.arguments.size(); ++index) {
        const auto& argument = expression.arguments[index];
        const auto expected = index < signature.parameters.size()
                ? std::optional<TypeId>(signature.parameters[index])
                : std::optional<TypeId>();
        if (index < semanticSignature.parameters.size()) {
            validateEnumArgumentLabel(
                    semanticSignature.parameters[index].label,
                    argument->label,
                    argument->span,
                    symbol->name);
        }
        permitMove(argument->value);
        const auto actual = checkExpression(argument->value, expected);
        if (argument->accessMarker != nullptr && !consumesArgument(*argument)) {
            report(TypeCheckingDiagnosticId::invalidAccessMarker, argument->span,
                   "an enum payload does not take an '&' access marker");
        }
        requireStoredArgument(*argument, actual);
        if (expected.has_value() && actual.has_value()) {
            requireAssignable(*actual, *expected, argument->value->span);
        }
    }
}

void TypeCheckingBuilder::validateEnumArgumentLabel(
        const std::optional<std::string>& expected,
        const Token::Ptr& actual,
        SourceSpan span,
        const std::string& caseName) {
    const auto matches = (!expected.has_value() && actual == nullptr) ||
            (expected.has_value() && actual != nullptr &&
             *expected == actual->rawValue);
    if (matches) return;
    report(
            TypeCheckingDiagnosticId::enumCaseArgumentMismatch,
            actual == nullptr ? span : actual->span,
            "payload label does not match enum case '" + caseName + "'");
}

TypeId TypeCheckingBuilder::checkMatch(
        const syntax::MatchExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    // `match consume x` moves the scrutinee into the arms.
    permitMove(expression->scrutinee);
    const auto scrutinee = checkExpression(expression->scrutinee).value_or(
            model->typeContext.errorType());
    if (const auto* scrutineeType = model->typeContext.type(scrutinee);
        scrutineeType != nullptr && scrutineeType->kind == TypeKind::typeParameter) {
        report(TypeCheckingDiagnosticId::typeMismatch, expression->scrutinee->span,
               "cannot match on a value of type parameter '" + model->typeContext.displayName(scrutinee) + "'",
               "a type parameter is opaque; call a requirement of its constraint instead (spec 20.5)");
    }
    std::vector<PatternCoverage> coverage;
    coverage.reserve(expression->arms.size());
    std::optional<TypeId> result;
    bool validPatterns = true;
    const auto discarded = model->isValueDiscarded(expression);

    for (const auto& arm : expression->arms) {
        const auto previousDiagnostics = diagnostics.size();
        coverage.push_back(checkPattern(arm->pattern, scrutinee));
        validPatterns &= diagnostics.size() == previousDiagnostics;
        if (discarded) markValueDiscarded(arm->body);
        const auto armType = checkExpression(arm->body, expected).value_or(
                model->typeContext.errorType());
        recordNodeType(arm, armType);
        result = result.has_value()
                ? std::optional<TypeId>(commonType(*result, armType, arm->body->span))
                : std::optional<TypeId>(armType);
    }

    if (validPatterns) checkMatchExhaustiveness(expression, scrutinee, coverage);
    requireOwnedScrutineeForMovedBindings(expression);
    if (!discarded && result.has_value() && producesBranchValue(*result)) {
        for (const auto& arm : expression->arms) {
            requireNoImplicitCopy(arm->body, model->typeOf(arm->body));
        }
    }
    return result.value_or(model->typeContext.voidType());
}

bool TypeCheckingBuilder::patternBindsNoncopyable(const syntax::PatternPtr& pattern) const {
    if (pattern == nullptr) return false;
    if (pattern->kind == syntax::Kind::bindingPattern) {
        const auto type = model->typeOf(pattern);
        return type.has_value() && model->noncopyableReason(*type).has_value();
    }
    if (pattern->kind != syntax::Kind::enumCasePattern) return false;
    for (const auto& argument :
         std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern)->arguments) {
        if (patternBindsNoncopyable(argument->pattern)) return true;
    }
    return false;
}

// A pattern that binds a noncopyable value takes it out of the scrutinee, so the scrutinee
// must be an owned temporary or `consume x`; matching a place would copy the value.
void TypeCheckingBuilder::requireOwnedScrutineeForMovedBindings(const syntax::MatchExprSyntax::Ptr& expression) {
    const auto binds = std::any_of(
            expression->arms.begin(), expression->arms.end(),
            [this](const auto& arm) { return patternBindsNoncopyable(arm->pattern); });
    if (!binds) return;
    for (const auto& arm : expression->arms) {
        // Binding the whole scrutinee would copy or move all of it; match its cases instead.
        if (arm->pattern != nullptr && arm->pattern->kind == syntax::Kind::bindingPattern &&
            patternBindsNoncopyable(arm->pattern)) {
            report(
                    TypeCheckingDiagnosticId::copyOfNoncopyable,
                    arm->pattern->span,
                    "a pattern cannot bind a noncopyable value as a whole",
                    "match its cases and bind the payloads, or consume the value in a call");
        }
    }
    auto scrutinee = expression->scrutinee;
    while (scrutinee != nullptr && scrutinee->kind == syntax::Kind::parenthesizedExpr) {
        scrutinee = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(scrutinee)->expression;
    }
    bool consumes = false;
    if (scrutinee != nullptr && scrutinee->kind == syntax::Kind::accessExpr) {
        const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(scrutinee);
        consumes = access->marker != nullptr && access->marker->kind == kwConsume;
        scrutinee = access->operand;
    }
    if (consumes || !isPlaceExpression(scrutinee)) return;
    report(
            TypeCheckingDiagnosticId::copyOfNoncopyable,
            expression->scrutinee->span,
            "a pattern binds a noncopyable value out of this place, which would copy it",
            "match on a temporary, or move the value in with 'match consume ...'");
}

TypeCheckingBuilder::PatternCoverage TypeCheckingBuilder::checkPattern(
        const syntax::PatternPtr& pattern,
        TypeId expected) {
    if (pattern == nullptr) return {};
    markDeferredReferenceHandled(pattern);
    PatternCoverage coverage;

    switch (pattern->kind) {
        case syntax::Kind::errorPattern:
            break;
        case syntax::Kind::unitPattern:
            requireAssignable(expected, model->typeContext.voidType(), pattern->span);
            coverage.catchesAll = expected == model->typeContext.voidType();
            break;
        case syntax::Kind::wildcardPattern:
            coverage.catchesAll = true;
            break;
        case syntax::Kind::bindingPattern: {
            const auto binding =
                    std::static_pointer_cast<syntax::BindingPatternSyntax>(pattern);
            recordDeclaredType(binding, expected);
            coverage.catchesAll = true;
            break;
        }
        case syntax::Kind::literalPattern: {
            const auto literal =
                    std::static_pointer_cast<syntax::LiteralPatternSyntax>(pattern);
            checkLiteralPatternType(literal->literal, expected, pattern->span);
            if (literal->literal != nullptr && literal->literal->kind == booleanLiteral) {
                coverage.booleanLiteral = literal->literal->rawValue == Literals::TRUE;
            }
            if (literal->literal != nullptr && literal->literal->kind == nilLiteral) {
                coverage.enumCase = optionalAbsentCase(expected);
            }
            break;
        }
        case syntax::Kind::enumCasePattern:
            coverage = checkEnumCasePattern(
                    std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern),
                    expected);
            break;
        default:
            break;
    }

    recordNodeType(pattern, expected);
    return coverage;
}

// A literal pattern compares a value of exactly the literal's type (spec 7.1.3, decision 0030).
// Unlike an expression it is neither promoted into an Optional nor converted to `Any`: lowering
// tests the scrutinee itself against the literal, so the two types must be the same.
void TypeCheckingBuilder::checkLiteralPatternType(const Token::Ptr& literal, TypeId expected, SourceSpan span) {
    const auto error = model->typeContext.errorType();
    if (expected == error) return;
    const auto expectedName = model->typeContext.displayName(expected);
    const auto* expectedType = model->typeContext.type(expected);
    if (literal != nullptr && literal->kind != nilLiteral &&
        model->typeContext.isOptional(expectedType)) {
        // Type the literal against the payload: it reports a literal the payload cannot hold,
        // and `.Some(...)` is suggested only where wrapping the literal would type-check.
        const auto payload = expectedType->arguments.front();
        if (payload == error) return;
        const auto actual = patternLiteralType(literal, payload);
        report(
                TypeCheckingDiagnosticId::typeMismatch,
                span,
                actual == error
                        ? "a literal pattern cannot match a value of type '" + expectedName + "'"
                        : "a literal pattern of type '" + model->typeContext.displayName(actual) +
                                "' cannot match a value of type '" + expectedName + "'",
                actual == payload
                        ? "a literal pattern is not promoted into an Optional; write it inside "
                          "'.Some(...)' to match a present value"
                        : "a literal pattern is not promoted into an Optional");
        return;
    }
    const auto actual = patternLiteralType(literal, expected);
    if (actual == expected || actual == error) return;
    const auto actualName = model->typeContext.displayName(actual);
    report(
            TypeCheckingDiagnosticId::typeMismatch,
            span,
            "a literal pattern of type '" + actualName + "' cannot match a value of type '" +
                    expectedName + "'",
            "expected '" + expectedName + "' but found '" + actualName + "'");
}

TypeId TypeCheckingBuilder::patternLiteralType(const Token::Ptr& literal, TypeId expected) {
    if (literal == nullptr) return model->typeContext.errorType();
    switch (literal->kind) {
        case decimalLiteral:
        case floatLiteral: return checkNumericLiteral(literal, expected);
        case booleanLiteral: return model->typeContext.boolType();
        case stringLiteral: return model->typeContext.stringType();
        case byteLiteral: return model->typeContext.uint8Type();
        case nilLiteral: {
            const auto* expectedType = model->typeContext.type(expected);
            if (model->typeContext.isOptional(expectedType)) return expected;
            // An error type was already reported where it arose.
            if (expected != model->typeContext.errorType()) reportNilPatternMismatch(literal->span, expected);
            return model->typeContext.errorType();
        }
        default:
            return model->typeContext.errorType();
    }
}

// A `nil` pattern matches only the prelude's Optional (decision 0024); reporting here keeps the failure at this
// stage rather than as an error type that IR lowering later reports as a missing return.
void TypeCheckingBuilder::reportNilPatternMismatch(SourceSpan span, TypeId expected) {
    // Only a shadow of Optional explains `nil`; a user `Result` gets the plain help.
    auto help = roleShadowHelp(expected, semantic::LibraryRole::optional);
    if (!help.has_value()) {
        const auto* record = model->typeContext.type(expected);
        const auto name = model->typeContext.displayName(expected);
        help = "'nil' matches only an Optional such as '" + name + "?'";
        if (record != nullptr && record->kind == TypeKind::enumeration) {
            *help += "; match a case of '" + name + "' with '.CaseName'";
        }
    }
    report(TypeCheckingDiagnosticId::typeMismatch, span,
           "a 'nil' pattern cannot match a value of type '" + model->typeContext.displayName(expected) + "'", help);
}

TypeCheckingBuilder::PatternCoverage TypeCheckingBuilder::checkEnumCasePattern(
        const syntax::EnumCasePatternSyntax::Ptr& pattern,
        TypeId expected) {
    const auto name = pattern->name == nullptr
            ? std::string()
            : pattern->name->rawValue;
    auto caseSymbol = model->referencedSymbol(pattern);
    if (!caseSymbol.has_value()) caseSymbol = lookupMember(expected, name);
    const auto signature = caseSymbol.has_value()
            ? enumCaseSignature(*caseSymbol, expected)
            : std::optional<TypedCallableSignature>();
    if (!caseSymbol.has_value() || !signature.has_value()) {
        report(
                TypeCheckingDiagnosticId::unknownEnumCase,
                pattern->name == nullptr ? pattern->span : pattern->name->span,
                "type '" + model->typeContext.displayName(expected) +
                        "' has no enum case named '" + name + "'");
        for (const auto& argument : pattern->arguments) {
            checkPattern(argument->pattern, model->typeContext.errorType());
        }
        return {};
    }

    if (!requireAccessible(*caseSymbol, pattern->span)) return {};
    recordResolvedReference(pattern, *caseSymbol);
    return PatternCoverage {
        false,
        caseSymbol,
        std::nullopt,
        validatePatternArguments(*pattern, *caseSymbol, *signature),
    };
}

std::vector<TypeCheckingBuilder::PatternCoverage> TypeCheckingBuilder::validatePatternArguments(
        const syntax::EnumCasePatternSyntax& pattern,
        semantic::SymbolId caseSymbol,
        const TypedCallableSignature& signature) {
    const auto* symbol = model->semanticModelValue->symbol(caseSymbol);
    assert(symbol != nullptr && symbol->callable.has_value());
    const auto& semanticSignature = *symbol->callable;

    if (pattern.arguments.size() != signature.parameters.size() ||
        pattern.hasPayloadClause != signature.acceptsArgumentClause) {
        report(
                TypeCheckingDiagnosticId::enumCaseArgumentMismatch,
                pattern.span,
                "enum case '" + symbol->name + "' expects " +
                        std::to_string(signature.parameters.size()) +
                        " payload pattern(s), but got " +
                        std::to_string(pattern.arguments.size()));
    }

    std::vector<PatternCoverage> payloads;
    payloads.reserve(pattern.arguments.size());
    for (size_t index = 0; index < pattern.arguments.size(); ++index) {
        const auto& argument = pattern.arguments[index];
        const auto payloadType = index < signature.parameters.size()
                ? signature.parameters[index]
                : model->typeContext.errorType();
        if (index < semanticSignature.parameters.size()) {
            validateEnumArgumentLabel(
                    semanticSignature.parameters[index].label,
                    argument->label,
                    argument->span,
                    symbol->name);
        }
        payloads.push_back(checkPattern(argument->pattern, payloadType));
        recordNodeType(argument, payloadType);
    }
    return payloads;
}

bool TypeCheckingBuilder::patternMatrixIsExhaustive(
        const std::vector<TypeId>& columns,
        const std::vector<PatternRow>& rows,
        size_t& remainingSpecializations,
        size_t depth) const {
    if (rows.empty()) return false;
    const auto isWildcard = [](const PatternCoverage* pattern) {
        return pattern == nullptr || pattern->catchesAll;
    };
    if (columns.empty() ||
        std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
            return std::all_of(row.begin(), row.end(), isWildcard);
        })) {
        return true;
    }

    // Keep wildcards symbolic instead of enumerating payload products.
    // Pathological matrices conservatively require a catch-all arm.
    if (remainingSpecializations == 0 || depth == 256) return false;
    --remainingSpecializations;

    const auto specialize = [&](std::optional<semantic::SymbolId> enumCase,
                                std::optional<bool> booleanLiteral,
                                const std::vector<TypeId>& payloadTypes) {
        auto specializedColumns = payloadTypes;
        specializedColumns.insert(
                specializedColumns.end(), columns.begin() + 1, columns.end());
        std::vector<PatternRow> specializedRows;
        for (const auto& row : rows) {
            const auto* pattern = row.front();
            const auto wildcard = isWildcard(pattern);
            if (!wildcard &&
                !(enumCase.has_value() && pattern->enumCase == enumCase) &&
                !(booleanLiteral.has_value() &&
                  pattern->booleanLiteral == booleanLiteral)) {
                continue;
            }

            PatternRow specialized;
            specialized.reserve(payloadTypes.size() + row.size() - 1);
            if (wildcard) {
                specialized.insert(specialized.end(), payloadTypes.size(), nullptr);
            } else {
                if (pattern->payloads.size() != payloadTypes.size()) continue;
                for (const auto& payload : pattern->payloads) {
                    specialized.push_back(&payload);
                }
            }
            specialized.insert(specialized.end(), row.begin() + 1, row.end());
            specializedRows.push_back(std::move(specialized));
        }
        return patternMatrixIsExhaustive(
                specializedColumns,
                specializedRows,
                remainingSpecializations,
                depth + 1);
    };

    const auto hasDefault = std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
        return isWildcard(row.front());
    });
    if (hasDefault && specialize(std::nullopt, std::nullopt, {})) return true;
    if (std::all_of(rows.begin(), rows.end(), [&](const auto& row) {
            return isWildcard(row.front());
        })) {
        return false;
    }

    const auto* type = model->typeContext.type(columns.front());
    if (type != nullptr && type->kind == TypeKind::boolean) {
        return specialize(std::nullopt, true, {}) &&
                specialize(std::nullopt, false, {});
    }

    const auto required = enumCases(columns.front());
    if (required.empty()) return false;
    for (const auto caseSymbol : required) {
        const auto signature = enumCaseSignature(caseSymbol, columns.front());
        if (!signature.has_value() ||
            !specialize(caseSymbol, std::nullopt, signature->parameters)) {
            return false;
        }
    }
    return true;
}

void TypeCheckingBuilder::checkMatchExhaustiveness(
        const syntax::MatchExprSyntax::Ptr& expression,
        TypeId scrutinee,
        const std::vector<PatternCoverage>& coverage) {
    if (scrutinee == model->typeContext.errorType()) return;

    std::vector<PatternRow> rows;
    rows.reserve(coverage.size());
    for (const auto& pattern : coverage) rows.push_back({ &pattern });
    size_t remainingSpecializations = 4096;
    if (!patternMatrixIsExhaustive({ scrutinee }, rows, remainingSpecializations)) {
        report(
                TypeCheckingDiagnosticId::nonExhaustiveMatch,
                expression->span,
                "match over '" + model->typeContext.displayName(scrutinee) +
                        "' is not exhaustive; add the missing cases or a wildcard arm");
    }
}

std::vector<semantic::SymbolId> TypeCheckingBuilder::enumCases(TypeId type) const {
    std::vector<semantic::SymbolId> result;
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return result;
    const auto* symbol = model->semanticModelValue->symbol(record->symbol);
    if (symbol == nullptr || !symbol->memberScope.has_value()) return result;
    const auto* members = model->semanticModelValue->scope(*symbol->memberScope);
    if (members == nullptr) return result;
    for (const auto& [name, memberId] : members->values) {
        static_cast<void>(name);
        const auto* member = model->semanticModelValue->symbol(memberId);
        if (member != nullptr &&
            (member->kind == semantic::SymbolKind::enumCase ||
             member->kind == semantic::SymbolKind::builtinEnumCase)) {
            result.push_back(memberId);
        }
    }
    // Symbol ids only grow, and name resolution declares a type's cases in source order (builtin enums in their
    // descriptor order), so sorting by id gives declaration order. The member
    // map's own order depends on the hash and on how often the scope table was copied, and the exhaustiveness
    // budget must not.
    std::sort(result.begin(), result.end());
    return result;
}

// The case a `nil` pattern matches: the Optional role's case without a payload (decision 0024), as IR lowering
// binds it.
std::optional<semantic::SymbolId> TypeCheckingBuilder::optionalAbsentCase(TypeId type) const {
    if (!model->typeContext.isOptional(type)) return std::nullopt;
    std::optional<semantic::SymbolId> absent;
    for (const auto caseSymbol : enumCases(type)) {
        const auto* symbol = model->semanticModelValue->symbol(caseSymbol);
        if (symbol == nullptr || !symbol->callable.has_value() || !symbol->callable->parameters.empty()) continue;
        if (absent.has_value()) return std::nullopt;
        absent = caseSymbol;
    }
    return absent;
}

} // namespace joyeer::typing
