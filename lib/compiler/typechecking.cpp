#include "joyeer/compiler/typechecking.h"

#include <cassert>
#include <functional>
#include <sstream>
#include <utility>

namespace joyeer::typing {

namespace {

void combineHash(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

size_t TypeContext::TypeKeyHash::operator()(const TypeKey& key) const noexcept {
    size_t result = std::hash<int>()(static_cast<int>(key.kind));
    combineHash(result, std::hash<semantic::SymbolId>()(key.symbol));
    for (const auto argument : key.arguments) {
        combineHash(result, std::hash<TypeId>()(argument));
    }
    return result;
}

TypeContext::TypeContext(const semantic::SemanticModel& model): model(model) {
    errorTypeId = intern(TypeKind::error, semantic::invalidSymbolId);

    registerConcreteBuiltin("Void", TypeKind::voidType, voidTypeId);
    registerConcreteBuiltin("Never", TypeKind::never, neverTypeId);
    registerConcreteBuiltin("Any", TypeKind::any, anyTypeId);
    registerConcreteBuiltin("Int", TypeKind::integer, intTypeId);
    registerConcreteBuiltin("Bool", TypeKind::boolean, boolTypeId);
    registerConcreteBuiltin("String", TypeKind::string, stringTypeId);
    registerConcreteBuiltin("UInt8", TypeKind::uint8, uint8TypeId);

    registerGenericBuiltin("Array", TypeKind::array, 1);
    registerGenericBuiltin("Dict", TypeKind::dictionary, 2);
    registerGenericBuiltin("Optional", TypeKind::optional, 1);
    registerGenericBuiltin("Result", TypeKind::result, 2);
}

TypeId TypeContext::errorType() const {
    return errorTypeId;
}

TypeId TypeContext::voidType() const {
    return voidTypeId;
}

TypeId TypeContext::neverType() const {
    return neverTypeId;
}

TypeId TypeContext::anyType() const {
    return anyTypeId;
}

TypeId TypeContext::intType() const {
    return intTypeId;
}

TypeId TypeContext::boolType() const {
    return boolTypeId;
}

TypeId TypeContext::stringType() const {
    return stringTypeId;
}

TypeId TypeContext::uint8Type() const {
    return uint8TypeId;
}

TypeId TypeContext::arrayType(TypeId element) {
    const auto symbol = builtinSymbol("Array");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { element }).value_or(errorTypeId);
}

TypeId TypeContext::dictionaryType(TypeId key, TypeId value) {
    const auto symbol = builtinSymbol("Dict");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { key, value }).value_or(errorTypeId);
}

TypeId TypeContext::optionalType(TypeId wrapped) {
    const auto symbol = builtinSymbol("Optional");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { wrapped }).value_or(errorTypeId);
}

TypeId TypeContext::resultType(TypeId success, TypeId failure) {
    const auto symbol = builtinSymbol("Result");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { success, failure }).value_or(errorTypeId);
}

std::optional<TypeId> TypeContext::typeForSymbol(
        semantic::SymbolId symbolId,
        const std::vector<TypeId>& arguments) {
    const auto concrete = zeroArgumentTypes.find(symbolId);
    if (concrete != zeroArgumentTypes.end()) {
        if (!arguments.empty()) return std::nullopt;
        return concrete->second;
    }

    const auto generic = genericConstructors.find(symbolId);
    if (generic != genericConstructors.end()) {
        if (arguments.size() != generic->second.arity) return std::nullopt;
        for (const auto argument : arguments) {
            if (type(argument) == nullptr) return std::nullopt;
            if (argument == errorTypeId) return errorTypeId;
        }
        return intern(generic->second.kind, symbolId, arguments);
    }

    const auto* declaration = model.symbol(symbolId);
    if (declaration == nullptr || declaration->isInvalid || !arguments.empty()) {
        return std::nullopt;
    }

    TypeKind kind;
    switch (declaration->kind) {
        case semantic::SymbolKind::structure:
            kind = TypeKind::structure;
            break;
        case semantic::SymbolKind::enumeration:
            kind = TypeKind::enumeration;
            break;
        default:
            return std::nullopt;
    }

    const auto id = intern(kind, symbolId);
    zeroArgumentTypes.emplace(symbolId, id);
    return id;
}

