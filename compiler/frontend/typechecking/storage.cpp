#include "builder.h"

#include <cassert>
#include <utility>

namespace joyeer::typing {

std::optional<std::string> TypeCheckingBuilder::readOnlyHelp(const StorageAccess& storage) {
    if (storage.stringByte) {
        return "a String's bytes are read-only (spec 2.1.1): build a new String instead";
    }
    if (!storage.libraryReadOnly) return std::nullopt;
    if (storage.dictionaryCount) {
        return "a dictionary's 'count' changes only by inserting with '&d[key] = value'";
    }
    return "an array's 'count' changes only through the array's own methods, such as append or removeLast";
}

// Whether `expression`, through parentheses, is `s[i]` of a String: a byte, which is only ever read.
bool TypeCheckingBuilder::isStringByte(const syntax::ExprPtr& expression) const {
    auto current = expression;
    while (current != nullptr && current->kind == syntax::Kind::parenthesizedExpr) {
        current = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)->expression;
    }
    if (current == nullptr || current->kind != syntax::Kind::subscriptExpr) return false;
    const auto base = model->typeOf(std::static_pointer_cast<syntax::SubscriptExprSyntax>(current)->base);
    return base.has_value() && *base == model->typeContext.stringType();
}

// `consume s[i]` would move a byte out of a String, whose bytes are read-only (spec 2.1.1). Reported once per
// operand, whichever of the marker and the argument checks sees it first with its type known.
// Returns whether `operand` is a String byte, so a caller reports nothing else about consuming it.
bool TypeCheckingBuilder::rejectConsumedStringByte(const syntax::ExprPtr& operand) {
    if (!isStringByte(operand)) return false;
    if (!reportedStringByteConsumes.insert(operand.get()).second) return true;
    report(TypeCheckingDiagnosticId::invalidConsumeArgument, operand->span, "cannot consume a byte of a String",
           "a String's bytes are read-only (spec 2.1.1): copy the byte into a binding ('let byte = s[i]') and "
           "use or consume that");
    return true;
}

// Whether `expression`, through parentheses, is a subscript.
bool TypeCheckingBuilder::isSubscript(const syntax::ExprPtr& expression) {
    auto current = expression;
    while (current != nullptr && current->kind == syntax::Kind::parenthesizedExpr) {
        current = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)->expression;
    }
    return current != nullptr && current->kind == syntax::Kind::subscriptExpr;
}

