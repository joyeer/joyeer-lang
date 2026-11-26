#include "builder.h"

#include <algorithm>

namespace joyeer::typing {

// True for a function type, or a builtin container of one.
bool TypeCheckingBuilder::containsFunctionType(TypeId type) const {
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return false;
    if (model->isArrayRoleType(type)) return containsFunctionType(record->arguments[0]);
    if (model->typeContext.isDictionary(record)) {
        const auto arguments = record->arguments;
        return std::any_of(arguments.begin(), arguments.end(),
                           [this](TypeId argument) { return containsFunctionType(argument); });
    }
    switch (record->kind) {
        case TypeKind::function: return true;
        case TypeKind::enumeration:
            // Role-bound enums are looked through: an optional function value stays a function value.
            if (!model->typeContext.isOptional(record) && !model->typeContext.isResult(record)) return false;
            return std::any_of(record->arguments.begin(), record->arguments.end(),
                    [this](TypeId argument) { return containsFunctionType(argument); });
        default: return false;
    }
}

// The library dictionary's own generic body writes `Dict<K, V>` over its type parameters (decision 0027, Decision 4):
// the key rules do not apply to the dictionary's own key parameter in a bundled prelude, and to nothing else.
bool TypeCheckingBuilder::genericLibraryKey(TypeId key, SourceSpan span) const {
    const auto* record = model->typeContext.type(key);
    const auto dictionary = model->semanticModelValue->roleSymbol(semantic::LibraryRole::dictType);
    if (record == nullptr || record->kind != TypeKind::typeParameter || !dictionary.has_value() ||
        !model->semanticModelValue->isPreludeSource(span)) {
        return false;
    }
    const auto* declaration = model->semanticModelValue->symbol(*dictionary);
    return declaration != nullptr && std::find(declaration->typeParameters.begin(),
                                               declaration->typeParameters.end(),
                                               record->symbol) != declaration->typeParameters.end();
}

// A key outside the four admitted types (decision 0027, Decision 4), a function-type key and a type-parameter key
// are rejected where the dictionary type is written; a literal whose key type came from a context is not checked
// again.
void TypeCheckingBuilder::rejectInvalidDictionaryKey(TypeId key, SourceSpan span) {
    // One diagnostic per key: a function type, then a bare type parameter, then any other key outside the
    // admitted four (a type built from a parameter is outside them whatever the parameter is).
    if (key == model->typeContext.errorType() || key == model->typeContext.neverType()) return;
    if (containsFunctionType(key)) {
        report(TypeCheckingDiagnosticId::invalidFunctionType, span,
               "a function type cannot be a dictionary key: function values have no equality",
               "key the dictionary by a name or a number and store the function value as the value");
        return;
    }
    if (const auto* record = model->typeContext.type(key);
        record != nullptr && record->kind == TypeKind::typeParameter) {
        report(TypeCheckingDiagnosticId::invalidTypeArgument, span,
               "a dictionary key cannot be the type parameter type '" + model->typeContext.displayName(key) + "'",
               "keys are restricted to Int, Bool, UInt8 and String, and no protocol states that "
               "yet (spec 20.5)");
        return;
    }
    if (!admittedKeyKind(key)) {
        report(TypeCheckingDiagnosticId::invalidTypeArgument, span,
               "'" + model->typeContext.displayName(key) + "' cannot be a dictionary key",
               "keys are restricted to Int, Bool, UInt8 and String, closed until plan 0012 (spec 2.6, 20.5)");
    }
}

TypeId TypeCheckingBuilder::checkSubscript(const syntax::SubscriptExprSyntax::Ptr& expression) {
    const auto base = checkExpression(expression->base).value_or(
            model->typeContext.errorType());
    const auto* baseType = model->typeContext.type(base);
    if (baseType == nullptr || base == model->typeContext.errorType()) {
        checkExpression(expression->index);
        return model->typeContext.errorType();
    }

    TypeId indexType = model->typeContext.errorType();
    TypeId result = model->typeContext.errorType();
    switch (baseType->kind) {
        case TypeKind::string:
            indexType = model->typeContext.intType();
            result = model->typeContext.uint8Type();
            break;
        case TypeKind::structure:
            if (model->isArrayRoleType(base)) {
                // A library array subscripts like a builtin one (decision 0025).
                indexType = model->typeContext.intType();
                result = baseType->arguments[0];
                break;
            }
            if (model->typeContext.isDictionary(baseType)) {
                // A library dictionary subscripts by its key to its value (decision 0027).
                indexType = baseType->arguments[0];
                result = baseType->arguments[1];
                break;
            }
            [[fallthrough]];
        default:
            report(
                    TypeCheckingDiagnosticId::notSubscriptable,
                    expression->base->span,
                    "value of type '" + model->typeContext.displayName(base) +
                            "' cannot be subscripted");
            checkExpression(expression->index);
            return model->typeContext.errorType();
    }

    const auto actualIndex = checkExpression(expression->index, indexType).value_or(
            model->typeContext.errorType());
    requireAssignable(actualIndex, indexType, expression->index->span);
    return result;
}

// The four dictionary key types the runtime compares by kind (decision 0027, spec 20.5).
bool TypeCheckingBuilder::admittedKeyKind(TypeId type) const {
    const auto* record = model->typeContext.type(type);
    return record != nullptr && (record->kind == TypeKind::integer || record->kind == TypeKind::boolean ||
                                 record->kind == TypeKind::string || record->kind == TypeKind::uint8);
}

// `Never` has no values, so an array of it could only be empty and every operation on one would bind a value
// that cannot exist: it is rejected like `Never` in any other generic (decision 0026, revision 5).
TypeId TypeCheckingBuilder::rejectNeverElement(TypeId element, SourceSpan span, bool literal) {
    if (element != model->typeContext.neverType()) return element;
    report(TypeCheckingDiagnosticId::invalidTypeArgument, span,
           "the element type of an array cannot be 'Never'",
           literal ? "'Never' has no values (spec 2.9); give the literal a contextual element type, or put "
                     "an element that has a value first"
                   : "'Never' has no values, so an array of it could never hold one (spec 2.9)");
    return model->typeContext.errorType();
}

// The same rule for a dictionary's key or value type (decision 0026, A11).
TypeId TypeCheckingBuilder::rejectNeverDictionaryPart(TypeId type, SourceSpan span, const char* part, bool literal) {
    if (type != model->typeContext.neverType()) return type;
    report(TypeCheckingDiagnosticId::invalidTypeArgument, span,
           std::string("the ") + part + " type of a dictionary cannot be 'Never'",
           literal ? "'Never' has no values (spec 2.9); give the literal a contextual type, or put an entry "
                     "that has a value first"
                   : "'Never' has no values, so a dictionary of it could never hold an entry (spec 2.9)");
    return model->typeContext.errorType();
}

TypeId TypeCheckingBuilder::checkArray(
        const syntax::ArrayExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    std::optional<TypeId> elementType;
    if (expected.has_value()) {
        // An optional context (`[T]?`) gives the literal its element type too: one layer, as the value is
        // promoted through one.
        const auto* expectedType = model->typeContext.type(*expected);
        if (model->typeContext.isOptional(expectedType) && !expectedType->arguments.empty()) {
            expectedType = model->typeContext.type(expectedType->arguments[0]);
        }
        if (model->typeContext.isArray(expectedType)) {
            elementType = expectedType->arguments[0];
        }
    }
    // The annotation was already rejected (for example `[Never]`): no follow-on error for its literal.
    if (expression->elements.empty() && expected.has_value() && *expected == model->typeContext.errorType()) {
        return model->typeContext.errorType();
    }
    if (expression->elements.empty() && !elementType.has_value()) {
        report(
                TypeCheckingDiagnosticId::missingContextualType,
                expression->span,
                "empty array literal requires a contextual element type");
        return model->typeContext.errorType();
    }

    const bool annotationRejected = expected.has_value() && *expected == model->typeContext.errorType();
    const auto diagnosticsBefore = diagnostics.size();
    for (const auto& element : expression->elements) {
        permitMove(element);
        const auto actual = checkExpression(element, elementType).value_or(
                model->typeContext.errorType());
        requireNoImplicitCopy(element, actual);
        if (!elementType.has_value()) {
            elementType = actual;
        } else {
            requireAssignable(actual, *elementType, element->span);
        }
    }
    // An element type of `Never` is reported once: not again when the annotation was already rejected or an
    // element already produced an error (`[return, 1]`).
    if (*elementType == model->typeContext.neverType() &&
        (annotationRejected || diagnostics.size() != diagnosticsBefore)) {
        return model->typeContext.errorType();
    }
    return model->typeContext.arrayType(rejectNeverElement(*elementType, expression->span, true));
}

TypeId TypeCheckingBuilder::checkDictionary(
        const syntax::DictionaryExprSyntax::Ptr& expression,
        std::optional<TypeId> expected) {
    std::optional<TypeId> keyType;
    std::optional<TypeId> valueType;
    if (expected.has_value()) {
        // An optional context (`[K: V]?`) gives the literal its key and value types too: one layer, as the value
        // is promoted through one.
        const auto* expectedType = model->typeContext.type(*expected);
        if (model->typeContext.isOptional(expectedType) && !expectedType->arguments.empty()) {
            expectedType = model->typeContext.type(expectedType->arguments[0]);
        }
        if (model->typeContext.isDictionary(expectedType)) {
            keyType = expectedType->arguments[0];
            valueType = expectedType->arguments[1];
        }
    }
    // A key type from the context was checked where the context wrote it.
    const bool contextualKey = keyType.has_value();
    // The annotation was already rejected (for example `[String: Never]`): no follow-on error for its literal.
    const bool annotationRejected = expected.has_value() && *expected == model->typeContext.errorType();
    if (expression->entries.empty() && annotationRejected) return model->typeContext.errorType();
    if (expression->entries.empty() && (!keyType.has_value() || !valueType.has_value())) {
        report(
                TypeCheckingDiagnosticId::missingContextualType,
                expression->span,
                "empty dictionary literal requires contextual key and value types");
        return model->typeContext.errorType();
    }

    const auto diagnosticsBefore = diagnostics.size();
    for (const auto& entry : expression->entries) {
        permitMove(entry->value);
        const auto actualKey = checkExpression(entry->key, keyType).value_or(
                model->typeContext.errorType());
        const auto actualValue = checkExpression(entry->value, valueType).value_or(
                model->typeContext.errorType());
        requireNoImplicitCopy(entry->key, actualKey);
        requireNoImplicitCopy(entry->value, actualValue);
        if (!keyType.has_value()) keyType = actualKey;
        else requireAssignable(actualKey, *keyType, entry->key->span);
        if (!valueType.has_value()) valueType = actualValue;
        else requireAssignable(actualValue, *valueType, entry->value->span);
    }
    // `Never` is reported once: not again when the annotation was rejected or an entry already had an error.
    if ((*keyType == model->typeContext.neverType() || *valueType == model->typeContext.neverType()) &&
        (annotationRejected || diagnostics.size() != diagnosticsBefore)) {
        return model->typeContext.errorType();
    }
    const auto key = rejectNeverDictionaryPart(*keyType, expression->span, "key", true);
    const auto value = rejectNeverDictionaryPart(*valueType, expression->span, "value", true);
    if (!contextualKey) {
        rejectInvalidDictionaryKey(key, expression->entries.empty()
                ? expression->span : expression->entries.front()->key->span);
    }
    return model->typeContext.dictionaryType(key, value);
}

} // namespace joyeer::typing
