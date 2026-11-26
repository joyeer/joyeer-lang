#include "builder.h"

#include <algorithm>
#include <cctype>

namespace joyeer::typing {

bool TypeCheckingBuilder::requireAssignable(TypeId source, TypeId destination, SourceSpan span) {
    if (model->typeContext.isAssignable(source, destination)) return true;
    report(
            TypeCheckingDiagnosticId::typeMismatch,
            span,
            "cannot use value of type '" + model->typeContext.displayName(source) +
                    "' where '" + model->typeContext.displayName(destination) +
                "' is required",
            "expected '" + model->typeContext.displayName(destination) +
                "' but found '" + model->typeContext.displayName(source) + "'");
    return false;
}

std::optional<TypeId> TypeCheckingBuilder::declaredAnnotationType(
        const syntax::BindingDeclSyntax::Ptr& declaration) {
    if (declaration->annotation == nullptr) return std::nullopt;
    const auto symbol = model->semanticModelValue->declaredSymbol(declaration);
    if (symbol.has_value()) {
        const auto found = model->symbolTypes.find(*symbol);
        if (found != model->symbolTypes.end()) return found->second;
    }
    return resolveType(declaration->annotation);
}

void TypeCheckingBuilder::checkBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
    const auto annotation = declaredAnnotationType(declaration);
    std::optional<TypeId> initializer;
    if (declaration->initializer != nullptr) {
        permitMove(declaration->initializer);
        initializer = checkExpression(declaration->initializer, annotation);
        requireNoImplicitCopy(declaration->initializer, initializer);
    }

    TypeId bindingType = model->typeContext.errorType();
    if (annotation.has_value()) {
        bindingType = *annotation;
        if (initializer.has_value()) {
            requireAssignable(*initializer, bindingType, declaration->initializer->span);
        }
    } else if (initializer.has_value()) {
        bindingType = *initializer;
    } else {
        report(
                TypeCheckingDiagnosticId::missingContextualType,
                declaration->span,
                "binding requires a type annotation or an initializer");
    }

    // A `Never` binding with no initializer could never be given a value.
    if (declaration->initializer == nullptr && bindingType == model->typeContext.neverType()) {
        report(TypeCheckingDiagnosticId::invalidTypeArgument, declaration->span,
               "a binding cannot have the type 'Never': it has no values",
               "'Never' is only the result type of a function that does not return (spec 2.9)");
    }

    // The parser rewrites `for x in source` into a hidden `$for<N>` copy of the
    // source that is later counted and subscripted. Reject a source that cannot
    // be, and poison the binding so the rewrite adds no follow-on errors.
    if (isForInSourceBinding(declaration) && initializer.has_value() &&
        *initializer != model->typeContext.errorType()) {
        const auto* sourceType = model->typeContext.type(*initializer);
        if (sourceType == nullptr || (!model->isArrayRoleType(*initializer) &&
                                      sourceType->kind != TypeKind::string)) {
            report(
                    TypeCheckingDiagnosticId::notIterable,
                    declaration->initializer->span,
                    "cannot iterate over a value of type '" +
                            model->typeContext.displayName(*initializer) +
                            "'; 'for' needs an array, a string, or an integer range "
                            "('a..<b' or 'a...b')");
            bindingType = model->typeContext.errorType();
        }
    }

    // Iterating an array of linear elements would drop them; there is no consuming iteration.
    if (isForInSourceBinding(declaration) && bindingType != model->typeContext.errorType()) {
        const auto* sourceType = model->typeContext.type(bindingType);
        if (sourceType != nullptr && model->isArrayRoleType(bindingType) &&
            sourceType->arguments.size() == 1 && model->isLinear(sourceType->arguments[0])) {
            report(TypeCheckingDiagnosticId::linearCollectionElement,
                   declaration->initializer == nullptr ? declaration->span
                                                       : declaration->initializer->span,
                   "cannot iterate over an array of linear elements",
                   "drain it with removeLast instead");
        }
    }
    if (bindingType != model->typeContext.errorType()) {
        requireNoLinearDictionary(bindingType, declaration->span);
    }

    const auto symbol = model->semanticModelValue->declaredSymbol(declaration);
    if (symbol.has_value()) model->symbolTypes[*symbol] = bindingType;
    recordNodeType(declaration, bindingType);
}

// `$for<N>`: the hidden source copy made by the parser's for-in rewrite
// (`$for<N>i` and the range temporaries carry a suffix after the digits).
bool TypeCheckingBuilder::isForInSourceBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
    if (declaration->name == nullptr) return false;
    const auto& name = declaration->name->rawValue;
    if (name.size() <= 4 || name.compare(0, 4, "$for") != 0) return false;
    return std::all_of(name.begin() + 4, name.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
}