TypeCheckingBuilder::StorageAccess TypeCheckingBuilder::analyzeStorage(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return StorageAccess { false, false, "<invalid>" };
    switch (expression->kind) {
        case syntax::Kind::nameExpr: {
            const auto referenced = model->referencedSymbol(expression);
            const auto* symbol = referenced.has_value()
                    ? model->semanticModelValue->symbol(*referenced)
                    : nullptr;
            if (symbol == nullptr) return StorageAccess { false, false, "<invalid>" };
            return StorageAccess {
                symbol->isMutable,
                symbol->kind == semantic::SymbolKind::parameter && symbol->isMutable,
                symbol->name,
            };
        }
        case syntax::Kind::memberExpr: {
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
            auto result = analyzeStorage(member->base);
            const auto referenced = model->referencedSymbol(member);
            const auto* symbol = referenced.has_value()
                    ? model->semanticModelValue->symbol(*referenced)
                    : nullptr;
            if (result.writable && (symbol == nullptr || !symbol->isMutable)) {
                result.writable = false;
                result.immutableName = symbol == nullptr ? "<invalid>" : symbol->name;
            } else if (result.writable && symbol->readableOutsideDeclaration &&
                       member->span.sourceId != symbol->span.sourceId) {
                // `count` of a library array is read-only outside its declaration (decision 0025).
                result.writable = false;
                result.immutableName = symbol->name;
                result.libraryReadOnly = true;
                if (const auto base = model->typeOf(member->base); base.has_value()) {
                    result.dictionaryCount = model->typeContext.isDictionary(model->typeContext.type(*base));
                }
            }
            return result;
        }
        case syntax::Kind::subscriptExpr: {
            const auto subscript =
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
            auto result = analyzeStorage(subscript->base);
            result.requiresMarker = true;
            // Every String byte is read-only, so a `let` base reports the byte, not the binding: making it
            // mutable would not help.
            if (isStringByte(expression)) {
                result.writable = false;
                result.stringByte = true;
            }
            return result;
        }
        case syntax::Kind::accessExpr:
            return analyzeStorage(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        case syntax::Kind::parenthesizedExpr:
            return analyzeStorage(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        default:
            return StorageAccess { false, false, "<expression>" };
    }
}

// `target op= value` reads the target, applies the binary operator, and
// stores the result, so the operator's result type must be the target type.
TypeId TypeCheckingBuilder::checkCompoundAssignment(const syntax::AssignmentExprSyntax::Ptr& assignment,
        std::optional<TypeId> target) {
    auto path = assignment->target;
    while (path != nullptr && (path->kind == syntax::Kind::accessExpr ||
                               path->kind == syntax::Kind::parenthesizedExpr)) {
        path = path->kind == syntax::Kind::accessExpr
                ? std::static_pointer_cast<syntax::AccessExprSyntax>(path)->operand
                : std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(path)->expression;
    }
    if (path != nullptr && path->kind == syntax::Kind::subscriptExpr) {
        const auto base = model->typeOf(
                std::static_pointer_cast<syntax::SubscriptExprSyntax>(path)->base);
        const auto* baseType = base.has_value() ? model->typeContext.type(*base) : nullptr;
        if (model->typeContext.isDictionary(baseType)) {
            report(TypeCheckingDiagnosticId::invalidOperatorOperands, assignment->span,
                   "compound assignment to a dictionary element is not supported; "
                   "read it with 'get' and assign the result");
            return model->typeContext.voidType();
        }
    }
    const auto op = compoundBaseOperator(assignment->op->kind);
    const bool isShift = op == shiftLeft || op == shiftRight;
    // Shift counts are independent of the target type; other operands share it.
    const auto value = isShift ? checkExpression(assignment->value)
                               : checkExpression(assignment->value, target);
    if (target.has_value() && value.has_value() &&
        *target != model->typeContext.errorType() &&
        *value != model->typeContext.errorType()) {
        const auto result = binaryResult(op, *target, *value);
        if (!result.has_value() || *result != *target) {
            reportInvalidOperator(assignment->op, { *target, *value }, assignment->span);
        }
    }
    return model->typeContext.voidType();
}

void TypeCheckingBuilder::checkAssignmentAccess(const syntax::ExprPtr& target) {
    const auto hasMarker = target != nullptr && target->kind == syntax::Kind::accessExpr;
    const auto storage = analyzeStorage(target);
    if (!storage.writable) {
        report(
                TypeCheckingDiagnosticId::assignmentToImmutable,
                target == nullptr ? SourceSpan {} : target->span,
                storage.stringByte ? std::string("cannot assign to a byte of a String")
                                   : "cannot assign through immutable binding or field '" +
                                             storage.immutableName + "'",
                readOnlyHelp(storage));
        return;
    }
    if (hasMarker != storage.requiresMarker) {
        report(
                TypeCheckingDiagnosticId::invalidAccessMarker,
                target->span,
                storage.requiresMarker
                        ? "assignment through an inout/initializing parameter or subscript requires '&'"
                        : "assignment to directly owned mutable storage must not use '&'");
    }
}

void TypeCheckingBuilder::checkCallArgumentAccess(
        const semantic::Symbol& target,
        size_t parameterIndex,
        const syntax::CallArgumentSyntax& argument) {
    assert(target.callable.has_value());
    const auto& parameters = target.callable->parameters;
    if (parameterIndex >= parameters.size()) return;
    checkArgumentAccess(parameters[parameterIndex].access, argument);
}

// The marker and the storage an argument needs for a parameter with effect `required`, shared by
// calls by name and calls through a function value (spec 4.2, 5.6.1).
void TypeCheckingBuilder::checkArgumentAccess(syntax::AccessEffect required, const syntax::CallArgumentSyntax& argument) {
    const auto marker = argument.accessMarker == nullptr
            ? syntax::AccessEffect::borrowing
            : argument.accessMarker->kind == kwConsume
                    ? syntax::AccessEffect::consuming
                    : syntax::AccessEffect::inout;
    const auto markerMatches = marker == required ||
            (marker == syntax::AccessEffect::inout &&
             required == syntax::AccessEffect::initializing);
    if (!markerMatches) {
        const auto consumeMismatch = marker == syntax::AccessEffect::consuming ||
                required == syntax::AccessEffect::consuming;
        const auto fix = accessMarkerFix(required, argument);
        report(
                required == syntax::AccessEffect::initializing
                        ? TypeCheckingDiagnosticId::invalidInitializingArgument
                        : consumeMismatch
                                ? TypeCheckingDiagnosticId::invalidConsumeArgument
                                : TypeCheckingDiagnosticId::invalidInoutArgument,
                argument.span,
                required == syntax::AccessEffect::consuming
                        ? "consuming argument requires 'consume' at the call site"
                        : required == syntax::AccessEffect::initializing
                                ? "initializing argument requires '&' at the call site"
                                : required == syntax::AccessEffect::inout
                                        ? "inout argument requires '&' at the call site"
                                        : marker == syntax::AccessEffect::consuming
                                                ? "non-consuming argument must not use 'consume'"
                                                : "non-inout argument must not use '&'",
                accessMarkerHelp(required, marker),
                fix);
        return;
    }
    if (required == syntax::AccessEffect::inout &&
        !analyzeStorage(argument.value).writable) {
        report(
                TypeCheckingDiagnosticId::invalidInoutArgument,
                argument.value->span,
                "inout argument must refer to mutable storage",
                readOnlyHelp(analyzeStorage(argument.value)));
    }
    if (required == syntax::AccessEffect::initializing &&
        !isInitializable(argument.value)) {
        report(
                TypeCheckingDiagnosticId::invalidInitializingArgument,
                argument.value->span,
                "initializing argument must be a whole mutable owning local or consuming parameter");
    }
    if (required == syntax::AccessEffect::consuming && !rejectConsumedStringByte(argument.value) &&
        !isConsumable(argument.value)) {
        report(
                TypeCheckingDiagnosticId::invalidConsumeArgument,
                argument.value->span,
                "consuming argument must be an owning local, consuming parameter, or temporary");
    }
}

std::optional<DiagnosticFixIt> TypeCheckingBuilder::accessMarkerFix(
        syntax::AccessEffect required,
        const syntax::CallArgumentSyntax& argument) const {
    std::string replacement;
    switch (required) {
        case syntax::AccessEffect::borrowing: replacement = ""; break;
        case syntax::AccessEffect::inout:
        case syntax::AccessEffect::initializing: replacement = "&"; break;
        case syntax::AccessEffect::consuming: replacement = "consume "; break;
    }
    if (argument.accessMarker != nullptr) {
        return DiagnosticFixIt {
            argument.accessMarker->span.offset,
            argument.accessMarker->span.length,
            std::move(replacement),
        };
    }
    return DiagnosticFixIt {
        argument.value->span.offset,
        0,
        std::move(replacement),
    };
}

std::string TypeCheckingBuilder::accessMarkerHelp(
        syntax::AccessEffect required,
        syntax::AccessEffect supplied) const {
    if (required == syntax::AccessEffect::borrowing) {
        return "remove the access marker from this borrowing argument";
    }
    const auto expected = required == syntax::AccessEffect::consuming
            ? "consume"
            : "&";
    if (supplied == syntax::AccessEffect::borrowing) {
        return "insert '" + std::string(expected) + "' before this argument";
    }
    const auto actual = supplied == syntax::AccessEffect::consuming
            ? "consume"
            : "&";
    return "replace '" + std::string(actual) + "' with '" + expected + "'";
}

bool TypeCheckingBuilder::storesItsArguments(const semantic::Symbol& target) {
    return target.callable.has_value() &&
            (target.callable->kind == semantic::CallableKind::structureInitializer ||
             target.callable->kind == semantic::CallableKind::enumCase);
}

// A place names existing storage; using one where an owned value is needed copies it.
// Anything else (a call result, a literal, a constructed value) is a temporary.
bool TypeCheckingBuilder::isPlaceExpression(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    switch (expression->kind) {
        case syntax::Kind::parenthesizedExpr:
            return isPlaceExpression(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                            ->expression);
        case syntax::Kind::accessExpr: {
            // `&x` still names the storage of `x`; only `consume x` moves it.
            const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
            if (access->marker != nullptr && access->marker->kind == kwConsume) return false;
            return isPlaceExpression(access->operand);
        }
        case syntax::Kind::subscriptExpr:
            return true;
        case syntax::Kind::nameExpr:
        case syntax::Kind::memberExpr: {
            // A local, a parameter, or a stored field; not an enum case or a function.
            const auto referenced = model->referencedSymbol(expression);
            const auto* symbol = referenced.has_value()
                    ? model->semanticModelValue->symbol(*referenced)
                    : nullptr;
            if (symbol == nullptr) return false;
            return symbol->kind == semantic::SymbolKind::binding ||
                    symbol->kind == semantic::SymbolKind::patternBinding ||
                    symbol->kind == semantic::SymbolKind::parameter ||
                    symbol->kind == semantic::SymbolKind::structureField;
        }
        default:
            return false;
    }
}

// True when `target` is `self` itself (through `&` and parentheses), not a field of it.
bool TypeCheckingBuilder::assignsWholeSelf(const syntax::ExprPtr& target) const {
    auto current = target;
    while (current != nullptr) {
        if (current->kind == syntax::Kind::parenthesizedExpr) {
            current = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)->expression;
        } else if (current->kind == syntax::Kind::accessExpr) {
            current = std::static_pointer_cast<syntax::AccessExprSyntax>(current)->operand;
        } else {
            break;
        }
    }
    if (current == nullptr || current->kind != syntax::Kind::nameExpr) return false;
    const auto referenced = model->referencedSymbol(current);
    const auto* symbol = referenced.has_value()
            ? model->semanticModelValue->symbol(*referenced)
            : nullptr;
    return symbol != nullptr && symbol->kind == semantic::SymbolKind::parameter &&
            symbol->name == "self";
}

bool TypeCheckingBuilder::consumesArgument(const syntax::CallArgumentSyntax& argument) {
    return argument.accessMarker != nullptr && argument.accessMarker->kind == kwConsume;
}

// `consume x` is a move wherever a value is stored: a binding, an assignment, an aggregate
// element. Elsewhere it is only an argument marker or a consuming receiver.
void TypeCheckingBuilder::permitMove(const syntax::ExprPtr& expression) {
    auto current = expression;
    while (current != nullptr && current->kind == syntax::Kind::parenthesizedExpr) {
        current = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)->expression;
    }
    if (current != nullptr && current->kind == syntax::Kind::accessExpr) {
        permittedMoves.insert(current.get());
    }
}