std::optional<semantic::SymbolId> TypeContext::builtinSymbol(std::string_view name) const {
    const auto found = builtinSymbols.find(std::string(name));
    if (found == builtinSymbols.end()) return std::nullopt;
    return found->second;
}

std::optional<size_t> TypeContext::typeArity(semantic::SymbolId symbolId) const {
    if (zeroArgumentTypes.contains(symbolId)) return 0;
    const auto generic = genericConstructors.find(symbolId);
    if (generic != genericConstructors.end()) return generic->second.arity;

    const auto* declaration = model.symbol(symbolId);
    if (declaration != nullptr && !declaration->isInvalid &&
        (declaration->kind == semantic::SymbolKind::structure ||
         declaration->kind == semantic::SymbolKind::enumeration)) {
        return 0;
    }
    return std::nullopt;
}

const TypeRecord* TypeContext::type(TypeId id) const {
    if (id >= types.size()) return nullptr;
    return &types[id];
}

size_t TypeContext::size() const {
    return types.size();
}

std::string TypeContext::displayName(TypeId id) const {
    const auto* value = type(id);
    if (value == nullptr) return "<invalid-type>";

    switch (value->kind) {
        case TypeKind::error:
            return "<error>";
        case TypeKind::array:
            return "[" + displayName(value->arguments[0]) + "]";
        case TypeKind::dictionary:
            return "[" + displayName(value->arguments[0]) + ": " +
                    displayName(value->arguments[1]) + "]";
        case TypeKind::optional:
            return displayName(value->arguments[0]) + "?";
        case TypeKind::result:
            return "Result<" + displayName(value->arguments[0]) + ", " +
                    displayName(value->arguments[1]) + ">";
        default: {
            const auto* declaration = model.symbol(value->symbol);
            return declaration == nullptr ? "<invalid-type>" : declaration->name;
        }
    }
}

bool TypeContext::isAssignable(TypeId source, TypeId destination) const {
    if (source == errorTypeId || destination == errorTypeId) return true;
    if (source == destination || source == neverTypeId || destination == anyTypeId) return true;

    const auto* destinationType = type(destination);
    if (destinationType == nullptr || destinationType->kind != TypeKind::optional) {
        return false;
    }
    const auto* sourceType = type(source);
    if (sourceType != nullptr && sourceType->kind == TypeKind::optional) {
        return isAssignable(sourceType->arguments[0], destinationType->arguments[0]);
    }
    return isAssignable(source, destinationType->arguments[0]);
}

TypeId TypeContext::intern(
        TypeKind kind,
        semantic::SymbolId symbol,
        std::vector<TypeId> arguments) {
    TypeKey key { kind, symbol, std::move(arguments) };
    const auto found = internedTypes.find(key);
    if (found != internedTypes.end()) return found->second;

    const auto id = static_cast<TypeId>(types.size());
    types.push_back(TypeRecord { id, key.kind, key.symbol, key.arguments });
    internedTypes.emplace(std::move(key), id);
    return id;
}

semantic::SymbolId TypeContext::requireBuiltinSymbol(const std::string& name) {
    const auto* prelude = model.scope(model.preludeScope());
    assert(prelude != nullptr);
    const auto found = prelude->types.find(name);
    assert(found != prelude->types.end());
    builtinSymbols.emplace(name, found->second);
    return found->second;
}

void TypeContext::registerConcreteBuiltin(
        const std::string& name,
        TypeKind kind,
        TypeId& destination) {
    const auto symbol = requireBuiltinSymbol(name);
    destination = intern(kind, symbol);
    zeroArgumentTypes.emplace(symbol, destination);
}