// The type-level rules of linear types (decision 0015): the `linear` modifier is for
// toolchain-authorized sources, and a linear value has no `deinit` to run, so a type with a
// `deinit` may not be or hold one.
void TypeCheckingBuilder::checkLinearStructure(const syntax::StructDeclSyntax::Ptr& structure) {
    if (structure->isLinear &&
        !model->semanticModelValue->isRuntimeSource(structure->span) &&
        !model->semanticModelValue->isCoreSource(structure->span)) {
        report(TypeCheckingDiagnosticId::linearReserved, structure->linearSpan,
               "'linear' is reserved for toolchain-authorized sources");
    }
    if (structure->isLinear && structure->fields.empty()) {
        report(TypeCheckingDiagnosticId::linearStructWithoutFields, structure->span,
               "a linear struct needs at least one field: it ends by consuming its fields",
               "add a field, or use a different design");
    }
    for (const auto& field : structure->fields) {
        const auto fieldSymbol = model->semanticModelValue->declaredSymbol(field);
        const auto fieldType = fieldSymbol.has_value() ? model->typeOf(*fieldSymbol)
                                                       : std::optional<TypeId>();
        if (fieldType.has_value()) requireNoLinearDictionary(*fieldType, field->span);
    }
    if (structure->deinitializer == nullptr) return;
    const auto symbol = model->semanticModelValue->declaredSymbol(structure);
    const auto type = symbol.has_value() ? model->typeOf(*symbol) : std::optional<TypeId>();
    if (type.has_value() && model->isLinear(*type) && !model->isArrayRoleType(*type) &&
        !(symbol.has_value() && model->semanticModelValue->symbol(*symbol)->libraryRole ==
                semantic::LibraryRole::arrayType)) {
        report(TypeCheckingDiagnosticId::linearInDeinitType, structure->deinitializer->span,
               "a linear value is never destroyed implicitly, so '" +
                       (structure->name == nullptr ? std::string() : structure->name->rawValue) +
                       "' cannot declare 'deinit'",
               "remove the 'deinit', or end the linear field explicitly instead");
    }
}

// A Dict cannot hold a linear key or value: nothing could drain it.
void TypeCheckingBuilder::requireNoLinearDictionary(TypeId id, SourceSpan span) {
    const auto* type = model->typeContext.type(id);
    if (type == nullptr) return;
    const auto arguments = type->arguments;
    if (model->typeContext.isDictionary(type) &&
        std::any_of(arguments.begin(), arguments.end(),
                    [this](TypeId argument) { return model->isLinear(argument); })) {
        report(TypeCheckingDiagnosticId::linearCollectionElement, span,
               "a dictionary cannot hold a linear key or value ('" +
                       model->typeContext.displayName(id) + "')",
               "use an array and drain it with removeLast");
        return;
    }
    for (const auto argument : arguments) requireNoLinearDictionary(argument, span);
}

void TypeCheckingBuilder::checkTopLevelBody(const syntax::NodePtr& node) {
    if (node == nullptr) return;
    switch (node->kind) {
        case syntax::Kind::bindingDecl: {
            const auto binding = std::static_pointer_cast<syntax::BindingDeclSyntax>(node);
            checkBinding(binding);
            // Nothing would ever end a top-level linear constant.
            const auto symbol = model->semanticModelValue->declaredSymbol(binding);
            const auto type = symbol.has_value() ? model->typeOf(*symbol) : std::optional<TypeId>();
            if (type.has_value() && model->isLinear(*type)) {
                report(TypeCheckingDiagnosticId::linearTopLevelBinding, binding->span,
                       "a top-level constant cannot have the linear type '" +
                               model->typeContext.displayName(*type) + "'",
                       "declare it inside a function, where it can be ended");
            }
            break;
        }
        case syntax::Kind::functionDecl:
            checkFunctionBody(std::static_pointer_cast<syntax::FunctionDeclSyntax>(node));
            break;
        case syntax::Kind::enumDecl:
            for (const auto& method :
                 std::static_pointer_cast<syntax::EnumDeclSyntax>(node)->methods) {
                checkFunctionBody(method);
            }
            break;
        case syntax::Kind::extensionDecl:
            for (const auto& method :
                 std::static_pointer_cast<syntax::ExtensionDeclSyntax>(node)->methods) {
                checkFunctionBody(method);
            }
            break;
        case syntax::Kind::structDecl: {
            const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
            checkLinearStructure(structure);
            for (const auto& method : structure->methods) checkFunctionBody(method);
            if (structure->deinitializer != nullptr) {
                insideDeinit = true;
                checkFunctionBody(structure->deinitializer);
                insideDeinit = false;
            }
            for (const auto& field : structure->fields) {
                if (field->initializer == nullptr) continue;
                const auto symbol = model->semanticModelValue->declaredSymbol(field);
                const auto expected = symbol.has_value()
                        ? model->typeOf(*symbol)
                        : std::optional<TypeId>();
                const auto actual = checkExpression(field->initializer, expected);
                if (expected.has_value() && actual.has_value()) {
                    requireAssignable(*actual, *expected, field->initializer->span);
                }
            }
            break;
        }
        default:
            break;
    }
}