// An argument that a memberwise initializer, an enum case or `append` stores: a copy of a
// noncopyable place is an error, and `consume` moves an owning local or parameter instead.
void TypeCheckingBuilder::requireStoredArgument(
        const syntax::CallArgumentSyntax& argument,
        std::optional<TypeId> type) {
    if (!consumesArgument(argument)) {
        requireNoImplicitCopy(argument.value, type);
        return;
    }
    if (!rejectConsumedStringByte(argument.value) && !isConsumable(argument.value)) {
        report(
                TypeCheckingDiagnosticId::invalidConsumeArgument,
                argument.value->span,
                "consuming argument must be an owning local, consuming parameter, or temporary");
    }
}

// Records that the value of `node` is discarded (TypeCheckedModel::isValueDiscarded), looking
// through parentheses. Must run before `node` is checked, which reads the mark.
void TypeCheckingBuilder::markValueDiscarded(const syntax::NodePtr& node) {
    auto current = node;
    while (current != nullptr && isExpressionKind(current->kind)) {
        if (const auto id = model->semanticModelValue->nodeId(current); id.has_value()) {
            model->discardedValues.insert(*id);
        }
        if (current->kind != syntax::Kind::parenthesizedExpr) break;
        current = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)->expression;
    }
}

// Whether an `if` or `match` of this type stores its branch values into a result, which IR
// lowering does for every type but `Void` and `Never` unless the value is discarded: a branch
// that names a noncopyable place then copies it, wherever the expression appears (a borrowing
// argument or a member base as much as a binding). An error type has already been reported.
bool TypeCheckingBuilder::producesBranchValue(TypeId type) const {
    return type != model->typeContext.voidType() && type != model->typeContext.neverType() &&
            type != model->typeContext.errorType();
}