void TypeContext::registerGenericBuiltin(
        const std::string& name,
        TypeKind kind,
        size_t arity) {
    const auto symbol = requireBuiltinSymbol(name);
    genericConstructors.emplace(symbol, GenericConstructor { kind, arity });
}

TypeCheckedModel::TypeCheckedModel(semantic::SemanticModel::Ptr semanticModel):
        semanticModelValue(std::move(semanticModel)),
        typeContext(*semanticModelValue) {
}

const semantic::SemanticModel::Ptr& TypeCheckedModel::semanticModel() const {
    return semanticModelValue;
}

TypeContext& TypeCheckedModel::types() {
    return typeContext;
}

const TypeContext& TypeCheckedModel::types() const {
    return typeContext;
}

std::optional<TypeId> TypeCheckedModel::typeOf(const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (!id.has_value()) return std::nullopt;
    const auto found = nodeTypes.find(*id);
    if (found == nodeTypes.end()) return std::nullopt;
    return found->second;
}

std::optional<TypeId> TypeCheckedModel::typeOf(semantic::SymbolId symbol) const {
    const auto found = symbolTypes.find(symbol);
    if (found == symbolTypes.end()) return std::nullopt;
    return found->second;
}

const TypedCallableSignature* TypeCheckedModel::callable(
        semantic::SymbolId symbol) const {
    const auto found = callables.find(symbol);
    if (found == callables.end()) return nullptr;
    return &found->second;
}

class TypeCheckingBuilder {
public:
    TypeCheckingResult build(const semantic::SemanticModel::Ptr& semanticModel) {
        assert(semanticModel != nullptr);
        model = TypeCheckedModel::Ptr(new TypeCheckedModel(semanticModel));

        const auto& root = semanticModel->root();
        if (root != nullptr) {
            for (const auto& item : root->items) resolveTopLevelDeclaration(item);
            for (const auto& item : root->items) checkTopLevelBody(item);
        }
        return TypeCheckingResult { model, std::move(diagnostics) };
    }

private:
    TypeCheckedModel::Ptr model;
    std::vector<TypeCheckingDiagnostic> diagnostics;
    std::optional<TypeId> currentReturnType;

    void report(TypeCheckingDiagnosticId id, SourceSpan span, std::string message) {
        diagnostics.push_back(TypeCheckingDiagnostic { id, span, std::move(message) });
    }

    void recordNodeType(const syntax::NodePtr& node, TypeId type) {
        const auto id = model->semanticModelValue->nodeId(node);
        if (id.has_value()) model->nodeTypes[*id] = type;
    }

    void recordDeclaredType(const syntax::NodePtr& node, TypeId type) {
        const auto symbol = model->semanticModelValue->declaredSymbol(node);
        if (symbol.has_value()) model->symbolTypes[*symbol] = type;
    }

