#include "joyeer/compiler/typechecking.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <sstream>
#include <unordered_set>
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

std::optional<semantic::SymbolId> TypeCheckedModel::referencedSymbol(
        const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (id.has_value()) {
        const auto resolved = resolvedReferences.find(*id);
        if (resolved != resolvedReferences.end()) return resolved->second;
    }
    return semanticModelValue->referencedSymbol(node);
}

std::optional<semantic::SymbolId> TypeCheckedModel::callTarget(
        const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (id.has_value()) {
        const auto resolved = resolvedCallTargets.find(*id);
        if (resolved != resolvedCallTargets.end()) return resolved->second;
    }
    return semanticModelValue->callTarget(node);
}

class TypeCheckingBuilder {
public:
    TypeCheckingResult build(const semantic::SemanticModel::Ptr& semanticModel) {
        assert(semanticModel != nullptr);
        model = TypeCheckedModel::Ptr(new TypeCheckedModel(semanticModel));
        resolveBuiltinSignatures();

        const auto& root = semanticModel->root();
        if (root != nullptr) {
            for (const auto& item : root->items) resolveTopLevelDeclaration(item);
            for (const auto& item : root->items) checkTopLevelBody(item);
        }
        validateDeferredReferences();
        return TypeCheckingResult { model, std::move(diagnostics) };
    }

private:
    TypeCheckedModel::Ptr model;
    std::vector<TypeCheckingDiagnostic> diagnostics;
    std::optional<TypeId> currentReturnType;
    std::unordered_set<semantic::NodeId> handledDeferredReferences;

    void report(
            TypeCheckingDiagnosticId id,
            SourceSpan span,
            std::string message,
            std::optional<std::string> help = std::nullopt,
            std::optional<DiagnosticFixIt> fixIt = std::nullopt) {
        diagnostics.push_back(TypeCheckingDiagnostic {
            id,
            span,
            std::move(message),
            std::move(help),
            std::move(fixIt),
        });
    }

    void recordNodeType(const syntax::NodePtr& node, TypeId type) {
        const auto id = model->semanticModelValue->nodeId(node);
        if (id.has_value()) model->nodeTypes[*id] = type;
    }

    void recordDeclaredType(const syntax::NodePtr& node, TypeId type) {
        const auto symbol = model->semanticModelValue->declaredSymbol(node);
        if (symbol.has_value()) model->symbolTypes[*symbol] = type;
    }

    void recordResolvedReference(
            const syntax::NodePtr& node,
            semantic::SymbolId symbol) {
        const auto id = model->semanticModelValue->nodeId(node);
        if (id.has_value()) model->resolvedReferences[*id] = symbol;
    }

    void recordResolvedCallTarget(
            const syntax::NodePtr& node,
            semantic::SymbolId symbol) {
        const auto id = model->semanticModelValue->nodeId(node);
        if (id.has_value()) model->resolvedCallTargets[*id] = symbol;
    }

    void markDeferredReferenceHandled(const syntax::NodePtr& node) {
        if (node == nullptr || model->semanticModelValue->deferredReference(node) == nullptr) {
            return;
        }
        const auto id = model->semanticModelValue->nodeId(node);
        if (id.has_value()) handledDeferredReferences.insert(*id);
    }

    void validateDeferredReferences() {
        for (const auto& deferred : model->semanticModelValue->deferredReferences()) {
            if (handledDeferredReferences.contains(deferred.node)) continue;
            report(
                    TypeCheckingDiagnosticId::unresolvedReference,
                    deferred.span,
                    "type checker did not consume deferred reference '" +
                            deferred.name + "'");
        }
    }

