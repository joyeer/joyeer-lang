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
        }
        return TypeCheckingResult { model, std::move(diagnostics) };
    }

private:
    TypeCheckedModel::Ptr model;
    std::vector<TypeCheckingDiagnostic> diagnostics;

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