    std::optional<TypeId> resolveType(const syntax::TypePtr& syntaxType) {
        if (syntaxType == nullptr) return std::nullopt;

        auto result = model->typeContext.errorType();
        switch (syntaxType->kind) {
            case syntax::Kind::errorType:
                break;
            case syntax::Kind::nominalType: {
                const auto nominal =
                        std::static_pointer_cast<syntax::NominalTypeSyntax>(syntaxType);
                std::vector<TypeId> arguments;
                arguments.reserve(nominal->arguments.size());
                for (const auto& argument : nominal->arguments) {
                    arguments.push_back(resolveType(argument).value_or(
                            model->typeContext.errorType()));
                }

                const auto symbol = model->semanticModelValue->referencedSymbol(syntaxType);
                if (!symbol.has_value()) break;
                const auto resolved = model->typeContext.typeForSymbol(*symbol, arguments);
                if (resolved.has_value()) {
                    result = *resolved;
                    break;
                }

                const auto arity = model->typeContext.typeArity(*symbol);
                const auto* declaration = model->semanticModelValue->symbol(*symbol);
                if (arity.has_value() && declaration != nullptr) {
                    report(
                            TypeCheckingDiagnosticId::invalidTypeArgumentCount,
                            syntaxType->span,
                            "type '" + declaration->name + "' expects " +
                                    std::to_string(*arity) + " type argument(s), but got " +
                                    std::to_string(arguments.size()));
                }
                break;
            }
            case syntax::Kind::arrayType: {
                const auto array = std::static_pointer_cast<syntax::ArrayTypeSyntax>(syntaxType);
                result = model->typeContext.arrayType(resolveType(array->element).value_or(
                        model->typeContext.errorType()));
                break;
            }
            case syntax::Kind::dictionaryType: {
                const auto dictionary =
                        std::static_pointer_cast<syntax::DictionaryTypeSyntax>(syntaxType);
                const auto key = resolveType(dictionary->key).value_or(
                        model->typeContext.errorType());
                const auto value = resolveType(dictionary->value).value_or(
                        model->typeContext.errorType());
                result = model->typeContext.dictionaryType(key, value);
                break;
            }
            case syntax::Kind::optionalType: {
                const auto optional =
                        std::static_pointer_cast<syntax::OptionalTypeSyntax>(syntaxType);
                result = model->typeContext.optionalType(resolveType(optional->wrapped).value_or(
                        model->typeContext.errorType()));
                break;
            }
            default:
                break;
        }

        recordNodeType(syntaxType, result);
        return result;
    }