    void resolveBuiltinSignatures() {
        const auto* prelude = model->semanticModelValue->scope(
                model->semanticModelValue->preludeScope());
        assert(prelude != nullptr);

        const auto print = prelude->values.find("print");
        if (print != prelude->values.end()) {
            model->symbolTypes[print->second] = model->typeContext.voidType();
            model->callables[print->second] = TypedCallableSignature {
                semantic::CallableKind::function,
                true,
                { model->typeContext.anyType() },
                model->typeContext.voidType(),
            };
        }

        const auto readFile = prelude->values.find("readFile");
        if (readFile != prelude->values.end()) {
            const auto result = model->typeContext.resultType(
                    model->typeContext.stringType(),
                    model->typeContext.intType());
            model->symbolTypes[readFile->second] = result;
            model->callables[readFile->second] = TypedCallableSignature {
                semantic::CallableKind::function,
                true,
                { model->typeContext.stringType() },
                result,
            };
        }

        for (const auto& [name, result] : std::array {
                 std::pair { std::string_view("byteToInt"), model->typeContext.intType() },
                 std::pair { std::string_view("byteToString"), model->typeContext.stringType() },
             }) {
            const auto conversion = prelude->values.find(std::string(name));
            if (conversion == prelude->values.end()) continue;
            model->symbolTypes[conversion->second] = result;
            model->callables[conversion->second] = TypedCallableSignature {
                semantic::CallableKind::function,
                true,
                { model->typeContext.uint8Type() },
                result,
            };
        }

        for (const auto& symbol : model->semanticModelValue->symbols()) {
            if (symbol.kind != semantic::SymbolKind::builtinMember ||
                !symbol.declaredType.has_value()) {
                continue;
            }
            const auto type = model->typeContext.typeForSymbol(*symbol.declaredType);
            if (type.has_value()) model->symbolTypes[symbol.id] = *type;
            if (symbol.name == "append" && symbol.callable.has_value()) {
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    { model->typeContext.anyType() },
                    model->typeContext.voidType(),
                };
            }
        }
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
            std::optional<TypeId> expected = std::nullopt,
            bool hasReceiverAccessMarker = false) {
        if (expression == nullptr) return std::nullopt;
        markDeferredReferenceHandled(expression);

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
                if (std::static_pointer_cast<syntax::AccessExprSyntax>(expression)
                        ->marker != nullptr &&
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)
                        ->marker->kind == kwConsume) {
                    report(
                        TypeCheckingDiagnosticId::invalidConsumeArgument,
                        expression->span,
                        "'consume' is only valid on an argument to a consuming parameter");
                }
                result = checkExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        expected,
                        true).value_or(model->typeContext.errorType());
                break;
            case syntax::Kind::assignmentExpr: {
                const auto assignment =
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                const auto target = checkExpression(assignment->target);
                checkAssignmentAccess(assignment->target);
                const auto value = checkExpression(assignment->value, target);
                if (target.has_value() && value.has_value()) {
                    requireAssignable(*value, *target, assignment->value->span);
                }
                result = model->typeContext.voidType();
                break;
            }
            case syntax::Kind::memberExpr:
                result = checkMember(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression));
                break;
            case syntax::Kind::callExpr:
                result = checkCall(
                        std::static_pointer_cast<syntax::CallExprSyntax>(expression),
                        hasReceiverAccessMarker);
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
                result = checkMatch(
                        std::static_pointer_cast<syntax::MatchExprSyntax>(expression),
                        expected);
                break;
            }
            case syntax::Kind::binaryExpr: {
                result = checkBinary(
                        std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
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
        const auto referenced = model->referencedSymbol(expression);
        if (!referenced.has_value()) return model->typeContext.errorType();
        const auto type = model->typeOf(*referenced);
        return type.value_or(model->typeContext.errorType());
    }

    TypeId checkMember(const syntax::MemberExprSyntax::Ptr& expression) {
        const auto base = checkExpression(expression->base).value_or(
                model->typeContext.errorType());
        auto member = model->referencedSymbol(expression);
        if (!member.has_value() && base != model->typeContext.errorType()) {
            member = lookupMember(
                    base,
                    expression->member == nullptr
                            ? std::string()
                            : expression->member->rawValue);
            if (member.has_value()) recordResolvedReference(expression, *member);
        }
        if (!member.has_value()) {
            if (base != model->typeContext.errorType()) {
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
        return model->typeOf(*member).value_or(model->typeContext.errorType());
    }

    std::optional<semantic::SymbolId> lookupMember(
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

    struct AccessPath {
        semantic::SymbolId root = semantic::invalidSymbolId;
        std::vector<std::optional<semantic::SymbolId>> projections;
    };

    struct CallAccess {
        AccessPath path;
        syntax::AccessEffect effect = syntax::AccessEffect::borrowing;
        SourceSpan span;
        bool sustained = true;
    };

    TypeId checkCall(
            const syntax::CallExprSyntax::Ptr& expression,
            bool hasReceiverAccessMarker = false) {
        const auto calleeType = checkExpression(expression->callee).value_or(
                model->typeContext.errorType());
        auto target = model->callTarget(expression);
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
            if (target.has_value()) recordResolvedCallTarget(expression, *target);
        }

        const auto* signature = target.has_value() ? model->callable(*target) : nullptr;
        const auto* semanticTarget = target.has_value()
                ? model->semanticModelValue->symbol(*target)
                : nullptr;
        const auto mutatingReceiver = semanticTarget != nullptr &&
                semanticTarget->kind == semantic::SymbolKind::builtinMember &&
                semanticTarget->isMutable;
        if (mutatingReceiver != hasReceiverAccessMarker) {
            report(
                    TypeCheckingDiagnosticId::invalidAccessMarker,
                    expression->callee->span,
                    mutatingReceiver
                            ? "mutating method call requires '&' on the receiver"
                            : "non-mutating call must not use '&' on the receiver");
        }

        std::vector<CallAccess> accesses;
        if (mutatingReceiver && expression->callee->kind == syntax::Kind::memberExpr) {
            const auto member =
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
            const auto storage = analyzeStorage(member->base);
            if (!storage.writable) {
                report(
                        TypeCheckingDiagnosticId::assignmentToImmutable,
                        member->base->span,
                        "cannot call mutating method through immutable binding '" +
                                storage.immutableName + "'");
            }
            const auto path = storagePath(member->base);
            if (path.has_value()) {
                accesses.push_back(CallAccess {
                    *path,
                    syntax::AccessEffect::inout,
                    member->base->span,
                    true,
                });
            }
        }

        std::optional<TypeId> arrayElementType;
        if (semanticTarget != nullptr && semanticTarget->name == "append" &&
            expression->callee->kind == syntax::Kind::memberExpr) {
            const auto member =
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
            const auto base = model->typeOf(member->base);
            const auto* baseType = base.has_value()
                    ? model->typeContext.type(*base)
                    : nullptr;
            if (baseType != nullptr && baseType->kind == TypeKind::array &&
                baseType->arguments.size() == 1) {
                arrayElementType = baseType->arguments[0];
            }
        }
        for (size_t index = 0; index < expression->arguments.size(); ++index) {
            const auto& argument = expression->arguments[index];
            const auto parameterIndex = semanticTarget == nullptr
                    ? std::optional<size_t>()
                    : parameterIndexForArgument(*semanticTarget, *argument, index);
            const auto expected = arrayElementType.has_value() &&
                                  parameterIndex == std::optional<size_t>(0)
                    ? arrayElementType
                    : signature != nullptr && parameterIndex.has_value() &&
                      *parameterIndex < signature->parameters.size()
                    ? std::optional<TypeId>(signature->parameters[*parameterIndex])
                    : std::optional<TypeId>();
            const auto actual = checkExpression(argument->value, expected);
            if (semanticTarget != nullptr && parameterIndex.has_value()) {
                checkCallArgumentAccess(*semanticTarget, *parameterIndex, *argument);
                const auto effect = semanticTarget->callable->parameters[*parameterIndex].access;
                collectCallArgumentAccess(*argument, effect, accesses);
            }
            if (expected.has_value() && actual.has_value()) {
                requireAssignable(*actual, *expected, argument->value->span);
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
                    "value '" +
                            (symbol == nullptr ? std::string("<expression>") : symbol->name) +
                            "' of type '" + model->typeContext.displayName(calleeType) +
                            "' is not callable");
        }
        return signature == nullptr ? model->typeContext.errorType() : signature->result;
    }

    void collectCallArgumentAccess(
            const syntax::CallArgumentSyntax& argument,
            syntax::AccessEffect effect,
            std::vector<CallAccess>& accesses,
            bool sustained = true) const {
        const auto path = storagePath(argument.value);
        if (path.has_value()) {
            accesses.push_back(CallAccess {
                *path,
                effect,
                argument.value->span,
                sustained,
            });
            collectSubscriptIndexAccesses(argument.value, accesses);
        } else {
            collectEvaluationAccesses(argument.value, accesses);
        }
    }

    std::optional<AccessPath> storagePath(const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return std::nullopt;
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return storagePath(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                            ->expression);
        }
        if (expression->kind == syntax::Kind::accessExpr) {
            return storagePath(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        }
        if (expression->kind == syntax::Kind::nameExpr) {
            const auto symbol = model->referencedSymbol(expression);
            if (!symbol.has_value()) return std::nullopt;
            return AccessPath { *symbol, {} };
        }
        if (expression->kind == syntax::Kind::memberExpr) {
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
            auto path = storagePath(member->base);
            if (!path.has_value()) return std::nullopt;
            const auto symbol = model->referencedSymbol(member);
            const auto* semanticMember = symbol.has_value()
                    ? model->semanticModelValue->symbol(*symbol)
                    : nullptr;
            if (semanticMember != nullptr &&
                semanticMember->kind == semantic::SymbolKind::structureField) {
                path->projections.push_back(*symbol);
            }
            return path;
        }
        if (expression->kind == syntax::Kind::subscriptExpr) {
            const auto subscript =
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
            auto path = storagePath(subscript->base);
            if (!path.has_value()) return std::nullopt;
            path->projections.push_back(std::nullopt);
            return path;
        }
        return std::nullopt;
    }

    void collectSubscriptIndexAccesses(
            const syntax::ExprPtr& expression,
            std::vector<CallAccess>& accesses) const {
        if (expression == nullptr) return;
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            collectSubscriptIndexAccesses(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                            ->expression,
                    accesses);
        } else if (expression->kind == syntax::Kind::accessExpr) {
            collectSubscriptIndexAccesses(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                    accesses);
        } else if (expression->kind == syntax::Kind::memberExpr) {
            collectSubscriptIndexAccesses(
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base,
                    accesses);
        } else if (expression->kind == syntax::Kind::subscriptExpr) {
            const auto subscript =
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
            collectSubscriptIndexAccesses(subscript->base, accesses);
            collectEvaluationAccesses(subscript->index, accesses);
        }
    }

    void collectEvaluationAccesses(
            const syntax::ExprPtr& expression,
            std::vector<CallAccess>& accesses) const {
        if (expression == nullptr) return;
        const auto path = storagePath(expression);
        if (path.has_value()) {
            accesses.push_back(CallAccess {
                *path,
                syntax::AccessEffect::borrowing,
                expression->span,
                false,
            });
            collectSubscriptIndexAccesses(expression, accesses);
            return;
        }
        switch (expression->kind) {
            case syntax::Kind::parenthesizedExpr:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                                ->expression,
                        accesses);
                break;
            case syntax::Kind::prefixExpr:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand,
                        accesses);
                break;
            case syntax::Kind::accessExpr:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        accesses);
                break;
            case syntax::Kind::binaryExpr: {
                const auto binary = std::static_pointer_cast<syntax::BinaryExprSyntax>(expression);
                collectEvaluationAccesses(binary->left, accesses);
                collectEvaluationAccesses(binary->right, accesses);
                break;
            }
            case syntax::Kind::assignmentExpr: {
                const auto assignment =
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                const auto target = storagePath(assignment->target);
                if (target.has_value()) {
                    accesses.push_back(CallAccess {
                        *target,
                        syntax::AccessEffect::inout,
                        assignment->target->span,
                        false,
                    });
                }
                collectEvaluationAccesses(assignment->value, accesses);
                break;
            }
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                const auto target = model->callTarget(call);
                const auto* symbol = target.has_value()
                        ? model->semanticModelValue->symbol(*target)
                        : nullptr;
                const auto mutatingReceiver = symbol != nullptr &&
                        symbol->kind == semantic::SymbolKind::builtinMember &&
                        symbol->isMutable && call->callee->kind == syntax::Kind::memberExpr;
                if (mutatingReceiver) {
                    const auto receiver = storagePath(
                            std::static_pointer_cast<syntax::MemberExprSyntax>(call->callee)->base);
                    if (receiver.has_value()) {
                        accesses.push_back(CallAccess {
                            *receiver,
                            syntax::AccessEffect::inout,
                            call->callee->span,
                            false,
                        });
                    }
                }
                for (size_t index = 0; index < call->arguments.size(); ++index) {
                    const auto& argument = call->arguments[index];
                    const auto parameterIndex = symbol == nullptr
                            ? std::optional<size_t>()
                            : parameterIndexForArgument(*symbol, *argument, index);
                    const auto effect = symbol != nullptr &&
                                        symbol->callable.has_value() &&
                                        parameterIndex.has_value()
                            ? symbol->callable->parameters[*parameterIndex].access
                            : syntax::AccessEffect::borrowing;
                    collectCallArgumentAccess(*argument, effect, accesses, false);
                }
                break;
            }
            case syntax::Kind::arrayExpr: {
                const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
                for (const auto& element : array->elements) {
                    collectEvaluationAccesses(element, accesses);
                }
                break;
            }
            case syntax::Kind::dictionaryExpr: {
                const auto dictionary =
                        std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression);
                for (const auto& entry : dictionary->entries) {
                    collectEvaluationAccesses(entry->key, accesses);
                    collectEvaluationAccesses(entry->value, accesses);
                }
                break;
            }
            case syntax::Kind::contextualCaseExpr: {
                const auto enumCase =
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
                for (const auto& argument : enumCase->arguments) {
                    collectEvaluationAccesses(argument->value, accesses);
                }
                break;
            }
            default:
                break;
        }
    }

    bool pathsOverlap(const AccessPath& left, const AccessPath& right) const {
        if (left.root != right.root) return false;
        const auto count = std::min(left.projections.size(), right.projections.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& leftProjection = left.projections[index];
            const auto& rightProjection = right.projections[index];
            if (!leftProjection.has_value() || !rightProjection.has_value()) return true;
            if (*leftProjection != *rightProjection) return false;
        }
        return true;
    }

    const char* accessEffectName(syntax::AccessEffect effect) const {
        switch (effect) {
            case syntax::AccessEffect::borrowing: return "borrowing";
            case syntax::AccessEffect::inout: return "inout";
            case syntax::AccessEffect::consuming: return "consuming";
            case syntax::AccessEffect::initializing: return "initializing";
        }
        return "unknown";
    }

    void validateCallExclusivity(const std::vector<CallAccess>& accesses) {
        for (size_t right = 0; right < accesses.size(); ++right) {
            for (size_t left = 0; left < right; ++left) {
                if (accesses[left].effect == syntax::AccessEffect::borrowing &&
                    accesses[right].effect == syntax::AccessEffect::borrowing) {
                    continue;
                }
                if (!accesses[left].sustained && !accesses[right].sustained) continue;
                if (!pathsOverlap(accesses[left].path, accesses[right].path)) continue;
                const auto* root = model->semanticModelValue->symbol(accesses[right].path.root);
                report(
                        TypeCheckingDiagnosticId::overlappingAccess,
                        accesses[right].span,
                        "overlapping " +
                                std::string(accessEffectName(accesses[left].effect)) +
                                " and " + accessEffectName(accesses[right].effect) +
                                " access to '" +
                                (root == nullptr ? std::string("<storage>") : root->name) +
                                "' in the same call");
                return;
            }
        }
    }

    struct StorageAccess {
        bool writable = true;
        bool requiresMarker = false;
        std::string immutableName;
    };

    StorageAccess analyzeStorage(const syntax::ExprPtr& expression) const {
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
                }
                return result;
            }
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                auto result = analyzeStorage(subscript->base);
                result.requiresMarker = true;
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

    void checkAssignmentAccess(const syntax::ExprPtr& target) {
        const auto hasMarker = target != nullptr && target->kind == syntax::Kind::accessExpr;
        const auto storage = analyzeStorage(target);
        if (!storage.writable) {
            report(
                    TypeCheckingDiagnosticId::assignmentToImmutable,
                    target == nullptr ? SourceSpan {} : target->span,
                    "cannot assign through immutable binding or field '" +
                            storage.immutableName + "'");
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

    void checkCallArgumentAccess(
            const semantic::Symbol& target,
            size_t parameterIndex,
            const syntax::CallArgumentSyntax& argument) {
        assert(target.callable.has_value());
        const auto& parameters = target.callable->parameters;
        if (parameterIndex >= parameters.size()) return;

        const auto required = parameters[parameterIndex].access;
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
                    "inout argument must refer to mutable storage");
        }
        if (required == syntax::AccessEffect::initializing &&
            !isInitializable(argument.value)) {
            report(
                    TypeCheckingDiagnosticId::invalidInitializingArgument,
                    argument.value->span,
                    "initializing argument must be a whole mutable owning local or consuming parameter");
        }
        if (required == syntax::AccessEffect::consuming &&
            !isConsumable(argument.value)) {
            report(
                    TypeCheckingDiagnosticId::invalidConsumeArgument,
                    argument.value->span,
                    "consuming argument must be an owning local, consuming parameter, or temporary");
        }
    }

    std::optional<DiagnosticFixIt> accessMarkerFix(
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

    std::string accessMarkerHelp(
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

    bool isConsumable(const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return false;
        const auto path = storagePath(expression);
        if (!path.has_value()) {
            return expression->kind != syntax::Kind::memberExpr &&
                expression->kind != syntax::Kind::subscriptExpr &&
                expression->kind != syntax::Kind::accessExpr;
        }
        const auto* symbol = model->semanticModelValue->symbol(path->root);
        if (symbol == nullptr) return false;
        if (symbol->kind == semantic::SymbolKind::binding) return true;
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

    bool isInitializable(const syntax::ExprPtr& expression) const {
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

    std::optional<size_t> parameterIndexForArgument(
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

    TypeId checkSubscript(const syntax::SubscriptExprSyntax::Ptr& expression) {
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
            case TypeKind::array:
                indexType = model->typeContext.intType();
                result = baseType->arguments[0];
                break;
            case TypeKind::dictionary:
                indexType = baseType->arguments[0];
                result = baseType->arguments[1];
                break;
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

    TypeId checkContextualCase(
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

        recordResolvedReference(expression, *caseSymbol);
        recordResolvedCallTarget(expression, *caseSymbol);
        validateContextualCaseArguments(*expression, *caseSymbol, *signature);
        return *expected;
    }

    std::optional<TypedCallableSignature> enumCaseSignature(
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
            if (signature == nullptr || signature->result != enumerationType) {
                return std::nullopt;
            }
            return *signature;
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
        if (type->kind == TypeKind::optional && symbol->name == "Some") {
            signature.parameters.push_back(type->arguments[0]);
        } else if (type->kind == TypeKind::optional && symbol->name != "None") {
            return std::nullopt;
        } else if (type->kind == TypeKind::result && symbol->name == "Ok") {
            signature.parameters.push_back(type->arguments[0]);
        } else if (type->kind == TypeKind::result && symbol->name == "Err") {
            signature.parameters.push_back(type->arguments[1]);
        } else if (type->kind != TypeKind::optional && type->kind != TypeKind::result) {
            return std::nullopt;
        }
        return signature;
    }

    void validateContextualCaseArguments(
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
            const auto actual = checkExpression(argument->value, expected);
            if (expected.has_value() && actual.has_value()) {
                requireAssignable(*actual, *expected, argument->value->span);
            }
        }
    }

    void validateEnumArgumentLabel(
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

    struct PatternCoverage {
        bool catchesAll = false;
        std::optional<semantic::SymbolId> enumCase;
        std::optional<bool> booleanLiteral;
    };

    TypeId checkMatch(
            const syntax::MatchExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        const auto scrutinee = checkExpression(expression->scrutinee).value_or(
                model->typeContext.errorType());
        std::vector<PatternCoverage> coverage;
        coverage.reserve(expression->arms.size());
        std::optional<TypeId> result;

        for (const auto& arm : expression->arms) {
            coverage.push_back(checkPattern(arm->pattern, scrutinee));
            const auto armType = checkExpression(arm->body, expected).value_or(
                    model->typeContext.errorType());
            recordNodeType(arm, armType);
            result = result.has_value()
                    ? std::optional<TypeId>(commonType(*result, armType, arm->body->span))
                    : std::optional<TypeId>(armType);
        }

        checkMatchExhaustiveness(expression, scrutinee, coverage);
        return result.value_or(model->typeContext.voidType());
    }

    PatternCoverage checkPattern(
            const syntax::PatternPtr& pattern,
            TypeId expected) {
        if (pattern == nullptr) return {};
        markDeferredReferenceHandled(pattern);
        PatternCoverage coverage;

        switch (pattern->kind) {
            case syntax::Kind::errorPattern:
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
                const auto actual = patternLiteralType(literal->literal, expected);
                requireAssignable(actual, expected, pattern->span);
                if (literal->literal != nullptr && literal->literal->kind == booleanLiteral) {
                    coverage.booleanLiteral = literal->literal->rawValue == Literals::TRUE;
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

    TypeId patternLiteralType(const Token::Ptr& literal, TypeId expected) const {
        if (literal == nullptr) return model->typeContext.errorType();
        switch (literal->kind) {
            case decimalLiteral: return model->typeContext.intType();
            case booleanLiteral: return model->typeContext.boolType();
            case stringLiteral: return model->typeContext.stringType();
            case byteLiteral: return model->typeContext.uint8Type();
            case nilLiteral: {
                const auto* expectedType = model->typeContext.type(expected);
                return expectedType != nullptr && expectedType->kind == TypeKind::optional
                        ? expected
                        : model->typeContext.errorType();
            }
            default:
                return model->typeContext.errorType();
        }
    }

    PatternCoverage checkEnumCasePattern(
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

        recordResolvedReference(pattern, *caseSymbol);
        validatePatternArguments(*pattern, *caseSymbol, *signature);
        return PatternCoverage { false, caseSymbol, std::nullopt };
    }

    void validatePatternArguments(
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
            checkPattern(argument->pattern, payloadType);
            recordNodeType(argument, payloadType);
        }
    }

    void checkMatchExhaustiveness(
            const syntax::MatchExprSyntax::Ptr& expression,
            TypeId scrutinee,
            const std::vector<PatternCoverage>& coverage) {
        if (scrutinee == model->typeContext.errorType() ||
            std::any_of(coverage.begin(), coverage.end(), [](const auto& item) {
                return item.catchesAll;
            })) {
            return;
        }

        const auto* type = model->typeContext.type(scrutinee);
        bool exhaustive = false;
        if (type != nullptr && type->kind == TypeKind::boolean) {
            bool hasTrue = false;
            bool hasFalse = false;
            for (const auto& item : coverage) {
                if (!item.booleanLiteral.has_value()) continue;
                hasTrue |= *item.booleanLiteral;
                hasFalse |= !*item.booleanLiteral;
            }
            exhaustive = hasTrue && hasFalse;
        } else {
            const auto required = enumCases(scrutinee);
            if (!required.empty()) {
                std::unordered_set<semantic::SymbolId> covered;
                for (const auto& item : coverage) {
                    if (item.enumCase.has_value()) covered.insert(*item.enumCase);
                }
                exhaustive = std::all_of(
                        required.begin(),
                        required.end(),
                        [&covered](const auto caseSymbol) {
                            return covered.contains(caseSymbol);
                        });
            }
        }

        if (!exhaustive) {
            report(
                    TypeCheckingDiagnosticId::nonExhaustiveMatch,
                    expression->span,
                    "match over '" + model->typeContext.displayName(scrutinee) +
                            "' is not exhaustive; add the missing cases or a wildcard arm");
        }
    }

    std::vector<semantic::SymbolId> enumCases(TypeId type) const {
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
        return result;
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
        case TypeCheckingDiagnosticId::unknownMember:
            return "type-checking.unknown-member";
        case TypeCheckingDiagnosticId::notSubscriptable:
            return "type-checking.not-subscriptable";
        case TypeCheckingDiagnosticId::unknownEnumCase:
            return "type-checking.unknown-enum-case";
        case TypeCheckingDiagnosticId::enumCaseArgumentMismatch:
            return "type-checking.enum-case-argument-mismatch";
        case TypeCheckingDiagnosticId::nonExhaustiveMatch:
            return "type-checking.non-exhaustive-match";
        case TypeCheckingDiagnosticId::assignmentToImmutable:
            return "type-checking.assignment-to-immutable";
        case TypeCheckingDiagnosticId::invalidAccessMarker:
            return "type-checking.invalid-access-marker";
        case TypeCheckingDiagnosticId::invalidInoutArgument:
            return "type-checking.invalid-inout-argument";
        case TypeCheckingDiagnosticId::invalidConsumeArgument:
            return "type-checking.invalid-consume-argument";
        case TypeCheckingDiagnosticId::invalidInitializingArgument:
            return "type-checking.invalid-initializing-argument";
        case TypeCheckingDiagnosticId::overlappingAccess:
            return "type-checking.overlapping-access";
        case TypeCheckingDiagnosticId::notCallable:
            return "type-checking.not-callable";
        case TypeCheckingDiagnosticId::unresolvedReference:
            return "type-checking.unresolved-reference";
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