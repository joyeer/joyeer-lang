#include "compiler/frontend/typechecking.h"
#include "compiler/frontend/hostbuiltins.h"

#include <algorithm>
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
    for (const auto effect : key.effects) {
        combineHash(result, std::hash<int>()(static_cast<int>(effect)));
    }
    return result;
}

TypeContext::TypeContext(const semantic::SemanticModel& model): model(model) {
    errorTypeId = intern(TypeKind::error, semantic::invalidSymbolId);

    voidTypeId = registerConcreteBuiltin("Void", TypeKind::voidType);
    neverTypeId = registerConcreteBuiltin("Never", TypeKind::never);
    anyTypeId = registerConcreteBuiltin("Any", TypeKind::any);
    intTypeId = registerConcreteBuiltin("Int", TypeKind::integer);
    boolTypeId = registerConcreteBuiltin("Bool", TypeKind::boolean);
    stringTypeId = registerConcreteBuiltin("String", TypeKind::string);
    uint8TypeId = registerConcreteBuiltin("UInt8", TypeKind::uint8);
    registerConcreteBuiltin("IOError", TypeKind::enumeration);
    for (const auto& descriptor : hostbuiltins::enumerations) {
        registerConcreteBuiltin(std::string(descriptor.name), TypeKind::enumeration);
    }

    for (const auto& [name, kind] : {
            std::pair {"Int8", TypeKind::int8}, {"Int16", TypeKind::int16},
            {"Int32", TypeKind::int32}, {"Int64", TypeKind::int64},
            {"UInt16", TypeKind::uint16}, {"UInt32", TypeKind::uint32},
            {"UInt64", TypeKind::uint64}, {"UInt", TypeKind::unsignedInteger},
            {"Float", TypeKind::float32}}) {
        registerConcreteBuiltin(name, kind);
    }
    doubleTypeId = registerConcreteBuiltin("Double", TypeKind::float64);
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

TypeId TypeContext::doubleType() const {
    return doubleTypeId;
}

TypeId TypeContext::arrayType(TypeId element) {
    const auto symbol = model.roleSymbol(semantic::LibraryRole::arrayType);
    if (!symbol.has_value()) return errorTypeId;
    return typeForSymbol(*symbol, { element }).value_or(errorTypeId);
}

bool TypeContext::isArray(const TypeRecord* record) const {
    return record != nullptr && record->kind == TypeKind::structure &&
            record->symbol == model.roleSymbol(semantic::LibraryRole::arrayType) &&
            record->arguments.size() == 1;
}

TypeId TypeContext::dictionaryType(TypeId key, TypeId value) {
    const auto symbol = model.roleSymbol(semantic::LibraryRole::dictType);
    if (!symbol.has_value()) return errorTypeId;
    return typeForSymbol(*symbol, { key, value }).value_or(errorTypeId);
}

bool TypeContext::isDictionary(const TypeRecord* record) const {
    return record != nullptr && record->kind == TypeKind::structure &&
            record->symbol == model.roleSymbol(semantic::LibraryRole::dictType) && record->arguments.size() == 2;
}

TypeId TypeContext::optionalType(TypeId wrapped) {
    const auto symbol = model.roleSymbol(semantic::LibraryRole::optional);
    if (!symbol.has_value()) return errorTypeId;
    return typeForSymbol(*symbol, { wrapped }).value_or(errorTypeId);
}

bool TypeContext::isOptional(const TypeRecord* record) const {
    return record != nullptr && record->kind == TypeKind::enumeration &&
            record->symbol == model.roleSymbol(semantic::LibraryRole::optional);
}

bool TypeContext::isResult(const TypeRecord* record) const {
    return record != nullptr && record->kind == TypeKind::enumeration &&
            record->symbol == model.roleSymbol(semantic::LibraryRole::result);
}

TypeId TypeContext::functionType(
        const std::vector<TypeId>& parameters,
        const std::vector<syntax::AccessEffect>& effects,
        TypeId result) {
    assert(parameters.size() == effects.size());
    std::vector<TypeId> arguments = parameters;
    arguments.push_back(result);
    for (const auto argument : arguments) {
        if (argument == errorTypeId) return errorTypeId;
    }
    return intern(TypeKind::function, semantic::invalidSymbolId, std::move(arguments), effects);
}

TypeId TypeContext::resultType(TypeId success, TypeId failure) {
    const auto symbol = model.roleSymbol(semantic::LibraryRole::result);
    if (!symbol.has_value()) return errorTypeId;
    return typeForSymbol(*symbol, { success, failure }).value_or(errorTypeId);
}

TypeId TypeContext::typeParameterType(semantic::SymbolId symbol) {
    return intern(TypeKind::typeParameter, symbol);
}

TypeId TypeContext::substitute(
        TypeId id, const std::unordered_map<semantic::SymbolId, TypeId>& substitution) {
    if (substitution.empty()) return id;
    const auto* value = type(id);
    if (value == nullptr) return id;
    if (value->kind == TypeKind::typeParameter) {
        const auto found = substitution.find(value->symbol);
        return found == substitution.end() ? id : found->second;
    }
    if (value->arguments.empty()) return id;
    // Copy before interning: substituting can grow `types` and invalidate `value`.
    const auto kind = value->kind;
    const auto symbol = value->symbol;
    auto arguments = value->arguments;
    const auto effects = value->effects;
    bool changed = false;
    for (auto& argument : arguments) {
        const auto replaced = substitute(argument, substitution);
        // An error argument makes the whole type an error, as typeForSymbol and functionType do,
        // so one inference failure does not cascade into a mismatch against "(<error>): Int".
        if (replaced == errorTypeId) return errorTypeId;
        changed = changed || replaced != argument;
        argument = replaced;
    }
    if (!changed) return id;
    return intern(kind, symbol, std::move(arguments), effects);
}

bool TypeContext::mentionsTypeParameter(TypeId id) const {
    const auto* value = type(id);
    if (value == nullptr) return false;
    if (value->kind == TypeKind::typeParameter) return true;
    return std::any_of(value->arguments.begin(), value->arguments.end(),
            [this](TypeId argument) { return mentionsTypeParameter(argument); });
}

size_t TypeContext::nestingDepth(TypeId id) const {
    const auto* value = type(id);
    if (value == nullptr || value->arguments.empty()) return 0;
    size_t deepest = 0;
    for (const auto argument : value->arguments) deepest = std::max(deepest, nestingDepth(argument));
    return deepest + 1;
}

std::optional<TypeId> TypeContext::typeForSymbol(
        semantic::SymbolId symbolId,
        const std::vector<TypeId>& arguments) {
    const auto concrete = zeroArgumentTypes.find(symbolId);
    if (concrete != zeroArgumentTypes.end()) {
        if (!arguments.empty()) return std::nullopt;
        return concrete->second;
    }

    const auto* declaration = model.symbol(symbolId);
    if (declaration == nullptr || declaration->isInvalid) return std::nullopt;
    // A generic struct or enum (spec 20.12) takes exactly its parameters' worth of arguments.
    if (!declaration->typeParameters.empty() &&
        (declaration->kind == semantic::SymbolKind::structure ||
         declaration->kind == semantic::SymbolKind::enumeration)) {
        if (arguments.size() != declaration->typeParameters.size()) return std::nullopt;
        for (const auto argument : arguments) {
            if (type(argument) == nullptr) return std::nullopt;
            if (argument == errorTypeId) return errorTypeId;
        }
        return intern(declaration->kind == semantic::SymbolKind::structure
                              ? TypeKind::structure : TypeKind::enumeration,
                      symbolId, arguments);
    }
    if (!arguments.empty()) return std::nullopt;

    TypeKind kind;
    switch (declaration->kind) {
        case semantic::SymbolKind::structure:
            kind = TypeKind::structure;
            break;
        case semantic::SymbolKind::enumeration:
            kind = TypeKind::enumeration;
            break;
        case semantic::SymbolKind::typeParameter:
            return typeParameterType(symbolId);
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

    const auto* declaration = model.symbol(symbolId);
    if (declaration != nullptr && !declaration->isInvalid &&
        (declaration->kind == semantic::SymbolKind::structure ||
         declaration->kind == semantic::SymbolKind::enumeration ||
         declaration->kind == semantic::SymbolKind::typeParameter)) {
        return declaration->typeParameters.size();
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

std::string TypeContext::displayName(TypeId id, bool disambiguateNominals) const {
    const auto* value = type(id);
    if (value == nullptr) return "<invalid-type>";

    if (isOptional(value) && value->arguments.size() == 1) {
        // `((Int): Int)?` is an optional function; `(Int): Int?` returns an optional.
        const auto* wrapped = type(value->arguments[0]);
        // `Int??` lexes as the `??` operator and cannot be written, so a nested optional prints nominally.
        if (isOptional(wrapped)) return "Optional<" + displayName(value->arguments[0], disambiguateNominals) + ">";
        const bool function = wrapped != nullptr && wrapped->kind == TypeKind::function;
        return (function ? "(" : "") + displayName(value->arguments[0], disambiguateNominals) +
                (function ? ")?" : "?");
    }
    if (isResult(value) && value->arguments.size() == 2) {
        return "Result<" + displayName(value->arguments[0], disambiguateNominals) + ", " +
                displayName(value->arguments[1], disambiguateNominals) + ">";
    }
    if (isArray(value)) {
        return "[" + displayName(value->arguments[0], disambiguateNominals) + "]";
    }
    if (isDictionary(value)) {
        return "[" + displayName(value->arguments[0], disambiguateNominals) + ": " +
                displayName(value->arguments[1], disambiguateNominals) + "]";
    }

    switch (value->kind) {
        case TypeKind::error:
            return "<error>";
        case TypeKind::typeParameter: {
            const auto* declaration = model.symbol(value->symbol);
            if (declaration != nullptr && declaration->kind == semantic::SymbolKind::protocol) return "Self";
            return declaration == nullptr ? "<invalid-type>" : declaration->name;
        }
        case TypeKind::function: {
            std::string text = "(";
            for (size_t index = 0; index + 1 < value->arguments.size(); ++index) {
                if (index != 0) text += ", ";
                switch (value->effects[index]) {
                    case syntax::AccessEffect::inout: text += "inout "; break;
                    case syntax::AccessEffect::consuming: text += "consuming "; break;
                    case syntax::AccessEffect::initializing: text += "initializing "; break;
                    case syntax::AccessEffect::borrowing: break;
                }
                text += displayName(value->arguments[index], disambiguateNominals);
            }
            return text + "): " + displayName(value->arguments.back(), disambiguateNominals);
        }
        default: {
            const auto* declaration = model.symbol(value->symbol);
            if (declaration == nullptr) return "<invalid-type>";
            auto name = declaration->name;
            // A generic struct or enum shows its arguments, `Box<Int>` (spec 20.12).
            std::string suffix;
            for (size_t index = 0; index < value->arguments.size(); ++index) {
                suffix += (index == 0 ? "<" : ", ") + displayName(value->arguments[index], disambiguateNominals);
            }
            if (!suffix.empty()) suffix += ">";
            if (!disambiguateNominals) {
                // A user type that shares its name with a role-bound enum must not print like it, in
                // every diagnostic (decision 0024): `Result<A, B>` of the prelude is not the user's.
                if (declaration->libraryRole == semantic::LibraryRole::none &&
                    (value->kind == TypeKind::structure || value->kind == TypeKind::enumeration)) {
                    for (const auto role : {semantic::LibraryRole::optional, semantic::LibraryRole::result,
                                           semantic::LibraryRole::arrayType, semantic::LibraryRole::dictType}) {
                        const auto roleSymbol = model.roleSymbol(role);
                        const auto* roleDeclaration = roleSymbol.has_value() ? model.symbol(*roleSymbol) : nullptr;
                        if (roleDeclaration == nullptr || roleDeclaration->name != name) continue;
                        const auto* module = model.moduleName(declaration->moduleId);
                        return ((module != nullptr && !module->empty()) ? *module : std::string("<main>")) + "." +
                                name + suffix;
                    }
                }
                return name + suffix;
            }
            if (value->kind != TypeKind::structure && value->kind != TypeKind::enumeration) return name + suffix;
            bool duplicate = false;
            bool sameModule = false;
            bool roleCollision = false;
            for (const auto& other : model.symbols()) {
                if (other.id == declaration->id || other.name != name ||
                    (other.kind != semantic::SymbolKind::structure &&
                     other.kind != semantic::SymbolKind::enumeration &&
                     other.kind != semantic::SymbolKind::builtinType)) continue;
                duplicate = true;
                sameModule |= other.moduleId == declaration->moduleId;
                roleCollision |= other.libraryRole != semantic::LibraryRole::none;
            }
            if (duplicate) {
                if (const auto* module = model.moduleName(declaration->moduleId);
                    module != nullptr && !module->empty()) name = *module + "." + name;
                // The unnamed root module of single-file mode: a user type that shares its name with a
                // role-bound enum must not print like it (decision 0024).
                else if (roleCollision && declaration->libraryRole == semantic::LibraryRole::none) name = "<main>." + name;
                if (sameModule) name += "@" + std::to_string(declaration->span.sourceId);
            }
            return name + suffix;
        }
    }
}

bool TypeContext::isAssignable(TypeId source, TypeId destination) const {
    if (source == errorTypeId || destination == errorTypeId) return true;
    if (source == destination || source == neverTypeId || destination == anyTypeId) return true;

    const auto* destinationType = type(destination);
    if (!isOptional(destinationType)) {
        return false;
    }
    const auto* sourceType = type(source);
    if (isOptional(sourceType)) {
        return isAssignable(sourceType->arguments[0], destinationType->arguments[0]);
    }
    // One implicit promotion wraps one layer (§2.6.1): `Int` does not become `Int??`, and lowering
    // wraps exactly one layer, so a destination whose payload is itself optional is not reachable.
    if (isOptional(type(destinationType->arguments[0]))) return false;
    return isAssignable(source, destinationType->arguments[0]);
}

TypeId TypeContext::intern(
        TypeKind kind,
        semantic::SymbolId symbol,
        std::vector<TypeId> arguments,
        std::vector<syntax::AccessEffect> effects) {
    TypeKey key { kind, symbol, std::move(arguments), std::move(effects) };
    const auto found = internedTypes.find(key);
    if (found != internedTypes.end()) return found->second;

    const auto id = static_cast<TypeId>(types.size());
    types.push_back(TypeRecord { id, key.kind, key.symbol, key.arguments, key.effects });
    internedTypes.emplace(std::move(key), id);
    return id;
}

TypeId TypeContext::registerConcreteBuiltin(const std::string& name, TypeKind kind) {
    const auto* prelude = model.scope(model.preludeScope());
    assert(prelude != nullptr);
    const auto found = prelude->types.find(name);
    assert(found != prelude->types.end());
    const auto symbol = found->second;
    builtinSymbols.emplace(name, symbol);
    const auto id = intern(kind, symbol);
    zeroArgumentTypes.emplace(symbol, id);
    return id;
}

} // namespace joyeer::typing