    void resolveTopLevelDeclaration(const syntax::NodePtr& node) {
        if (node == nullptr) return;
        switch (node->kind) {
            case syntax::Kind::bindingDecl:
                resolveBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(node));
                break;
            case syntax::Kind::functionDecl:
                resolveFunction(std::static_pointer_cast<syntax::FunctionDeclSyntax>(node));
                break;
            case syntax::Kind::structDecl:
                resolveStructure(std::static_pointer_cast<syntax::StructDeclSyntax>(node));
                break;
            case syntax::Kind::enumDecl:
                resolveEnumeration(std::static_pointer_cast<syntax::EnumDeclSyntax>(node));
                break;
            default:
                break;
        }
    }

    void resolveBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto type = resolveType(declaration->annotation);
        if (type.has_value()) recordDeclaredType(declaration, *type);
    }

    void resolveFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        const auto function = model->semanticModelValue->declaredSymbol(declaration);
        if (!function.has_value()) return;

        TypedCallableSignature signature {
            semantic::CallableKind::function,
            true,
            {},
            model->typeContext.voidType(),
        };
        signature.parameters.reserve(declaration->parameters.size());
        for (const auto& parameter : declaration->parameters) {
            const auto type = resolveType(parameter->type).value_or(
                    model->typeContext.errorType());
            recordDeclaredType(parameter, type);
            signature.parameters.push_back(type);
        }
        if (declaration->returnType != nullptr) {
            signature.result = resolveType(declaration->returnType).value_or(
                    model->typeContext.errorType());
        }

        model->symbolTypes[*function] = signature.result;
        model->callables[*function] = std::move(signature);
    }

    void resolveStructure(const syntax::StructDeclSyntax::Ptr& declaration) {
        const auto structure = model->semanticModelValue->declaredSymbol(declaration);
        if (!structure.has_value()) return;
        const auto structureType = model->typeContext.typeForSymbol(*structure).value_or(
                model->typeContext.errorType());
        model->symbolTypes[*structure] = structureType;
        recordNodeType(declaration, structureType);

        TypedCallableSignature initializer {
            semantic::CallableKind::structureInitializer,
            true,
            {},
            structureType,
        };
        initializer.parameters.reserve(declaration->fields.size());
        for (const auto& field : declaration->fields) {
            const auto fieldType = resolveType(field->type).value_or(
                    model->typeContext.errorType());
            recordDeclaredType(field, fieldType);
            initializer.parameters.push_back(fieldType);
        }

        const auto* symbol = model->semanticModelValue->symbol(*structure);
        if (symbol != nullptr && symbol->synthesizedInitializer.has_value()) {
            model->symbolTypes[*symbol->synthesizedInitializer] = structureType;
            model->callables[*symbol->synthesizedInitializer] = std::move(initializer);
        }
    }

    void resolveEnumeration(const syntax::EnumDeclSyntax::Ptr& declaration) {
        const auto enumeration = model->semanticModelValue->declaredSymbol(declaration);
        if (!enumeration.has_value()) return;
        const auto enumerationType = model->typeContext.typeForSymbol(*enumeration).value_or(
                model->typeContext.errorType());
        model->symbolTypes[*enumeration] = enumerationType;
        recordNodeType(declaration, enumerationType);

        for (const auto& enumCase : declaration->cases) {
            const auto caseSymbol = model->semanticModelValue->declaredSymbol(enumCase);
            if (!caseSymbol.has_value()) continue;

            TypedCallableSignature signature {
                semantic::CallableKind::enumCase,
                enumCase->hasPayloadClause,
                {},
                enumerationType,
            };
            signature.parameters.reserve(enumCase->associatedTypes.size());
            for (const auto& associatedType : enumCase->associatedTypes) {
                signature.parameters.push_back(resolveType(associatedType->type).value_or(
                        model->typeContext.errorType()));
            }
            model->symbolTypes[*caseSymbol] = enumerationType;
            model->callables[*caseSymbol] = std::move(signature);
        }
    }

    bool requireAssignable(TypeId source, TypeId destination, SourceSpan span) {
        if (model->typeContext.isAssignable(source, destination)) return true;
        report(
                TypeCheckingDiagnosticId::typeMismatch,
                span,
                "cannot use value of type '" + model->typeContext.displayName(source) +
                        "' where '" + model->typeContext.displayName(destination) +
                        "' is required");
        return false;
    }

    std::optional<TypeId> declaredAnnotationType(
            const syntax::BindingDeclSyntax::Ptr& declaration) {
        if (declaration->annotation == nullptr) return std::nullopt;
        const auto symbol = model->semanticModelValue->declaredSymbol(declaration);
        if (symbol.has_value()) {
            const auto found = model->symbolTypes.find(*symbol);
            if (found != model->symbolTypes.end()) return found->second;
        }
        return resolveType(declaration->annotation);
    }

    void checkBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto annotation = declaredAnnotationType(declaration);
        std::optional<TypeId> initializer;
        if (declaration->initializer != nullptr) {
            initializer = checkExpression(declaration->initializer, annotation);
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

        const auto symbol = model->semanticModelValue->declaredSymbol(declaration);
        if (symbol.has_value()) model->symbolTypes[*symbol] = bindingType;
        recordNodeType(declaration, bindingType);
    }

    void checkTopLevelBody(const syntax::NodePtr& node) {
        if (node == nullptr) return;
        switch (node->kind) {
            case syntax::Kind::bindingDecl:
                checkBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(node));
                break;
            case syntax::Kind::functionDecl:
                {
                const auto function = std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
                const auto functionSymbol = model->semanticModelValue->declaredSymbol(function);
                const auto* signature = functionSymbol.has_value()
                    ? model->callable(*functionSymbol)
                    : nullptr;
                const auto previousReturnType = currentReturnType;
                currentReturnType = signature == nullptr
                    ? model->typeContext.errorType()
                    : signature->result;
                checkBlock(function->body);
                currentReturnType = previousReturnType;
                break;
                }
            case syntax::Kind::structDecl: {
                const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
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

    TypeId checkBlock(const syntax::BlockExprSyntax::Ptr& block) {
        if (block == nullptr) return model->typeContext.errorType();
        TypeId result = model->typeContext.voidType();
        for (const auto& item : block->items) {
            result = checkNode(item);
        }
        recordNodeType(block, result);
        return result;
    }

    TypeId checkNode(const syntax::NodePtr& node) {
        if (node == nullptr) return model->typeContext.errorType();
        if (node->kind == syntax::Kind::bindingDecl) {
            checkBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(node));
            return model->typeContext.voidType();
        }
        if (node->kind == syntax::Kind::whileStmt) {
            const auto statement = std::static_pointer_cast<syntax::WhileStmtSyntax>(node);
            const auto condition = checkExpression(statement->condition).value_or(
                    model->typeContext.errorType());
            requireAssignable(
                    condition,
                    model->typeContext.boolType(),
                    statement->condition->span);
            checkBlock(statement->body);
            recordNodeType(node, model->typeContext.voidType());
            return model->typeContext.voidType();
        }
        if (isExpressionKind(node->kind)) {
            return checkExpression(std::static_pointer_cast<syntax::ExprSyntax>(node)).value_or(
                    model->typeContext.errorType());
        }
        return model->typeContext.errorType();
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
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

    std::optional<TypeId> checkExpression(
            const syntax::ExprPtr& expression,
            std::optional<TypeId> expected = std::nullopt) {
        if (expression == nullptr) return std::nullopt;

        auto result = model->typeContext.errorType();
        switch (expression->kind) {
            case syntax::Kind::errorExpr:
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
                    std::static_pointer_cast<syntax::PrefixExprSyntax>(expression));
                break;
            case syntax::Kind::accessExpr:
                result = checkExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        expected).value_or(model->typeContext.errorType());
                break;
            case syntax::Kind::assignmentExpr: {
                const auto assignment =
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                const auto target = checkExpression(assignment->target);
                const auto value = checkExpression(assignment->value, target);
                if (target.has_value() && value.has_value()) {
                    requireAssignable(*value, *target, assignment->value->span);
                }
                result = model->typeContext.voidType();
                break;
            }
            case syntax::Kind::memberExpr: {
                const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
                checkExpression(member->base);
                result = referencedValueType(expression);
                break;
            }
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                checkExpression(call->callee);
                for (const auto& argument : call->arguments) checkExpression(argument->value);
                const auto target = model->semanticModelValue->callTarget(call);
                const auto* signature = target.has_value() ? model->callable(*target) : nullptr;
                if (signature != nullptr) result = signature->result;
                break;
            }
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
                const auto contextual =
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
                for (const auto& argument : contextual->arguments) checkExpression(argument->value);
                break;
            }
            case syntax::Kind::blockExpr:
                result = checkBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression));
                break;
            case syntax::Kind::ifExpr: {
                const auto conditional = std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                const auto condition = checkExpression(conditional->condition).value_or(
                    model->typeContext.errorType());
                requireAssignable(
                    condition,
                    model->typeContext.boolType(),
                    conditional->condition->span);
                const auto thenType = checkBlock(conditional->thenBranch);
                if (conditional->elseBranch == nullptr) {
                    result = model->typeContext.voidType();
                    break;
                }
                const auto elseType = checkExpression(conditional->elseBranch, expected).value_or(
                    model->typeContext.errorType());
                result = commonType(thenType, elseType, conditional->elseBranch->span);
                break;
            }
            case syntax::Kind::returnExpr: {
                const auto returned = std::static_pointer_cast<syntax::ReturnExprSyntax>(expression);
                const auto required = currentReturnType.value_or(model->typeContext.voidType());
                const auto actual = returned->value == nullptr
                    ? model->typeContext.voidType()
                    : checkExpression(returned->value, required).value_or(
                        model->typeContext.errorType());
                requireAssignable(
                    actual,
                    required,
                    returned->value == nullptr ? returned->span : returned->value->span);
                result = model->typeContext.neverType();
                break;
            }
            case syntax::Kind::matchExpr: {
                const auto match = std::static_pointer_cast<syntax::MatchExprSyntax>(expression);
                checkExpression(match->scrutinee);
                for (const auto& arm : match->arms) checkExpression(arm->body, expected);
                break;
            }
            case syntax::Kind::binaryExpr: {
                result = checkBinary(
                        std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
                break;
            }
            case syntax::Kind::subscriptExpr: {
                const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                checkExpression(subscript->base);
                checkExpression(subscript->index);
                break;
            }
            default:
                break;
        }

        recordNodeType(expression, result);
        return result;
    }

    TypeId checkLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        if (expression->literal == nullptr) return model->typeContext.errorType();
        switch (expression->literal->kind) {
            case decimalLiteral: return model->typeContext.intType();
            case booleanLiteral: return model->typeContext.boolType();
            case stringLiteral: return model->typeContext.stringType();
            case byteLiteral: return model->typeContext.uint8Type();
            case nilLiteral: {
                if (expected.has_value()) {
                    const auto* expectedType = model->typeContext.type(*expected);
                    if (expectedType != nullptr && expectedType->kind == TypeKind::optional) {
                        return *expected;
                    }
                }
                report(
                        TypeCheckingDiagnosticId::missingContextualType,
                        expression->span,
                        "'nil' requires an optional contextual type");
                return model->typeContext.errorType();
            }
            default:
                return model->typeContext.errorType();
        }
    }

    TypeId checkPrefix(const syntax::PrefixExprSyntax::Ptr& expression) {
        const auto operand = checkExpression(expression->operand).value_or(
                model->typeContext.errorType());
        if (operand == model->typeContext.errorType()) return operand;
        if (expression->op != nullptr && expression->op->kind == minus &&
            operand == model->typeContext.intType()) {
            return operand;
        }
        reportInvalidOperator(expression->op, { operand }, expression->span);
        return model->typeContext.errorType();
    }

    TypeId checkBinary(const syntax::BinaryExprSyntax::Ptr& expression) {
        const auto left = checkExpression(expression->left).value_or(
                model->typeContext.errorType());
        const auto right = checkExpression(expression->right).value_or(
                model->typeContext.errorType());
        if (left == model->typeContext.errorType() ||
            right == model->typeContext.errorType()) {
            return model->typeContext.errorType();
        }

        const auto op = expression->op == nullptr ? invalid : expression->op->kind;
        switch (op) {
            case plus:
                if (left == model->typeContext.intType() && left == right) return left;
                if (left == model->typeContext.stringType() && left == right) return left;
                break;
            case minus:
            case multiply:
                if (left == model->typeContext.intType() && left == right) return left;
                break;
            case less:
            case lessEqual:
            case greater:
            case greaterEqual:
                if (left == right &&
                    (left == model->typeContext.intType() ||
                     left == model->typeContext.uint8Type() ||
                     left == model->typeContext.stringType())) {
                    return model->typeContext.boolType();
                }
                break;
            case equalEqual:
            case notEqual:
                if (left == right) return model->typeContext.boolType();
                break;
            case andAnd:
                if (left == model->typeContext.boolType() && left == right) return left;
                break;
            default:
                break;
        }

        reportInvalidOperator(expression->op, { left, right }, expression->span);
        return model->typeContext.errorType();
    }

    void reportInvalidOperator(
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
        report(TypeCheckingDiagnosticId::invalidOperatorOperands, span, std::move(message));
    }

    TypeId commonType(TypeId left, TypeId right, SourceSpan mismatchSpan) {
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

    TypeId checkName(const syntax::NameExprSyntax::Ptr& expression) {
        return referencedValueType(expression);
    }

    TypeId referencedValueType(const syntax::NodePtr& expression) {
        const auto referenced = model->semanticModelValue->referencedSymbol(expression);
        if (!referenced.has_value()) return model->typeContext.errorType();
        const auto type = model->typeOf(*referenced);
        return type.value_or(model->typeContext.errorType());
    }

    TypeId checkArray(
            const syntax::ArrayExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        std::optional<TypeId> elementType;
        if (expected.has_value()) {
            const auto* expectedType = model->typeContext.type(*expected);
            if (expectedType != nullptr && expectedType->kind == TypeKind::array) {
                elementType = expectedType->arguments[0];
            }
        }
        if (expression->elements.empty() && !elementType.has_value()) {
            report(
                    TypeCheckingDiagnosticId::missingContextualType,
                    expression->span,
                    "empty array literal requires a contextual element type");
            return model->typeContext.errorType();
        }

        for (const auto& element : expression->elements) {
            const auto actual = checkExpression(element, elementType).value_or(
                    model->typeContext.errorType());
            if (!elementType.has_value()) {
                elementType = actual;
            } else {
                requireAssignable(actual, *elementType, element->span);
            }
        }
        return model->typeContext.arrayType(*elementType);
    }

    TypeId checkDictionary(
            const syntax::DictionaryExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        std::optional<TypeId> keyType;
        std::optional<TypeId> valueType;
        if (expected.has_value()) {
            const auto* expectedType = model->typeContext.type(*expected);
            if (expectedType != nullptr && expectedType->kind == TypeKind::dictionary) {
                keyType = expectedType->arguments[0];
                valueType = expectedType->arguments[1];
            }
        }
        if (expression->entries.empty() && (!keyType.has_value() || !valueType.has_value())) {
            report(
                    TypeCheckingDiagnosticId::missingContextualType,
                    expression->span,
                    "empty dictionary literal requires contextual key and value types");
            return model->typeContext.errorType();
        }

        for (const auto& entry : expression->entries) {
            const auto actualKey = checkExpression(entry->key, keyType).value_or(
                    model->typeContext.errorType());
            const auto actualValue = checkExpression(entry->value, valueType).value_or(
                    model->typeContext.errorType());
            if (!keyType.has_value()) keyType = actualKey;
            else requireAssignable(actualKey, *keyType, entry->key->span);
            if (!valueType.has_value()) valueType = actualValue;
            else requireAssignable(actualValue, *valueType, entry->value->span);
        }
        return model->typeContext.dictionaryType(*keyType, *valueType);
    }
};