// The places a value-producing expression copies: the expression itself when it names
// storage, or the value of a block. The branches of an `if` or `match` are checked where the
// expression is checked, and the operand of `?` where the propagation is.
void TypeCheckingBuilder::collectCopiedPlaces(const syntax::ExprPtr& expression,
                         std::vector<syntax::ExprPtr>& places) const {
    if (expression == nullptr) return;
    switch (expression->kind) {
        case syntax::Kind::parenthesizedExpr:
            collectCopiedPlaces(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                            ->expression,
                    places);
            return;
        case syntax::Kind::blockExpr: {
            const auto block = std::static_pointer_cast<syntax::BlockExprSyntax>(expression);
            if (block->items.empty()) return;
            const auto last = std::dynamic_pointer_cast<syntax::ExprSyntax>(block->items.back());
            collectCopiedPlaces(last, places);
            return;
        }
        case syntax::Kind::ifExpr:
        case syntax::Kind::matchExpr:
            // A value-producing `if` or `match` checks its own branches, in every context
            // (see producesBranchValue), so its result is already an owned temporary here.
            return;
        case syntax::Kind::propagateExpr:
            return; // checkPropagation checks its owned operand independently of the result's context.
        default:
            if (isPlaceExpression(expression)) places.push_back(expression);
            return;
    }
}

