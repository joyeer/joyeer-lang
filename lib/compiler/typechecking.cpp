#include "joyeer/compiler/typechecking.h"

#include <cassert>
#include <functional>
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

} // namespace joyeer::typing