TypeCheckingResult TypeChecker::check(
        const semantic::SemanticModel::Ptr& semanticModel) const {
    assert(semanticModel != nullptr);
    return TypeCheckingBuilder().build(semanticModel);
}

const char* typeKindName(TypeKind kind) {
    switch (kind) {
        case TypeKind::error: return "error";
        case TypeKind::voidType: return "void";
        case TypeKind::never: return "never";
        case TypeKind::any: return "any";
        case TypeKind::integer: return "integer";
        case TypeKind::boolean: return "boolean";
        case TypeKind::string: return "string";
        case TypeKind::uint8: return "uint8";
        case TypeKind::structure: return "structure";
        case TypeKind::enumeration: return "enumeration";
        case TypeKind::array: return "array";
        case TypeKind::dictionary: return "dictionary";
        case TypeKind::optional: return "optional";
        case TypeKind::result: return "result";
    }
    return "unknown";
}

const char* diagnosticName(TypeCheckingDiagnosticId id) {
    switch (id) {
        case TypeCheckingDiagnosticId::invalidTypeArgumentCount:
            return "type-checking.invalid-type-argument-count";
        case TypeCheckingDiagnosticId::typeMismatch:
            return "type-checking.type-mismatch";
        case TypeCheckingDiagnosticId::missingContextualType:
            return "type-checking.missing-contextual-type";
        case TypeCheckingDiagnosticId::invalidOperatorOperands:
            return "type-checking.invalid-operator-operands";
    }
    return "type-checking.unknown";
}

std::string dump(const std::vector<TypeCheckingDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length
            << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::typing