// Reports the implicit copy of a noncopyable value at an owned context (decision 0015).
// `movesOwningName` lets a bare owning local or consuming parameter be moved instead,
// which is what `return` does with it.
void TypeCheckingBuilder::requireNoImplicitCopy(
        const syntax::ExprPtr& expression,
        std::optional<TypeId> type,
        bool movesOwningName) {
    if (expression == nullptr) return;
    std::vector<syntax::ExprPtr> places;
    collectCopiedPlaces(expression, places);
    for (const auto& place : places) {
        const auto placeType = place == expression ? type : model->typeOf(place);
        if (!placeType.has_value() || *placeType == model->typeContext.errorType()) continue;
        const auto reason = model->noncopyableReason(*placeType);
        if (!reason.has_value()) continue;
        if (movesOwningName && place == expression) {
            auto bare = place;
            while (bare->kind == syntax::Kind::parenthesizedExpr) {
                bare = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(bare)
                               ->expression;
            }
            if (bare->kind == syntax::Kind::nameExpr && isConsumable(bare)) continue;
        }
        report(
                TypeCheckingDiagnosticId::copyOfNoncopyable,
                place->span,
                "cannot copy a value of type '" + model->typeContext.displayName(*placeType) +
                        "': " + *reason,
                "move an owning local or consuming parameter with 'consume', borrow it, or "
                "build a new value");
    }
}

bool TypeCheckingBuilder::isConsumable(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isConsumable(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::memberExpr) {
        const auto* declaration = referencedEnumCase(expression);
        if (declaration != nullptr &&
            declaration->callable.has_value() && declaration->callable->parameters.empty()) {
            return true;
        }
    }
    const auto path = storagePath(expression);
    if (!path.has_value()) {
        return expression->kind != syntax::Kind::memberExpr &&
            expression->kind != syntax::Kind::subscriptExpr &&
            expression->kind != syntax::Kind::accessExpr;
    }
    const auto* symbol = model->semanticModelValue->symbol(path->root);
    if (symbol == nullptr) return false;
    if (symbol->kind == semantic::SymbolKind::binding ||
        symbol->kind == semantic::SymbolKind::patternBinding) {
        return true;
    }
    // A function named as a value is a temporary, so `consume square` is valid; like any
    // temporary passed to a consuming parameter it is still written with `consume` (spec 4.3).
    if (symbol->kind == semantic::SymbolKind::function && path->projections.empty()) return true;
    if (symbol->kind != semantic::SymbolKind::parameter ||
        !symbol->declaration.has_value()) {
        return false;
    }
    const auto& declaration = model->semanticModelValue->node(*symbol->declaration);
    return declaration != nullptr &&
            declaration->kind == syntax::Kind::parameterDecl &&
            std::static_pointer_cast<syntax::ParameterDeclSyntax>(declaration)
                    ->accessEffect() == syntax::AccessEffect::consuming;
}