void TypeCheckingBuilder::checkFunctionBody(const syntax::FunctionDeclSyntax::Ptr& function) {
    const auto functionSymbol = model->semanticModelValue->declaredSymbol(function);
    const auto* signature = functionSymbol.has_value()
        ? model->callable(*functionSymbol)
        : nullptr;
    if (signature != nullptr) {
        for (const auto parameter : signature->parameters) {
            requireNoLinearDictionary(parameter, function->span);
        }
        requireNoLinearDictionary(signature->result, function->span);
    }
    const auto previousReturnType = currentReturnType;
    const auto previousLoopDepth = loopDepth;
    loopDepth = 0;
    const auto previousFunction = currentFunction;
    currentFunction = functionSymbol;
    currentReturnType = signature == nullptr
        ? model->typeContext.errorType()
        : signature->result;
    // A `Void` or `Never` body discards its trailing expression, as IR lowering does.
    checkBlock(function->body, std::nullopt,
            currentReturnType.has_value() &&
                    (*currentReturnType == model->typeContext.voidType() ||
                     *currentReturnType == model->typeContext.neverType()));
    // A trailing expression is the function's value; one that names a noncopyable place copies it.
    if (function->body != nullptr && !function->body->items.empty() &&
        currentReturnType.has_value() &&
        *currentReturnType != model->typeContext.voidType() &&
        *currentReturnType != model->typeContext.neverType()) {
        const auto last = std::dynamic_pointer_cast<syntax::ExprSyntax>(function->body->items.back());
        if (last != nullptr) requireNoImplicitCopy(last, model->typeOf(last));
    }
    currentReturnType = previousReturnType;
    loopDepth = previousLoopDepth;
    currentFunction = previousFunction;
}

// Every item but the last is a statement whose value is discarded; the last is discarded too when
// the block's own value is (`discarded`).
TypeId TypeCheckingBuilder::checkBlock(const syntax::BlockExprSyntax::Ptr& block,
        std::optional<TypeId> expected,
        bool discarded) {
    if (block == nullptr) return model->typeContext.errorType();
    TypeId result = model->typeContext.voidType();
    for (size_t index = 0; index < block->items.size(); ++index) {
        const auto isLast = index + 1 == block->items.size();
        if (!isLast || discarded) markValueDiscarded(block->items[index]);
        const auto itemType = checkNode(block->items[index], isLast ? expected : std::nullopt);
        if (result != model->typeContext.neverType()) result = itemType;
    }
    recordNodeType(block, result);
    return result;
}

TypeId TypeCheckingBuilder::checkNode(const syntax::NodePtr& node,
        std::optional<TypeId> expected) {
    if (node == nullptr) return model->typeContext.errorType();
    if (node->kind == syntax::Kind::bindingDecl) {
        checkBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(node));
        return model->typeContext.voidType();
    }
    if (node->kind == syntax::Kind::whileStmt) {
        const auto statement = std::static_pointer_cast<syntax::WhileStmtSyntax>(node);
        ++loopDepth;
        const auto condition = checkExpression(statement->condition).value_or(
                model->typeContext.errorType());
        requireAssignable(
                condition,
                model->typeContext.boolType(),
                statement->condition->span);
        checkBlock(statement->body, std::nullopt, true);
        --loopDepth;
        recordNodeType(node, model->typeContext.voidType());
        return model->typeContext.voidType();
    }
    if (node->kind == syntax::Kind::breakStmt ||
        node->kind == syntax::Kind::continueStmt) {
        if (loopDepth == 0) {
            report(
                    TypeCheckingDiagnosticId::invalidLoopControl,
                    node->span,
                    node->kind == syntax::Kind::breakStmt
                            ? "'break' requires an enclosing loop in the current function"
                            : "'continue' requires an enclosing loop in the current function");
        }
        recordNodeType(node, model->typeContext.neverType());
        return model->typeContext.neverType();
    }
    if (isExpressionKind(node->kind)) {
        return checkExpression(std::static_pointer_cast<syntax::ExprSyntax>(node), expected).value_or(
                model->typeContext.errorType());
    }
    return model->typeContext.errorType();
}

bool TypeCheckingBuilder::isExpressionKind(syntax::Kind kind) const {
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

} // namespace joyeer::typing