bool TypeCheckingBuilder::isInitializable(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isInitializable(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                        ->expression);
    }
    if (expression->kind != syntax::Kind::nameExpr) return false;
    const auto referenced = model->referencedSymbol(expression);
    const auto* symbol = referenced.has_value()
            ? model->semanticModelValue->symbol(*referenced)
            : nullptr;
    if (symbol == nullptr || !symbol->isMutable) return false;
    if (symbol->kind == semantic::SymbolKind::binding) return true;
    if (symbol->kind != semantic::SymbolKind::parameter ||
        !symbol->declaration.has_value()) {
        return false;
    }
    const auto& declaration = model->semanticModelValue->node(*symbol->declaration);
    return declaration != nullptr &&
            declaration->kind == syntax::Kind::parameterDecl &&
        (std::static_pointer_cast<syntax::ParameterDeclSyntax>(declaration)
             ->accessEffect() == syntax::AccessEffect::consuming ||
         std::static_pointer_cast<syntax::ParameterDeclSyntax>(declaration)
             ->accessEffect() == syntax::AccessEffect::initializing);
}

// A place `borrow_address` may take (decision 0027): a local binding, a parameter (including `self`) or a field path
// of those; never a subscript, a computed member, a top-level constant or an enum case.
bool TypeCheckingBuilder::isNamedLocalPlace(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        return isNamedLocalPlace(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
    }
    if (expression->kind == syntax::Kind::nameExpr) {
        const auto symbol = model->referencedSymbol(expression);
        const auto* declared = symbol.has_value() ? model->semanticModelValue->symbol(*symbol) : nullptr;
        if (declared == nullptr) return false;
        if (declared->kind == semantic::SymbolKind::parameter ||
            declared->kind == semantic::SymbolKind::patternBinding) {
            return true;
        }
        const auto* scope = model->semanticModelValue->scope(declared->ownerScope);
        return declared->kind == semantic::SymbolKind::binding && scope != nullptr &&
                scope->kind != semantic::ScopeKind::file && scope->kind != semantic::ScopeKind::module &&
                scope->kind != semantic::ScopeKind::prelude;
    }
    if (expression->kind == syntax::Kind::memberExpr) {
        const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
        const auto symbol = model->referencedSymbol(member);
        const auto* declared = symbol.has_value() ? model->semanticModelValue->symbol(*symbol) : nullptr;
        return declared != nullptr && declared->kind == semantic::SymbolKind::structureField &&
                isNamedLocalPlace(member->base);
    }
    return false;
}

// The place of `borrow_address` or `place_address` has exactly the type the intrinsic names: assignability would
// let an `Int` place stand for an `Int?` layout (stage 2b review).
void TypeCheckingBuilder::requireExactPlaceType(std::optional<TypeId> actual, TypeId type, const std::string& name, SourceSpan span) {
    const auto error = model->typeContext.errorType();
    if (!actual.has_value() || *actual == type || *actual == error || type == error) return;
    if (!model->typeContext.isAssignable(*actual, type)) return;  // already reported as a type mismatch
    report(TypeCheckingDiagnosticId::typeMismatch, span,
           "the place of '" + name + "' has type '" + model->typeContext.displayName(*actual) +
                   "', not exactly '" + model->typeContext.displayName(type) + "'",
           "name the place's own type as the type argument");
}

} // namespace joyeer::typing
