#include "typechecking.h"
#include "hostbuiltins.h"
#include "nameresolution.h"
#include "intrinsics.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace joyeer::typing {

namespace {

void combineHash(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

// The marker written before a method call: `&receiver.method()` or
// `consume receiver.method()`.
enum class ReceiverMarker {
    none,
    inout,
    consume,
};

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

    registerConcreteBuiltin("Void", TypeKind::voidType, voidTypeId);
    registerConcreteBuiltin("Never", TypeKind::never, neverTypeId);
    registerConcreteBuiltin("Any", TypeKind::any, anyTypeId);
    registerConcreteBuiltin("Int", TypeKind::integer, intTypeId);
    registerConcreteBuiltin("Bool", TypeKind::boolean, boolTypeId);
    registerConcreteBuiltin("String", TypeKind::string, stringTypeId);
    registerConcreteBuiltin("UInt8", TypeKind::uint8, uint8TypeId);
    registerConcreteBuiltin("IOError", TypeKind::enumeration, ioErrorTypeId);
    for (const auto& descriptor : hostbuiltins::enumerations) {
        TypeId registeredType = invalidTypeId;
        registerConcreteBuiltin(
                std::string(descriptor.name), TypeKind::enumeration, registeredType);
    }

    registerGenericBuiltin("Array", TypeKind::array, 1);
    registerGenericBuiltin("Dict", TypeKind::dictionary, 2);
    registerGenericBuiltin("Optional", TypeKind::optional, 1);
    registerGenericBuiltin("Result", TypeKind::result, 2);
    registerGenericBuiltin("JoinHandle", TypeKind::task, 1);
    registerGenericBuiltin("ChannelSender", TypeKind::channelSender, 1);
    registerGenericBuiltin("ChannelReceiver", TypeKind::channelReceiver, 1);
    for (const auto& [name, kind] : {
            std::pair {"Int8", TypeKind::int8}, {"Int16", TypeKind::int16},
            {"Int32", TypeKind::int32}, {"Int64", TypeKind::int64},
            {"UInt16", TypeKind::uint16}, {"UInt32", TypeKind::uint32},
            {"UInt64", TypeKind::uint64}, {"UInt", TypeKind::unsignedInteger},
            {"Float", TypeKind::float32}}) {
        TypeId registeredType = invalidTypeId;
        registerConcreteBuiltin(name, kind, registeredType);
    }
    registerConcreteBuiltin("Double", TypeKind::float64, doubleTypeId);
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

TypeId TypeContext::ioErrorType() const {
    return ioErrorTypeId;
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

TypeId TypeContext::taskType(TypeId result) {
    const auto symbol = builtinSymbol("JoinHandle");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { result }).value_or(errorTypeId);
}

TypeId TypeContext::channelSenderType(TypeId element) {
    const auto symbol = builtinSymbol("ChannelSender");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { element }).value_or(errorTypeId);
}

TypeId TypeContext::channelReceiverType(TypeId element) {
    const auto symbol = builtinSymbol("ChannelReceiver");
    assert(symbol.has_value());
    return typeForSymbol(*symbol, { element }).value_or(errorTypeId);
}

TypeId TypeContext::channelResultType(TypeId success) {
    const auto closed = builtinSymbol("ChannelClosed");
    assert(closed.has_value());
    const auto closedType = typeForSymbol(*closed).value_or(errorTypeId);
    return resultType(success, closedType);
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
    const auto symbol = builtinSymbol("Result");
    assert(symbol.has_value());
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
    const auto generic = genericConstructors.find(symbolId);
    if (generic != genericConstructors.end()) return generic->second.arity;

    const auto* declaration = model.symbol(symbolId);
    if (declaration != nullptr && !declaration->isInvalid &&
        (declaration->kind == semantic::SymbolKind::structure ||
         declaration->kind == semantic::SymbolKind::enumeration ||
         declaration->kind == semantic::SymbolKind::typeParameter)) {
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

std::string TypeContext::displayName(TypeId id, bool disambiguateNominals) const {
    const auto* value = type(id);
    if (value == nullptr) return "<invalid-type>";

    switch (value->kind) {
        case TypeKind::error:
            return "<error>";
        case TypeKind::array:
            return "[" + displayName(value->arguments[0], disambiguateNominals) + "]";
        case TypeKind::dictionary:
            return "[" + displayName(value->arguments[0], disambiguateNominals) + ": " +
                    displayName(value->arguments[1], disambiguateNominals) + "]";
        case TypeKind::optional: {
            // `((Int): Int)?` is an optional function; `(Int): Int?` returns an optional.
            const auto* wrapped = type(value->arguments[0]);
            const bool function = wrapped != nullptr && wrapped->kind == TypeKind::function;
            return (function ? "(" : "") + displayName(value->arguments[0], disambiguateNominals) +
                    (function ? ")?" : "?");
        }
        case TypeKind::result:
            return "Result<" + displayName(value->arguments[0], disambiguateNominals) + ", " +
                    displayName(value->arguments[1], disambiguateNominals) + ">";
        case TypeKind::task:
            return "JoinHandle<" + displayName(value->arguments[0], disambiguateNominals) + ">";
        case TypeKind::channelSender:
            return "ChannelSender<" + displayName(value->arguments[0], disambiguateNominals) + ">";
        case TypeKind::channelReceiver:
            return "ChannelReceiver<" + displayName(value->arguments[0], disambiguateNominals) + ">";
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
            if (!disambiguateNominals) return name;
            if (value->kind != TypeKind::structure && value->kind != TypeKind::enumeration) return name;
            bool duplicate = false;
            bool sameModule = false;
            for (const auto& other : model.symbols()) {
                if (other.id == declaration->id || other.name != name ||
                    (other.kind != semantic::SymbolKind::structure &&
                     other.kind != semantic::SymbolKind::enumeration &&
                     other.kind != semantic::SymbolKind::builtinType)) continue;
                duplicate = true;
                sameModule |= other.moduleId == declaration->moduleId;
            }
            if (duplicate) {
                if (const auto* module = model.moduleName(declaration->moduleId);
                    module != nullptr && !module->empty()) name = *module + "." + name;
                if (sameModule) name += "@" + std::to_string(declaration->span.sourceId);
            }
            return name;
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

syntax::StructDeclSyntax::Ptr TypeCheckedModel::structureDeclaration(TypeId id) const {
    const auto* type = typeContext.type(id);
    if (type == nullptr || type->kind != TypeKind::structure) return nullptr;
    const auto* symbol = semanticModelValue->symbol(type->symbol);
    if (symbol == nullptr || !symbol->declaration.has_value()) return nullptr;
    const auto declaration = semanticModelValue->node(*symbol->declaration);
    if (declaration == nullptr || declaration->kind != syntax::Kind::structDecl) return nullptr;
    return std::static_pointer_cast<syntax::StructDeclSyntax>(declaration);
}

syntax::FunctionDeclSyntax::Ptr TypeCheckedModel::deinitializerOf(TypeId type) const {
    const auto structure = structureDeclaration(type);
    return structure == nullptr ? nullptr : structure->deinitializer;
}

bool TypeCheckedModel::isDeclaredLinear(TypeId type) const {
    const auto structure = structureDeclaration(type);
    return structure != nullptr && structure->isLinear;
}

bool TypeCheckedModel::isLinear(TypeId type) const {
    std::vector<TypeId> visiting;
    std::unordered_set<TypeId> clean;
    return isLinear(type, visiting, clean);
}

bool TypeCheckedModel::isLinear(
        TypeId id, std::vector<TypeId>& visiting, std::unordered_set<TypeId>& clean) const {
    const auto* type = typeContext.type(id);
    if (type == nullptr || clean.contains(id)) return false;
    if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) return false;
    visiting.push_back(id);
    bool linear = false;
    switch (type->kind) {
        case TypeKind::array:
        case TypeKind::dictionary:
        case TypeKind::optional:
        case TypeKind::result:
            for (const auto argument : type->arguments) {
                if (isLinear(argument, visiting, clean)) { linear = true; break; }
            }
            break;
        case TypeKind::task:
            // A running task must be joined, never dropped (spec 21.3).
            linear = true;
            break;
        case TypeKind::structure: {
            const auto structure = structureDeclaration(id);
            if (structure == nullptr) break;
            if (structure->isLinear) { linear = true; break; }
            for (const auto& field : structure->fields) {
                const auto fieldSymbol = semanticModelValue->declaredSymbol(field);
                const auto fieldType = fieldSymbol.has_value()
                        ? typeOf(*fieldSymbol)
                        : std::optional<TypeId>();
                if (fieldType.has_value() && isLinear(*fieldType, visiting, clean)) {
                    linear = true;
                    break;
                }
            }
            break;
        }
        case TypeKind::enumeration: {
            const auto* symbol = semanticModelValue->symbol(type->symbol);
            if (symbol == nullptr || !symbol->declaration.has_value()) break;
            const auto declaration = semanticModelValue->node(*symbol->declaration);
            if (declaration == nullptr || declaration->kind != syntax::Kind::enumDecl) break;
            const auto enumeration = std::static_pointer_cast<syntax::EnumDeclSyntax>(declaration);
            for (const auto& enumCase : enumeration->cases) {
                const auto caseSymbol = semanticModelValue->declaredSymbol(enumCase);
                const auto* signature = caseSymbol.has_value() ? callable(*caseSymbol) : nullptr;
                if (signature == nullptr) continue;
                for (const auto payload : signature->parameters) {
                    if (isLinear(payload, visiting, clean)) { linear = true; break; }
                }
                if (linear) break;
            }
            break;
        }
        default:
            break;
    }
    visiting.pop_back();
    if (!linear) clean.insert(id);
    return linear;
}

std::vector<std::vector<semantic::SymbolId>> TypeCheckedModel::linearLeafPaths(TypeId type) const {
    if (!isLinear(type)) return {};
    const auto structure = structureDeclaration(type);
    // Any other linear type (an enum, an Optional, an Array, a builtin) is a single leaf: it is
    // consumed whole.
    if (structure == nullptr) return { {} };
    // A struct ends field by field. One declared `linear` has no linear field to point at, so each
    // of its fields is a leaf, copyable ones included (destructuring); one that is linear only
    // through its fields needs just those fields consumed.
    std::vector<std::vector<semantic::SymbolId>> paths;
    for (const auto& field : structure->fields) {
        const auto fieldSymbol = semanticModelValue->declaredSymbol(field);
        const auto fieldType = fieldSymbol.has_value() ? typeOf(*fieldSymbol) : std::optional<TypeId>();
        if (!fieldSymbol.has_value() || !fieldType.has_value()) continue;
        auto fieldPaths = linearLeafPaths(*fieldType);
        if (fieldPaths.empty() && structure->isLinear) fieldPaths.push_back({});
        for (auto& rest : fieldPaths) {
            rest.insert(rest.begin(), *fieldSymbol);
            paths.push_back(std::move(rest));
        }
    }
    return paths;
}

bool TypeCheckedModel::isCopyable(TypeId type) const {
    return !noncopyableReason(type).has_value();
}

std::optional<std::string> TypeCheckedModel::noncopyableReason(TypeId type) const {
    std::vector<TypeId> visiting;
    std::unordered_set<TypeId> clean;
    return noncopyableReason(type, visiting, clean);
}

std::optional<std::string> TypeCheckedModel::noncopyableReason(
        TypeId id, std::vector<TypeId>& visiting, std::unordered_set<TypeId>& clean) const {
    const auto* type = typeContext.type(id);
    if (type == nullptr) return std::nullopt;
    // A struct cannot contain itself by value, but guard so a malformed program cannot loop.
    if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) return std::nullopt;
    // A type already found copyable in this query is not walked again, so a type that reuses the
    // same part many times stays linear in the number of distinct types.
    if (clean.contains(id)) return std::nullopt;
    visiting.push_back(id);
    std::optional<std::string> reason;
    const auto name = "'" + typeContext.displayName(id) + "'";

    switch (type->kind) {
        case TypeKind::array:
        case TypeKind::dictionary:
        case TypeKind::optional:
        case TypeKind::result:
            for (const auto argument : type->arguments) {
                if (noncopyableReason(argument, visiting, clean).has_value()) {
                    reason = name + " holds the noncopyable type '" +
                            typeContext.displayName(argument) + "'";
                    break;
                }
            }
            break;
        case TypeKind::task:
            reason = name + " is a task handle, which has exactly one owner";
            break;
        case TypeKind::typeParameter: {
            // Copyable only when its constraint says so (spec 20.5). `Self` in a protocol is never
            // copied: requirements have no bodies.
            const auto copyable = copyableProtocol();
            const auto& protocols = constraints(type->symbol);
            if (!copyable.has_value() ||
                std::find(protocols.begin(), protocols.end(), *copyable) == protocols.end()) {
                reason = name + " is a type parameter not constrained to 'Copyable'";
            }
            break;
        }
        case TypeKind::channelSender:
        case TypeKind::channelReceiver:
            reason = name + " is a channel end, which has exactly one owner";
            break;
        case TypeKind::structure: {
            const auto structure = structureDeclaration(id);
            if (structure == nullptr) break;
            if (structure->deinitializer != nullptr) {
                reason = name + " declares 'deinit'";
                break;
            }
            if (structure->isLinear) {
                reason = name + " is linear";
                break;
            }
            for (const auto& field : structure->fields) {
                const auto fieldSymbol = semanticModelValue->declaredSymbol(field);
                const auto fieldType = fieldSymbol.has_value()
                        ? typeOf(*fieldSymbol)
                        : std::optional<TypeId>();
                if (fieldType.has_value() &&
                    noncopyableReason(*fieldType, visiting, clean).has_value()) {
                    reason = name + " has the noncopyable field '" +
                            (field->name == nullptr ? std::string() : field->name->rawValue) +
                            "' of type '" + typeContext.displayName(*fieldType) + "'";
                    break;
                }
            }
            break;
        }
        case TypeKind::enumeration: {
            const auto* symbol = semanticModelValue->symbol(type->symbol);
            if (symbol == nullptr || !symbol->declaration.has_value()) break;
            const auto declaration = semanticModelValue->node(*symbol->declaration);
            if (declaration == nullptr || declaration->kind != syntax::Kind::enumDecl) break;
            const auto enumeration = std::static_pointer_cast<syntax::EnumDeclSyntax>(declaration);
            for (const auto& enumCase : enumeration->cases) {
                const auto caseSymbol = semanticModelValue->declaredSymbol(enumCase);
                const auto* signature = caseSymbol.has_value()
                        ? callable(*caseSymbol)
                        : nullptr;
                if (signature == nullptr) continue;
                for (const auto payload : signature->parameters) {
                    if (noncopyableReason(payload, visiting, clean).has_value()) {
                        reason = name + " has a noncopyable payload of type '" +
                                typeContext.displayName(payload) + "'";
                        break;
                    }
                }
                if (reason.has_value()) break;
            }
            break;
        }
        default:
            break;
    }

    visiting.pop_back();
    if (!reason.has_value()) clean.insert(id);
    return reason;
}

const TypedCallableSignature* TypeCheckedModel::callable(
        semantic::SymbolId symbol) const {
    const auto found = callables.find(symbol);
    if (found == callables.end()) return nullptr;
    return &found->second;
}

const NativeBinding* TypeCheckedModel::nativeBinding(semantic::SymbolId symbol) const {
    const auto found = nativeBindings.find(symbol);
    return found == nativeBindings.end() ? nullptr : &found->second;
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

const std::vector<semantic::SymbolId>* TypeCheckedModel::genericParameters(semantic::SymbolId function) const {
    const auto found = genericParameterLists.find(function);
    return found == genericParameterLists.end() ? nullptr : &found->second;
}

const std::vector<semantic::SymbolId>& TypeCheckedModel::constraints(semantic::SymbolId typeParameter) const {
    static const std::vector<semantic::SymbolId> none;
    const auto found = constraintLists.find(typeParameter);
    return found == constraintLists.end() ? none : found->second;
}

std::optional<semantic::SymbolId> TypeCheckedModel::copyableProtocol() const {
    const auto* prelude = semanticModelValue->scope(semanticModelValue->preludeScope());
    if (prelude == nullptr) return std::nullopt;
    const auto found = prelude->types.find("Copyable");
    if (found == prelude->types.end()) return std::nullopt;
    return found->second;
}

const GenericCall* TypeCheckedModel::genericCall(const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (!id.has_value()) return nullptr;
    const auto found = genericCalls.find(*id);
    return found == genericCalls.end() ? nullptr : &found->second;
}

std::optional<semantic::SymbolId> TypeCheckedModel::witness(
        TypeId type, semantic::SymbolId requirement) const {
    for (const auto& conformance : conformanceList) {
        if (conformance.type != type) continue;
        const auto found = conformance.witnesses.find(requirement);
        if (found != conformance.witnesses.end()) return found->second;
    }
    return std::nullopt;
}

std::optional<size_t> TypeCheckedModel::instanceIndex(
        semantic::SymbolId function, const std::vector<TypeId>& arguments) const {
    const auto found = instanceIndices.find({function, arguments});
    if (found == instanceIndices.end()) return std::nullopt;
    return found->second;
}

std::string TypeCheckedModel::functionDisplayName(semantic::SymbolId function) const {
    const auto* symbol = semanticModelValue->symbol(function);
    assert(symbol != nullptr);
    const auto localName = [this](const semantic::Symbol& value) {
        if (value.containingSymbol.has_value()) {
            const auto* owner = semanticModelValue->symbol(*value.containingSymbol);
            if (owner != nullptr) return owner->name + "." + value.name;
        }
        return value.name;
    };
    const auto local = localName(*symbol);
    auto name = local;
    if (const auto* module = semanticModelValue->moduleName(symbol->moduleId);
        module != nullptr && !module->empty()) {
        name = *module + "." + name;
    }
    if (std::any_of(semanticModelValue->symbols().begin(), semanticModelValue->symbols().end(),
            [&](const auto& other) {
                return other.id != function && other.kind == symbol->kind &&
                        other.moduleId == symbol->moduleId && localName(other) == local;
            })) {
        name += "@" + std::to_string(symbol->span.sourceId);
    }
    return name;
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
            checkConstraintConflicts();
            for (const auto& item : root->items) {
                if (item->kind == syntax::Kind::extensionDecl) {
                    checkConformances(std::static_pointer_cast<syntax::ExtensionDeclSyntax>(item));
                }
            }
            for (const auto& [type, span] : enumPayloadChecks) requireNoLinearDictionary(type, span);
            validateNativeSymbols();
            for (const auto& item : root->items) checkTopLevelBody(item);
        }
        validateDeferredReferences();
        // Instances are found only for a program that checked cleanly (decision 0021).
        if (diagnostics.empty()) checkExpandingRecursion();
        if (diagnostics.empty()) instantiate();
        return TypeCheckingResult { model, std::move(diagnostics) };
    }

private:
    TypeCheckedModel::Ptr model;
    std::vector<TypeCheckingDiagnostic> diagnostics;
    std::optional<TypeId> currentReturnType;
    size_t loopDepth = 0;
    std::unordered_set<semantic::NodeId> handledDeferredReferences;
    // Enum case payload types, checked for linear dictionaries and channel elements after resolution.
    std::vector<std::pair<TypeId, SourceSpan>> enumPayloadChecks;
    // `consume` access expressions that appear where a value is stored (see permitMove).
    std::unordered_set<const syntax::Node*> permittedMoves;
    bool insideDeinit = false;
    // Type parameters with constraints, checked for conflicting requirements once all are known.
    std::vector<semantic::SymbolId> constrainedParameters;
    std::optional<semantic::SymbolId> currentFunction;

    void report(
            TypeCheckingDiagnosticId id,
            SourceSpan span,
            std::string message,
            std::optional<std::string> help = std::nullopt,
            std::optional<DiagnosticFixIt> fixIt = std::nullopt,
            std::vector<DiagnosticSourceNote> notes = {}) {
        diagnostics.push_back(TypeCheckingDiagnostic {
            id,
            span,
            std::move(message),
            std::move(help),
            std::move(fixIt),
            std::move(notes),
        });
    }

    // A core unit (spec 19.4.6) is part of the runtime, so it may use only the
    // types that never lower to a runtime call: Bool, the integer and floating
    // types, Void and Never.
    [[nodiscard]] bool allowedInCoreUnit(TypeId type) const {
        const auto* record = model->typeContext.type(type);
        if (record == nullptr) return true;
        switch (record->kind) {
            case TypeKind::error:
            case TypeKind::voidType:
            case TypeKind::never:
            case TypeKind::boolean:
                return true;
            default:
                return isNumeric(record->kind);
        }
    }

    // The string-literal argument of joyeer_intrinsic_literal_address, which is
    // read as constant data and never becomes a String value, is the one managed
    // value a core unit may write.
    const syntax::Node* literalAddressArgument = nullptr;
    // The callee of the call being checked: a function named there is called, not a function value.
    const syntax::Node* currentCallee = nullptr;
    // The base of the member expression being checked: a type named there is not a value.
    const syntax::Node* currentMemberBase = nullptr;
    // Set while resolving the parameter of the literal-address intrinsic itself.
    bool coreCheckSuspended = false;

    [[nodiscard]] static bool declaresLiteralAddress(const syntax::FunctionDeclSyntax& declaration) {
        for (const auto& attribute : declaration.attributes) {
            if (attribute.name == nullptr || attribute.name->rawValue != "c_extern" ||
                attribute.arguments.empty() || attribute.arguments.front()->value == nullptr ||
                attribute.arguments.front()->value->kind != syntax::Kind::literalExpr) {
                continue;
            }
            const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(
                    attribute.arguments.front()->value);
            if (literal->literal != nullptr && intrinsic::readsLiteral(literal->literal->rawValue)) {
                return true;
            }
        }
        return false;
    }

    void requireCoreType(const syntax::NodePtr& node, TypeId type) {
        if (node == nullptr || coreCheckSuspended || node.get() == literalAddressArgument ||
            !model->semanticModelValue->isCoreSource(node->span) || allowedInCoreUnit(type)) {
            return;
        }
        report(TypeCheckingDiagnosticId::managedTypeInCoreUnit, node->span,
               "a runtime core unit may use only scalar types, but this has type '" +
                       model->typeContext.displayName(type) +
                       "', whose operations call into the runtime");
    }

    void recordNodeType(const syntax::NodePtr& node, TypeId type) {
        const auto id = model->semanticModelValue->nodeId(node);
        if (id.has_value()) model->nodeTypes[*id] = type;
        requireCoreType(node, type);
    }

    void recordDeclaredType(const syntax::NodePtr& node, TypeId type) {
        const auto symbol = model->semanticModelValue->declaredSymbol(node);
        if (symbol.has_value()) model->symbolTypes[*symbol] = type;
        requireCoreType(node, type);
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
            if (symbol.kind != semantic::SymbolKind::builtinMember) continue;
            if (symbol.name == "get" && symbol.callable.has_value()) {
                model->symbolTypes[symbol.id] = model->typeContext.anyType();
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    { model->typeContext.anyType() },
                    model->typeContext.anyType(),
                };
                continue;
            }
            if (symbol.name == "removeLast" && symbol.callable.has_value()) {
                model->symbolTypes[symbol.id] = model->typeContext.anyType();
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    {},
                    model->typeContext.anyType(),
                };
                continue;
            }
            if ((symbol.name == "send" || symbol.name == "receive") && symbol.callable.has_value()) {
                model->symbolTypes[symbol.id] = model->typeContext.anyType();
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    symbol.name == "send" ? std::vector<TypeId> { model->typeContext.anyType() }
                                          : std::vector<TypeId> {},
                    model->typeContext.anyType(),
                };
                continue;
            }
            if (symbol.name == "close" && symbol.callable.has_value()) {
                model->symbolTypes[symbol.id] = model->typeContext.voidType();
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    {},
                    model->typeContext.voidType(),
                };
                continue;
            }
            if (symbol.name == "join" && symbol.callable.has_value()) {
                model->symbolTypes[symbol.id] = model->typeContext.anyType();
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    {},
                    model->typeContext.anyType(),
                };
                continue;
            }
            if (symbol.name == "consumeEmpty" && symbol.callable.has_value()) {
                model->symbolTypes[symbol.id] = model->typeContext.voidType();
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    {},
                    model->typeContext.voidType(),
                };
                continue;
            }
            if (symbol.name == "utf8" && symbol.callable.has_value()) {
                const auto result = model->typeContext.arrayType(
                        model->typeContext.uint8Type());
                model->symbolTypes[symbol.id] = result;
                model->callables[symbol.id] = TypedCallableSignature {
                    semantic::CallableKind::function,
                    true,
                    {},
                    result,
                };
                continue;
            }
            if (!symbol.declaredType.has_value()) continue;
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
            case syntax::Kind::unitType:
                result = model->typeContext.voidType();
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
                if (const auto* declaration = model->semanticModelValue->symbol(*symbol)) {
                    if (declaration->kind == semantic::SymbolKind::protocol) {
                        if (nominal->name != nullptr && nominal->name->rawValue == "Self" && arguments.empty()) {
                            result = model->typeContext.typeParameterType(*symbol);
                            break;
                        }
                        report(TypeCheckingDiagnosticId::protocolUsedAsType, syntaxType->span,
                               "'" + declaration->name + "' is a protocol, not a type (spec 20.2)",
                               "declare a type parameter constrained to it, as in 'func f<T: " +
                                       declaration->name + ">(value: T)'");
                        break;
                    }
                    if (declaration->kind == semantic::SymbolKind::typeParameter && !arguments.empty()) {
                        report(TypeCheckingDiagnosticId::invalidGenericDeclaration, syntaxType->span,
                               "type parameter '" + declaration->name + "' takes no type arguments");
                        break;
                    }
                }
                const auto resolved = model->typeContext.typeForSymbol(*symbol, arguments);
                if (resolved.has_value()) {
                    result = *resolved;
                    const auto* record = model->typeContext.type(result);
                    if (record != nullptr && record->kind == TypeKind::dictionary) {
                        rejectInvalidDictionaryKey(record->arguments[0], nominal->arguments[0]->span);
                    }
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
                rejectInvalidDictionaryKey(key, dictionary->key->span);
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
            case syntax::Kind::functionType: {
                const auto function = std::static_pointer_cast<syntax::FunctionTypeSyntax>(syntaxType);
                std::vector<TypeId> parameters;
                std::vector<syntax::AccessEffect> effects;
                for (const auto& parameter : function->parameters) {
                    auto type = resolveType(parameter.type).value_or(model->typeContext.errorType());
                    if (type == model->typeContext.neverType()) {
                        report(TypeCheckingDiagnosticId::invalidFunctionType, parameter.type->span,
                               "'Never' cannot be a function type's parameter type",
                               "a function taking a value that cannot exist could never be called");
                        type = model->typeContext.errorType();
                    }
                    parameters.push_back(type);
                    effects.push_back(parameter.accessEffect());
                }
                const auto functionResult = resolveType(function->result).value_or(model->typeContext.errorType());
                result = model->typeContext.functionType(parameters, effects, functionResult);
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
            case syntax::Kind::functionDecl: {
                const auto declaration = std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
                resolveGenericParameters(declaration, std::nullopt);
                resolveFunction(declaration);
                break;
            }
            case syntax::Kind::protocolDecl:
                resolveProtocol(std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node));
                break;
            case syntax::Kind::structDecl: {
                const auto declaration = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                resolveStructure(declaration);
                for (const auto& method : declaration->methods) resolveMethod(method);
                if (declaration->deinitializer != nullptr) resolveFunction(declaration->deinitializer);
                break;
            }
            case syntax::Kind::enumDecl: {
                const auto declaration = std::static_pointer_cast<syntax::EnumDeclSyntax>(node);
                resolveEnumeration(declaration);
                for (const auto& method : declaration->methods) resolveMethod(method);
                break;
            }
            case syntax::Kind::extensionDecl: {
                const auto declaration = std::static_pointer_cast<syntax::ExtensionDeclSyntax>(node);
                if (!declaration->conformances.empty()) (void) resolveType(declaration->extendedType);
                for (const auto& method : declaration->methods) resolveMethod(method);
                break;
            }
            default:
                break;
        }
    }

    // ---- Protocols and generic functions (spec 20, decision 0021) ----

    const semantic::Symbol* symbolOf(semantic::SymbolId id) const {
        return model->semanticModelValue->symbol(id);
    }

    std::string symbolName(semantic::SymbolId id) const {
        const auto* symbol = symbolOf(id);
        return symbol == nullptr ? std::string("<unknown>") : symbol->name;
    }

    bool checkProtocolReference(const syntax::TypePtr& reference) {
        if (reference->kind == syntax::Kind::nominalType &&
            std::static_pointer_cast<syntax::NominalTypeSyntax>(reference)->arguments.empty()) return true;
        report(TypeCheckingDiagnosticId::invalidConformance, reference->span,
               "a protocol reference must name a protocol without type arguments (spec 20.2)");
        return false;
    }

    // Records `T: P & Q`: the parameters of a top-level generic function and each parameter's
    // constraint. A method, `init` or requirement with type parameters is reported by the caller's
    // `misplaced` diagnostic, but its parameters still get constraints so their types resolve.
    void resolveGenericParameters(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            std::optional<TypeCheckingDiagnosticId> misplaced) {
        if (declaration->genericParameters.empty()) return;
        const auto function = model->semanticModelValue->declaredSymbol(declaration);
        if (!function.has_value()) return;
        const auto clause = coveringSpan(declaration->genericParameters.front()->span,
                                         declaration->genericParameters.back()->span);
        if (misplaced.has_value()) {
            report(*misplaced, clause,
                   *misplaced == TypeCheckingDiagnosticId::invalidProtocolMember
                           ? "a protocol requirement cannot declare type parameters"
                           : "only a top-level function can declare type parameters (spec 20.4)",
                   "generic methods, structs and enums are not supported yet (spec 20.10)");
        }
        for (const auto& attribute : declaration->attributes) {
            const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
            if (name == "c_extern" || name == "c_export" || name == "c_data") {
                report(TypeCheckingDiagnosticId::genericNativeBinding, clause,
                       "a C binding cannot declare type parameters (spec 20.7)",
                       "write a non-generic binding and call it from generic code");
                break;
            }
        }
        std::vector<semantic::SymbolId> parameters;
        for (const auto& parameter : declaration->genericParameters) {
            const auto symbol = model->semanticModelValue->declaredSymbol(parameter);
            if (!symbol.has_value()) continue;
            parameters.push_back(*symbol);
            if (parameter->name != nullptr && parameter->name->rawValue == "Copyable") {
                report(TypeCheckingDiagnosticId::reservedProtocolName, parameter->name->span,
                       "'Copyable' is reserved for the builtin protocol (spec 20.5)");
            }
            std::vector<semantic::SymbolId> protocols;
            for (const auto& constraint : parameter->constraints) {
                if (!checkProtocolReference(constraint)) continue;
                const auto referenced = model->semanticModelValue->referencedSymbol(constraint);
                if (!referenced.has_value()) continue;
                const auto* protocol = symbolOf(*referenced);
                if (protocol == nullptr || protocol->kind != semantic::SymbolKind::protocol) {
                    report(TypeCheckingDiagnosticId::invalidConformance, constraint->span,
                           "'" + symbolName(*referenced) + "' is not a protocol; a constraint lists protocols");
                    continue;
                }
                if (std::find(protocols.begin(), protocols.end(), *referenced) != protocols.end()) {
                    report(TypeCheckingDiagnosticId::invalidConformance, constraint->span,
                           "the constraint of '" + symbolName(*symbol) + "' names '" +
                                   protocol->name + "' twice");
                    continue;
                }
                protocols.push_back(*referenced);
            }
            model->constraintLists[*symbol] = std::move(protocols);
            (void) model->typeContext.typeParameterType(*symbol);
        }
        constrainedParameters.insert(constrainedParameters.end(), parameters.begin(), parameters.end());
        if (!misplaced.has_value()) model->genericParameterLists[*function] = std::move(parameters);
    }

    static DiagnosticSourceNote sourceNote(SourceSpan span, std::string message) {
        return DiagnosticSourceNote { span.offset, span.length, std::move(message) };
    }

    static SourceSpan coveringSpan(SourceSpan first, SourceSpan last) {
        if (first.sourceId != last.sourceId || last.offset < first.offset) return first;
        first.length = last.offset + last.length - first.offset;
        return first;
    }

    void resolveProtocol(const syntax::ProtocolDeclSyntax::Ptr& declaration) {
        const auto name = declaration->name == nullptr ? std::string() : declaration->name->rawValue;
        if (name == "Copyable" || name == "Display" || name == "Equatable" || name == "Comparable" ||
            name == "Any") {
            report(TypeCheckingDiagnosticId::reservedProtocolName,
                   declaration->name == nullptr ? declaration->span : declaration->name->span,
                   "'" + name + "' is reserved and cannot be declared (spec 20.2)",
                   name == "Copyable" ? std::optional<std::string>("'Copyable' is the builtin protocol of spec 20.5")
                                      : std::nullopt);
        }
        for (const auto& requirement : declaration->requirements) {
            if (requirement->body != nullptr) {
                report(TypeCheckingDiagnosticId::invalidProtocolMember, requirement->body->span,
                       "a protocol requirement has no body (spec 20.2)",
                       "implement the method in a conformance extension");
            }
            if (requirement->explicitVisibility) {
                report(TypeCheckingDiagnosticId::invalidProtocolMember, requirement->span,
                       "a protocol requirement has no visibility modifier; it has the protocol's");
            }
            resolveGenericParameters(requirement, TypeCheckingDiagnosticId::invalidProtocolMember);
            resolveFunction(requirement);
        }
    }

    // What must match between a requirement and its witness (spec 20.3), with `Self` replaced.
    struct MethodShape {
        syntax::AccessEffect receiver = syntax::AccessEffect::borrowing;
        std::vector<std::optional<std::string>> labels;
        std::vector<syntax::AccessEffect> effects;
        std::vector<TypeId> parameters;
        TypeId result = invalidTypeId;
    };

    std::optional<MethodShape> methodShape(
            semantic::SymbolId method,
            const std::unordered_map<semantic::SymbolId, TypeId>& substitution) {
        const auto* symbol = symbolOf(method);
        const auto* signature = model->callable(method);
        if (symbol == nullptr || signature == nullptr || !symbol->callable.has_value()) return std::nullopt;
        MethodShape shape;
        if (symbol->callable->receiver.has_value()) shape.receiver = symbol->callable->receiver->access;
        for (const auto& parameter : symbol->callable->parameters) {
            shape.labels.push_back(parameter.label);
            shape.effects.push_back(parameter.access);
        }
        for (const auto parameter : signature->parameters) {
            shape.parameters.push_back(model->typeContext.substitute(parameter, substitution));
        }
        shape.result = model->typeContext.substitute(signature->result, substitution);
        return shape;
    }

    static const char* effectWord(syntax::AccessEffect effect) {
        switch (effect) {
            case syntax::AccessEffect::inout: return "inout";
            case syntax::AccessEffect::consuming: return "consuming";
            case syntax::AccessEffect::initializing: return "initializing";
            case syntax::AccessEffect::borrowing: return "borrowing";
        }
        return "borrowing";
    }

    // Why `witness` does not match `requirement`, or nullopt when it matches.
    std::optional<std::string> shapeMismatch(const MethodShape& requirement, const MethodShape& witness) const {
        if (requirement.receiver != witness.receiver) {
            return std::string("its receiver is ") + effectWord(witness.receiver) + ", the requirement's is " +
                    effectWord(requirement.receiver);
        }
        if (requirement.parameters.size() != witness.parameters.size()) {
            return "it takes " + std::to_string(witness.parameters.size()) + " parameter(s), the requirement " +
                    std::to_string(requirement.parameters.size());
        }
        for (size_t index = 0; index < requirement.parameters.size(); ++index) {
            const auto position = "parameter " + std::to_string(index + 1);
            if (requirement.labels[index] != witness.labels[index]) {
                return position + " is labeled '" + witness.labels[index].value_or("") + "', the requirement's '" +
                        requirement.labels[index].value_or("") + "'";
            }
            if (requirement.effects[index] != witness.effects[index]) {
                return position + " is " + effectWord(witness.effects[index]) + ", the requirement's is " +
                        effectWord(requirement.effects[index]);
            }
            if (requirement.parameters[index] != witness.parameters[index]) {
                return position + " has type '" + model->typeContext.displayName(witness.parameters[index]) +
                        "', the requirement's '" + model->typeContext.displayName(requirement.parameters[index]) + "'";
            }
        }
        if (requirement.result != witness.result) {
            return "it returns '" + model->typeContext.displayName(witness.result) + "', the requirement '" +
                    model->typeContext.displayName(requirement.result) + "'";
        }
        return std::nullopt;
    }

    std::vector<semantic::SymbolId> requirementsOf(semantic::SymbolId protocol) const {
        std::vector<semantic::SymbolId> requirements;
        const auto* symbol = symbolOf(protocol);
        if (symbol == nullptr || !symbol->declaration.has_value()) return requirements;
        const auto node = model->semanticModelValue->node(*symbol->declaration);
        if (node == nullptr || node->kind != syntax::Kind::protocolDecl) return requirements;
        for (const auto& requirement : std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node)->requirements) {
            const auto id = model->semanticModelValue->declaredSymbol(requirement);
            if (id.has_value()) requirements.push_back(*id);
        }
        return requirements;
    }

    std::string requirementTitle(semantic::SymbolId requirement) const {
        const auto* symbol = symbolOf(requirement);
        if (symbol == nullptr) return "<requirement>";
        std::string title = symbol->name + "(";
        if (symbol->callable.has_value()) {
            for (const auto& parameter : symbol->callable->parameters) title += parameter.label.value_or("_") + ":";
        }
        return title + ")";
    }

    // Two protocols of one constraint that require one name must require the same method
    // (spec 20.5): witnesses are found by name.
    void checkConstraintConflicts() {
        for (const auto parameter : constrainedParameters) {
            const auto& protocols = model->constraints(parameter);
            if (protocols.size() < 2) continue;
            const std::unordered_map<semantic::SymbolId, TypeId> none;
            std::unordered_map<std::string, std::pair<semantic::SymbolId, semantic::SymbolId>> seen;
            for (const auto protocol : protocols) {
                const std::unordered_map<semantic::SymbolId, TypeId> self {
                    { protocol, model->typeContext.typeParameterType(parameter) } };
                for (const auto requirement : requirementsOf(protocol)) {
                    const auto name = symbolName(requirement);
                    const auto previous = seen.find(name);
                    if (previous == seen.end()) {
                        seen.emplace(name, std::pair { protocol, requirement });
                        continue;
                    }
                    const std::unordered_map<semantic::SymbolId, TypeId> previousSelf {
                        { previous->second.first, model->typeContext.typeParameterType(parameter) } };
                    const auto first = methodShape(previous->second.second, previousSelf);
                    const auto second = methodShape(requirement, self);
                    if (!first.has_value() || !second.has_value() || !shapeMismatch(*first, *second).has_value()) {
                        continue;
                    }
                    report(TypeCheckingDiagnosticId::invalidConformance, symbolOf(parameter)->span,
                           "protocols '" + symbolName(previous->second.first) + "' and '" + symbolName(protocol) +
                                   "' both require '" + name + "' with different signatures, so no type can "
                                   "satisfy the constraint of '" + symbolName(parameter) + "'");
                }
            }
        }
    }

    // Spec 20.3: each requirement has exactly one matching, accessible witness, and at most one
    // conformance of a type to a protocol exists.
    void checkConformances(const syntax::ExtensionDeclSyntax::Ptr& extension) {
        if (extension->conformances.empty()) return;
        const auto extended = model->typeOf(extension->extendedType);
        const auto typeSymbolId = model->semanticModelValue->referencedSymbol(extension->extendedType);
        if (!extended.has_value() || !typeSymbolId.has_value() || *extended == model->typeContext.errorType()) return;
        const auto* typeSymbol = symbolOf(*typeSymbolId);
        if (typeSymbol == nullptr) return;
        const auto copyable = model->copyableProtocol();
        const auto module = model->semanticModelValue->moduleOf(extension->span);
        std::vector<semantic::SymbolId> listed;
        for (const auto& conformance : extension->conformances) {
            if (!checkProtocolReference(conformance)) continue;
            const auto referenced = model->semanticModelValue->referencedSymbol(conformance);
            if (!referenced.has_value()) continue;
            const auto* protocol = symbolOf(*referenced);
            if (protocol == nullptr || protocol->kind != semantic::SymbolKind::protocol) {
                report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                       "'" + symbolName(*referenced) + "' is not a protocol; an extension can only conform to protocols");
                continue;
            }
            if (copyable.has_value() && *referenced == *copyable) {
                report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                       "'Copyable' cannot be declared; a type is copyable exactly when spec 4.1.1 says so");
                continue;
            }
            if (std::find(listed.begin(), listed.end(), *referenced) != listed.end()) {
                report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                       "'" + protocol->name + "' is listed twice");
                continue;
            }
            listed.push_back(*referenced);
            if (module != typeSymbol->moduleId && module != protocol->moduleId) {
                report(TypeCheckingDiagnosticId::orphanConformance, conformance->span,
                       "a conformance of '" + typeSymbol->name + "' to '" + protocol->name +
                               "' must be declared in the module of the type or of the protocol (spec 20.3)");
                continue;
            }
            const auto duplicate = std::find_if(model->conformanceList.begin(), model->conformanceList.end(),
                    [&](const Conformance& existing) {
                        return existing.type == *extended && existing.protocol == *referenced;
                    });
            if (duplicate != model->conformanceList.end()) {
                report(TypeCheckingDiagnosticId::duplicateConformance, conformance->span,
                       "'" + typeSymbol->name + "' already conforms to '" + protocol->name + "'",
                       std::nullopt, std::nullopt,
                       { sourceNote(duplicate->span, "the first conformance is here") });
                continue;
            }
            Conformance result { *extended, *referenced, {}, conformance->span };
            const auto conformanceVisibility = std::min(protocol->visibility, typeSymbol->visibility);
            const std::unordered_map<semantic::SymbolId, TypeId> self { { *referenced, *extended } };
            bool valid = true;
            for (const auto requirement : requirementsOf(*referenced)) {
                const auto witness = lookupMember(*extended, symbolName(requirement));
                const auto* witnessSymbol = witness.has_value() ? symbolOf(*witness) : nullptr;
                if (witnessSymbol == nullptr || witnessSymbol->kind != semantic::SymbolKind::method) {
                    report(TypeCheckingDiagnosticId::missingWitness, conformance->span,
                           "'" + typeSymbol->name + "' does not implement '" + requirementTitle(requirement) +
                                   "' required by '" + protocol->name + "'",
                           witnessSymbol != nullptr
                                   ? std::optional<std::string>("a builtin member is not a declared method and cannot be a witness")
                                   : std::optional<std::string>("declare the method in this extension"));
                    valid = false;
                    continue;
                }
                const auto required = methodShape(requirement, self);
                const auto provided = methodShape(*witness, {});
                if (!required.has_value() || !provided.has_value()) { valid = false; continue; }
                if (const auto reason = shapeMismatch(*required, *provided)) {
                    report(TypeCheckingDiagnosticId::witnessMismatch, witnessSymbol->span,
                           "'" + typeSymbol->name + "." + witnessSymbol->name + "' does not match '" +
                                   requirementTitle(requirement) + "' of '" + protocol->name + "': " + *reason,
                           std::nullopt, std::nullopt,
                           { sourceNote(symbolOf(requirement)->span, "the requirement is here") });
                    valid = false;
                    continue;
                }
                if (std::min(witnessSymbol->visibility, typeSymbol->visibility) < conformanceVisibility ||
                    !model->semanticModelValue->isAccessible(*witness, conformance->span)) {
                    report(TypeCheckingDiagnosticId::witnessNotAccessible, witnessSymbol->span,
                           "'" + typeSymbol->name + "." + witnessSymbol->name +
                                   "' is less accessible than the conformance to '" + protocol->name + "'",
                           "give the method the visibility of the protocol and the type");
                    valid = false;
                    continue;
                }
                result.witnesses.emplace(requirement, *witness);
            }
            // An invalid conformance is still recorded, so a call does not also report it as missing.
            (void) valid;
            model->conformanceList.push_back(std::move(result));
        }
    }

    // Whether `type` satisfies `protocol` (spec 20.4.2). Inside a generic body a type that mentions
    // a parameter satisfies a user protocol only when it is that parameter and its constraint
    // lists the protocol.
    bool satisfies(TypeId type, semantic::SymbolId protocol) const {
        if (type == model->typeContext.errorType()) return true;
        const auto copyable = model->copyableProtocol();
        if (copyable.has_value() && protocol == *copyable) return model->isCopyable(type);
        const auto* record = model->typeContext.type(type);
        if (record == nullptr) return false;
        if (record->kind == TypeKind::typeParameter) {
            const auto& protocols = model->constraints(record->symbol);
            return std::find(protocols.begin(), protocols.end(), protocol) != protocols.end();
        }
        if (model->typeContext.mentionsTypeParameter(type)) return false;
        return std::any_of(model->conformanceList.begin(), model->conformanceList.end(),
                [&](const Conformance& conformance) {
                    return conformance.type == type && conformance.protocol == protocol;
                });
    }

    enum class ArgumentInformation { typed, literal, none };

    // Spec 20.4.1: literals are unified after typed arguments; `nil`, `[]`, `[:]`, `.Case` say nothing.
    static ArgumentInformation argumentInformation(const syntax::ExprPtr& value) {
        if (value == nullptr) return ArgumentInformation::none;
        switch (value->kind) {
            case syntax::Kind::parenthesizedExpr:
                return argumentInformation(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(value)->expression);
            case syntax::Kind::literalExpr: {
                const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(value);
                if (literal->literal != nullptr && literal->literal->kind == nilLiteral) {
                    return ArgumentInformation::none;
                }
                return ArgumentInformation::literal;
            }
            case syntax::Kind::prefixExpr: {
                const auto prefix = std::static_pointer_cast<syntax::PrefixExprSyntax>(value);
                if (prefix->castTarget == nullptr && prefix->op != nullptr && prefix->op->kind == minus &&
                    prefix->operand != nullptr && prefix->operand->kind == syntax::Kind::literalExpr) {
                    return ArgumentInformation::literal;
                }
                return ArgumentInformation::typed;
            }
            case syntax::Kind::arrayExpr:
                return std::static_pointer_cast<syntax::ArrayExprSyntax>(value)->elements.empty()
                        ? ArgumentInformation::none : ArgumentInformation::typed;
            case syntax::Kind::dictionaryExpr:
                return std::static_pointer_cast<syntax::DictionaryExprSyntax>(value)->entries.empty()
                        ? ArgumentInformation::none : ArgumentInformation::typed;
            case syntax::Kind::contextualCaseExpr:
                return ArgumentInformation::none;
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(value);
                return call->callee != nullptr && call->callee->kind == syntax::Kind::contextualCaseExpr
                        ? ArgumentInformation::none : ArgumentInformation::typed;
            }
            default:
                return ArgumentInformation::typed;
        }
    }

    struct Inference {
        const std::vector<semantic::SymbolId>& parameters;
        std::unordered_map<semantic::SymbolId, TypeId> bindings;
        std::unordered_set<semantic::SymbolId> conflicted;
    };

    bool mentionsAny(TypeId type, const std::vector<semantic::SymbolId>& parameters) const {
        const auto* record = model->typeContext.type(type);
        if (record == nullptr) return false;
        if (record->kind == TypeKind::typeParameter) {
            return std::find(parameters.begin(), parameters.end(), record->symbol) != parameters.end();
        }
        const auto arguments = record->arguments;
        return std::any_of(arguments.begin(), arguments.end(),
                [&](TypeId argument) { return mentionsAny(argument, parameters); });
    }

    // Structural unification of a parameter type with an argument type (spec 20.4.1). A mismatch
    // binds nothing; the ordinary argument check reports it afterwards.
    void unify(TypeId parameter, TypeId argument, Inference& inference, SourceSpan span) {
        if (argument == model->typeContext.errorType()) return;
        const auto* record = model->typeContext.type(parameter);
        if (record == nullptr) return;
        if (record->kind == TypeKind::typeParameter &&
            std::find(inference.parameters.begin(), inference.parameters.end(), record->symbol) !=
                    inference.parameters.end()) {
            const auto symbol = record->symbol;
            const auto bound = inference.bindings.find(symbol);
            if (bound == inference.bindings.end()) {
                inference.bindings.emplace(symbol, argument);
            } else if (bound->second != argument && !inference.conflicted.contains(symbol)) {
                inference.conflicted.insert(symbol);
                report(TypeCheckingDiagnosticId::conflictingTypeArgument, span,
                       "type parameter '" + symbolName(symbol) + "' is inferred as both '" +
                               model->typeContext.displayName(bound->second) + "' and '" +
                               model->typeContext.displayName(argument) + "'",
                       "every use of one type parameter must have one type (spec 20.4.1)");
            }
            return;
        }
        if (!mentionsAny(parameter, inference.parameters)) return;
        const auto* actual = model->typeContext.type(argument);
        if (actual == nullptr || actual->kind != record->kind || actual->symbol != record->symbol ||
            actual->arguments.size() != record->arguments.size() || actual->effects != record->effects) {
            return;
        }
        const auto parameterArguments = record->arguments;
        const auto argumentArguments = actual->arguments;
        for (size_t index = 0; index < parameterArguments.size(); ++index) {
            unify(parameterArguments[index], argumentArguments[index], inference, span);
        }
    }

    // Infers the type arguments of a call of a generic function, checks their constraints, records
    // the call, and returns the signature with the arguments substituted. Arguments checked here
    // are marked in `checked`, so the caller does not check them again.
    TypedCallableSignature inferGenericCall(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId target,
            const semantic::Symbol& targetSymbol,
            const TypedCallableSignature& signature,
            const std::vector<semantic::SymbolId>& parameters,
            std::vector<std::optional<std::optional<TypeId>>>& checked) {
        Inference inference { parameters, {}, {} };
        std::vector<std::optional<size_t>> parameterIndices;
        for (size_t index = 0; index < expression->arguments.size(); ++index) {
            parameterIndices.push_back(
                    parameterIndexForArgument(targetSymbol, *expression->arguments[index], index));
        }
        const auto parameterType = [&](size_t index) -> std::optional<TypeId> {
            const auto parameter = parameterIndices[index];
            if (!parameter.has_value() || *parameter >= signature.parameters.size()) return std::nullopt;
            return signature.parameters[*parameter];
        };
        std::vector<size_t> order;
        for (size_t index = 0; index < expression->arguments.size(); ++index) order.push_back(index);
        const auto floatingLiteral = [&](size_t index) {
            auto value = expression->arguments[index]->value;
            while (value != nullptr && value->kind == syntax::Kind::parenthesizedExpr) {
                value = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(value)->expression;
            }
            if (value != nullptr && value->kind == syntax::Kind::prefixExpr) {
                value = std::static_pointer_cast<syntax::PrefixExprSyntax>(value)->operand;
            }
            return value != nullptr && value->kind == syntax::Kind::literalExpr &&
                    std::static_pointer_cast<syntax::LiteralExprSyntax>(value)->literal->kind == floatLiteral;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            return floatingLiteral(left) && !floatingLiteral(right);
        });
        for (const auto pass : { ArgumentInformation::typed, ArgumentInformation::literal }) {
            for (const auto index : order) {
                const auto& argument = expression->arguments[index];
                const auto type = parameterType(index);
                if (!type.has_value() || !mentionsAny(*type, parameters) ||
                    argumentInformation(argument->value) != pass) {
                    continue;
                }
                const auto expected = model->typeContext.substitute(*type, inference.bindings);
                const auto actual = checkExpression(
                        argument->value,
                        pass == ArgumentInformation::literal && !mentionsAny(expected, parameters)
                                ? std::optional<TypeId>(expected) : std::optional<TypeId>());
                checked[index] = actual;
                if (actual.has_value() && *actual != model->typeContext.neverType()) {
                    unify(*type, *actual, inference, argument->value->span);
                }
            }
        }
        std::vector<TypeId> arguments;
        for (const auto parameter : parameters) {
            const auto bound = inference.bindings.find(parameter);
            if (bound != inference.bindings.end()) {
                arguments.push_back(inference.conflicted.contains(parameter)
                        ? model->typeContext.errorType() : bound->second);
                continue;
            }
            report(TypeCheckingDiagnosticId::cannotInferTypeParameter, expression->span,
                   "cannot infer type parameter '" + symbolName(parameter) + "' of '" + targetSymbol.name +
                           "' from the arguments",
                   "pass an argument whose type determines it; 'nil', '[]', '.Case' and the result type do not "
                   "(spec 20.4.1)");
            arguments.push_back(model->typeContext.errorType());
        }
        std::unordered_map<semantic::SymbolId, TypeId> substitution;
        for (size_t index = 0; index < parameters.size(); ++index) {
            substitution.emplace(parameters[index], arguments[index]);
        }
        checkTypeArguments(expression, targetSymbol, parameters, arguments);
        model->genericCalls[*model->semanticModelValue->nodeId(expression)] = GenericCall {
            target, arguments, currentFunction, expression->span };
        TypedCallableSignature result = signature;
        for (auto& parameter : result.parameters) parameter = model->typeContext.substitute(parameter, substitution);
        result.result = model->typeContext.substitute(result.result, substitution);
        return result;
    }

    void checkTypeArguments(
            const syntax::CallExprSyntax::Ptr& expression,
            const semantic::Symbol& targetSymbol,
            const std::vector<semantic::SymbolId>& parameters,
            const std::vector<TypeId>& arguments) {
        const auto copyable = model->copyableProtocol();
        for (size_t index = 0; index < parameters.size(); ++index) {
            const auto argument = arguments[index];
            if (argument == model->typeContext.errorType()) continue;
            const auto parameter = symbolName(parameters[index]);
            const auto argumentName = "'" + model->typeContext.displayName(argument) + "'";
            if (argument == model->typeContext.neverType()) {
                report(TypeCheckingDiagnosticId::invalidTypeArgument, expression->span,
                       "type parameter '" + parameter + "' of '" + targetSymbol.name + "' cannot be 'Never'",
                       "'Never' has no values, so a generic function cannot take or hold one (spec 2.9)");
                continue;
            }
            if (model->isLinear(argument)) {
                report(TypeCheckingDiagnosticId::linearTypeArgument, expression->span,
                       "type parameter '" + parameter + "' of '" + targetSymbol.name + "' cannot be the linear type " +
                               argumentName,
                       "generic code may drop a value of a type parameter, and a linear value must be ended "
                       "(spec 4.1.1, 20.5)");
                continue;
            }
            for (const auto protocol : model->constraints(parameters[index])) {
                if (satisfies(argument, protocol)) continue;
                const bool isCopyable = copyable.has_value() && protocol == *copyable;
                std::optional<std::string> help;
                if (isCopyable) {
                    if (const auto reason = model->noncopyableReason(argument)) help = *reason;
                } else if (model->typeContext.mentionsTypeParameter(argument)) {
                    help = "inside a generic function, only a type parameter whose constraint lists '" +
                            symbolName(protocol) + "' satisfies it";
                } else {
                    help = "declare 'extension " + model->typeContext.displayName(argument) + ": " +
                            symbolName(protocol) + " { ... }'";
                }
                report(TypeCheckingDiagnosticId::unsatisfiedConstraint, expression->span,
                       "type " + argumentName + " does not satisfy '" + symbolName(protocol) +
                               "', required by type parameter '" + parameter + "' of '" + targetSymbol.name + "'",
                       help);
            }
        }
    }

    // Spec 20.6: a cycle of generic calls in which a type parameter is passed on inside a larger
    // type would need infinitely many instances. Edges run from a caller's parameter to the
    // callee's parameter it reaches; an edge is expanding when the argument strictly contains it.
    void checkExpandingRecursion() {
        struct Edge {
            semantic::SymbolId from;
            semantic::SymbolId to;
            bool expanding;
            SourceSpan span;
        };
        std::vector<Edge> edges;
        std::unordered_map<semantic::SymbolId, std::vector<size_t>> outgoing;
        std::vector<std::pair<semantic::NodeId, const GenericCall*>> calls;
        for (const auto& [node, call] : model->genericCalls) calls.emplace_back(node, &call);
        std::sort(calls.begin(), calls.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
        for (const auto& [node, call] : calls) {
            if (!call->caller.has_value()) continue;
            const auto* callerParameters = model->genericParameters(*call->caller);
            const auto* calleeParameters = model->genericParameters(call->callee);
            if (callerParameters == nullptr || calleeParameters == nullptr) continue;
            for (size_t index = 0; index < call->arguments.size() && index < calleeParameters->size(); ++index) {
                const auto argument = call->arguments[index];
                for (const auto parameter : *callerParameters) {
                    if (!mentionsAny(argument, { parameter })) continue;
                    const auto* record = model->typeContext.type(argument);
                    const bool direct = record != nullptr && record->kind == TypeKind::typeParameter;
                    outgoing[parameter].push_back(edges.size());
                    edges.push_back({ parameter, (*calleeParameters)[index], !direct, call->span });
                }
            }
        }
        // An expanding edge from `a` to `b` closes a cycle when `a` is reachable from `b`.
        for (const auto& edge : edges) {
            if (!edge.expanding) continue;
            std::vector<semantic::SymbolId> stack { edge.to };
            std::unordered_set<semantic::SymbolId> visited { edge.to };
            bool cycle = edge.to == edge.from;
            while (!stack.empty() && !cycle) {
                const auto current = stack.back();
                stack.pop_back();
                for (const auto next : outgoing[current]) {
                    const auto target = edges[next].to;
                    if (target == edge.from) { cycle = true; break; }
                    if (visited.insert(target).second) stack.push_back(target);
                }
            }
            if (!cycle) continue;
            report(TypeCheckingDiagnosticId::expandingGenericRecursion, edge.span,
                   "this call passes type parameter '" + symbolName(edge.from) +
                           "' on inside a larger type in a cycle of generic calls, which would need "
                           "infinitely many instances (spec 20.6)",
                   "make the recursion keep the same type arguments, or move it into a non-generic function");
            return;
        }
    }

    // Decision 0021: the instances reachable from non-generic code, each once, with a cap as a
    // backstop for programs the expanding-recursion rule allows but that are still huge.
    static constexpr size_t maximumInstances = 10000;
    static constexpr size_t maximumTypeNesting = 64;
    static constexpr size_t maximumInstanceDepth = 64;
    static constexpr size_t maximumTypeOccurrences = 4096;

    void instantiate() {
        std::vector<std::pair<semantic::NodeId, const GenericCall*>> calls;
        for (const auto& [node, call] : model->genericCalls) calls.emplace_back(node, &call);
        std::sort(calls.begin(), calls.end(), [](const auto& left, const auto& right) {
            return left.second->span.sourceId != right.second->span.sourceId
                    ? left.second->span.sourceId < right.second->span.sourceId
                    : left.second->span.offset < right.second->span.offset;
        });
        std::unordered_map<semantic::SymbolId, std::vector<const GenericCall*>> callsIn;
        std::vector<const GenericCall*> roots;
        for (const auto& [node, call] : calls) {
            if (call->caller.has_value() && model->genericParameters(*call->caller) != nullptr) {
                callsIn[*call->caller].push_back(call);
            }
            else roots.push_back(call);
        }
        // Breadth first, so an instance's "required by" is the shortest chain from a root.
        std::vector<size_t> queue;
        const auto require = [&](const GenericCall& call, std::vector<TypeId> arguments,
                                 std::optional<size_t> requiredBy) -> bool {
            for (const auto argument : arguments) {
                std::vector<TypeId> pending { argument };
                size_t occurrences = 0;
                while (!pending.empty() && occurrences <= maximumTypeOccurrences) {
                    const auto* type = model->typeContext.type(pending.back());
                    pending.pop_back();
                    ++occurrences;
                    if (type != nullptr) pending.insert(pending.end(), type->arguments.begin(), type->arguments.end());
                }
                if (occurrences > maximumTypeOccurrences) {
                    report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                           "this call needs an instance whose type arguments contain more than " +
                                   std::to_string(maximumTypeOccurrences) + " type occurrences",
                           std::nullopt, std::nullopt, instantiationChain(requiredBy));
                    return false;
                }
                if (model->typeContext.nestingDepth(argument) > maximumTypeNesting) {
                    report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                           "this call needs an instance of '" + symbolName(call.callee) +
                                   "' whose type arguments nest more than " + std::to_string(maximumTypeNesting) +
                                   " levels deep",
                           std::nullopt, std::nullopt, instantiationChain(requiredBy));
                    return false;
                }
            }
            const auto depth = requiredBy.has_value() ? model->instanceList[*requiredBy].depth + 1 : 1;
            if (const auto existing = model->instanceIndices.find({call.callee, arguments});
                existing != model->instanceIndices.end()) {
                auto& instance = model->instanceList[existing->second];
                const auto before = call.span.sourceId < instance.requiredAt.sourceId ||
                        (call.span.sourceId == instance.requiredAt.sourceId &&
                         call.span.offset < instance.requiredAt.offset);
                if (depth < instance.depth || (depth == instance.depth && before)) {
                    instance.depth = depth;
                    instance.requiredAt = call.span;
                    instance.requiredBy = requiredBy;
                    instance.requiredByFunction = requiredBy.has_value()
                            ? std::optional<semantic::SymbolId>(model->instanceList[*requiredBy].function)
                            : call.caller;
                }
                return true;
            }
            if (depth > maximumInstanceDepth) {
                report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                       "this call needs an instantiation chain longer than " +
                               std::to_string(maximumInstanceDepth),
                       std::nullopt, std::nullopt, instantiationChain(requiredBy));
                return false;
            }
            if (model->instanceList.size() >= maximumInstances) {
                report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                       "the program needs more than " + std::to_string(maximumInstances) +
                               " instances of generic functions",
                       std::nullopt, std::nullopt, instantiationChain(requiredBy));
                return false;
            }
            Instance instance;
            instance.function = call.callee;
            instance.arguments = arguments;
            const auto* parameters = model->genericParameters(call.callee);
            std::string name = model->functionDisplayName(call.callee) + "<";
            for (size_t index = 0; parameters != nullptr && index < parameters->size(); ++index) {
                instance.substitution.emplace((*parameters)[index], arguments[index]);
                if (index != 0) name += ", ";
                name += model->typeContext.displayName(arguments[index], true);
            }
            instance.name = name + ">";
            instance.requiredAt = call.span;
            instance.requiredBy = requiredBy;
            instance.depth = depth;
            instance.requiredByFunction = requiredBy.has_value()
                    ? std::optional<semantic::SymbolId>(model->instanceList[*requiredBy].function)
                    : call.caller;
            model->instanceIndices.emplace(std::pair { call.callee, arguments }, model->instanceList.size());
            queue.push_back(model->instanceList.size());
            model->instanceList.push_back(std::move(instance));
            return true;
        };
        for (const auto* call : roots) {
            if (!require(*call, call->arguments, std::nullopt)) return;
        }
        for (size_t next = 0; next < queue.size(); ++next) {
            const auto index = queue[next];
            const auto function = model->instanceList[index].function;
            for (const auto* call : callsIn[function]) {
                std::vector<TypeId> arguments;
                for (const auto argument : call->arguments) {
                    arguments.push_back(model->typeContext.substitute(argument, model->instanceList[index].substitution));
                }
                if (!require(*call, std::move(arguments), index)) return;
            }
        }
        internInstanceTypes();
    }

    // Lowering substitutes every type it reads, and the IR type table is a snapshot taken before
    // it starts, so each instance's concrete types are interned here.
    void internInstanceTypes() {
        const auto known = model->typeContext.size();
        for (TypeId id = 0; id < known; ++id) {
            if (!model->typeContext.mentionsTypeParameter(id)) continue;
            for (size_t index = 0; index < model->instanceList.size(); ++index) {
                (void) model->typeContext.substitute(id, model->instanceList[index].substitution);
            }
        }
    }

    std::vector<DiagnosticSourceNote> instantiationChain(std::optional<size_t> instance) const {
        std::vector<DiagnosticSourceNote> chain;
        while (instance.has_value()) {
            const auto& current = model->instanceList[*instance];
            chain.push_back(sourceNote(current.requiredAt, "required for '" + current.name + "'"));
            instance = current.requiredBy;
        }
        // The first three and the last three steps are enough to see the shape of a chain.
        if (chain.size() > 6) chain.erase(chain.begin() + 3, chain.end() - 3);
        return chain;
    }

    void resolveBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto type = resolveType(declaration->annotation);
        if (type.has_value() && containsFunctionType(*type)) {
            report(TypeCheckingDiagnosticId::invalidFunctionType, declaration->annotation->span,
                   "a top-level constant cannot have the function type '" + model->typeContext.displayName(*type) + "'",
                   "name the function directly, or bind the value to a local 'let'");
        }
        if (type.has_value()) recordDeclaredType(declaration, *type);
    }

    void resolveMethod(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        resolveGenericParameters(declaration, TypeCheckingDiagnosticId::invalidGenericDeclaration);
        resolveFunction(declaration);
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
        const bool literalAddress = declaresLiteralAddress(*declaration);
        for (const auto& parameter : declaration->parameters) {
            // The literal-address intrinsic's own String parameter is not a
            // value a core unit builds; every other declaration is checked.
            coreCheckSuspended = literalAddress;
            auto type = resolveType(parameter->type).value_or(
                    model->typeContext.errorType());
            if (type == model->typeContext.neverType() && !parameter->isReceiver) {
                report(TypeCheckingDiagnosticId::invalidFunctionType, parameter->type->span,
                       "a parameter cannot have type 'Never'",
                       "a function taking a value that cannot exist could never be called");
                type = model->typeContext.errorType();
            }
            recordDeclaredType(parameter, type);
            coreCheckSuspended = false;
            if (parameter->isReceiver) {
                signature.receiverType = type;
                continue;
            }
            signature.parameters.push_back(type);
        }
        if (declaration->returnType != nullptr) {
            signature.result = resolveType(declaration->returnType).value_or(
                    model->typeContext.errorType());
            requireCoreType(declaration->returnType, signature.result);
        }

        model->symbolTypes[*function] = signature.result;
        model->callables[*function] = std::move(signature);
        resolveNativeBinding(declaration, *function);
    }

    void resolveNativeBinding(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            semantic::SymbolId function) {
        if (declaration->attributes.empty()) return;
        const auto diagnosticStart = diagnostics.size();
        std::optional<NativeBinding> binding;
        for (const auto& attribute : declaration->attributes) {
            const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
            if (name != "c_extern" && name != "c_export" && name != "c_data") {
                report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                       "unknown annotation '@" + name + "'");
                continue;
            }
            if (binding.has_value()) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                       "a function may have only one C binding annotation");
                continue;
            }
            binding = NativeBinding {
                name == "c_export" ? NativeBindingKind::cExport : NativeBindingKind::cImport,
                {},
                attribute.span,
            };
            binding->dataSymbol = name == "c_data";
            if (attribute.arguments.empty()) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                       "C annotations require a 'symbol:' string-literal argument");
                continue;
            }
            const auto& argument = attribute.arguments.front();
            const auto literal = argument->value != nullptr &&
                    argument->value->kind == syntax::Kind::literalExpr
                    ? std::static_pointer_cast<syntax::LiteralExprSyntax>(argument->value)
                    : nullptr;
            if (argument->label == nullptr || argument->label->rawValue != "symbol" ||
                argument->accessMarker != nullptr || literal == nullptr ||
                literal->literal == nullptr || literal->literal->kind != stringLiteral) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, argument->span,
                       "C annotations require 'symbol:' followed by a string literal");
                continue;
            }
            binding->symbol = literal->literal->rawValue;
            if (!isCIdentifier(binding->symbol)) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, argument->span,
                       "native symbol must be a nonempty ASCII C identifier without linker decoration");
            }
            // Further arguments name `[UInt8]` parameters passed as a bare
            // data pointer: `pointer: "label"`, once per parameter.
            for (size_t position = 1; position < attribute.arguments.size(); ++position) {
                const auto& extra = attribute.arguments[position];
                const auto name = extra->value != nullptr && extra->value->kind == syntax::Kind::literalExpr
                        ? std::static_pointer_cast<syntax::LiteralExprSyntax>(extra->value)
                        : nullptr;
                if (extra->label == nullptr || extra->label->rawValue != "pointer" ||
                    extra->accessMarker != nullptr || name == nullptr || name->literal == nullptr ||
                    name->literal->kind != stringLiteral) {
                    report(TypeCheckingDiagnosticId::invalidAnnotation, extra->span,
                           "after 'symbol:', C annotations accept only 'pointer:' followed by a "
                           "parameter label string literal");
                    continue;
                }
                const auto& parameters = declaration->parameters;
                const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
                    return parameter->label != nullptr && parameter->label->rawValue == name->literal->rawValue;
                });
                if (found == parameters.end()) {
                    report(TypeCheckingDiagnosticId::invalidAnnotation, extra->span,
                           "'pointer:' names no parameter '" + name->literal->rawValue + "'");
                    continue;
                }
                const auto index = static_cast<uint32_t>(found - parameters.begin());
                if (binding->passesPointer(index)) {
                    report(TypeCheckingDiagnosticId::invalidAnnotation, extra->span,
                           "parameter '" + name->literal->rawValue + "' is already passed as a pointer");
                    continue;
                }
                binding->pointerParameters.push_back(index);
            }
        }
        if (!binding.has_value()) return;
        if (binding->kind == NativeBindingKind::cExport && !binding->pointerParameters.empty()) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, binding->span,
                   "'pointer:' applies only to C imports");
        }
        if (!model->semanticModelValue->isRuntimeSource(declaration->span)) {
            report(TypeCheckingDiagnosticId::unauthorizedNativeBinding, binding->span,
                   "C bindings require an explicitly selected runtime compilation unit");
        }
        const bool importing = binding->kind == NativeBindingKind::cImport;
        const bool hasBody = declaration->body != nullptr;
        if ((importing && hasBody) || (!importing && !hasBody)) {
            report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                   importing ? "@c_extern must not have a function body"
                             : "@c_export requires a function body");
        }
        if (declaration->returnType == nullptr) {
            report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                   "C bindings require an explicit result type, including Void");
        }
        if (binding->dataSymbol) {
            // @c_data names a data symbol, not a function: no parameters, and the
            // address it yields is an Int. The name must not be a startup symbol.
            const auto& shape = model->callables.at(function);
            const auto* result = model->typeContext.type(shape.result);
            if (!declaration->parameters.empty() || result == nullptr || result->kind != TypeKind::integer ||
                !binding->pointerParameters.empty()) {
                report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                       "@c_data declares 'func name(): Int', the address of a C data symbol");
            }
            if (isCEntrySymbol(binding->symbol)) {
                report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                       "@c_data must not name a toolchain executable entry point");
            }
        }
        const auto* symbol = model->semanticModelValue->symbol(function);
        // Only a core unit (spec 19.4.6) may export the platform's program entry
        // point; every other startup symbol, and every other unit, may not.
        const bool coreEntry = !importing && isCoreEntrySymbol(binding->symbol) &&
                model->semanticModelValue->isCoreSource(declaration->span);
        if ((symbol->isRootModule && symbol->name == "main") ||
            (!importing && isCEntrySymbol(binding->symbol) && !coreEntry)) {
            report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                   "C bindings must not replace a toolchain executable entry point");
        }
        const auto& signature = model->callables.at(function);
        for (size_t index = 0; index < declaration->parameters.size(); ++index) {
            const auto& parameter = declaration->parameters[index];
            const auto effect = parameter->accessEffect();
            const auto buffer = nativeBufferKind(signature.parameters[index],
                    [&](TypeId id) { return model->typeContext.type(id); });
            if (binding->passesPointer(static_cast<uint32_t>(index))) {
                if (!importing || buffer != NativeBufferKind::bytes ||
                    (effect != syntax::AccessEffect::borrowing && effect != syntax::AccessEffect::inout)) {
                    report(TypeCheckingDiagnosticId::unsupportedNativeSignature, parameter->span,
                           "'pointer:' applies only to borrowing or inout [UInt8] parameters of C imports");
                }
                continue;
            }
            const bool scalar = effect == syntax::AccessEffect::borrowing &&
                    isNativeScalar(model->typeContext.type(signature.parameters[index])->kind);
            const bool nativeBuffer = importing && buffer != NativeBufferKind::none &&
                    (effect == syntax::AccessEffect::borrowing ||
                     (effect == syntax::AccessEffect::initializing &&
                      buffer != NativeBufferKind::byteArrays));
            if (!scalar && !nativeBuffer) {
                report(TypeCheckingDiagnosticId::unsupportedNativeSignature, parameter->span,
                       "C bindings require borrowing scalars; C imports also accept borrowing "
                       "String, [UInt8], [String], [[UInt8]] and initializing String, [UInt8], [String]");
            }
        }
        // Never marks a C function that does not return (an import such as exit),
        // or an export that does not (the runtime's panic).
        if (!isNativeScalar(model->typeContext.type(signature.result)->kind) &&
            signature.result != model->typeContext.voidType() &&
            model->typeContext.type(signature.result)->kind != TypeKind::never) {
            report(TypeCheckingDiagnosticId::unsupportedNativeSignature,
                   declaration->returnType == nullptr ? declaration->span : declaration->returnType->span,
                   "C bindings require a scalar or Void result");
        }
        if (intrinsic::reserved(binding->symbol)) {
            // A reserved symbol names a compiler intrinsic (§19.4.5): it must be
            // a known one, imported, with exactly its signature.
            std::vector<TypeKind> kinds;
            std::vector<bool> pointers;
            std::vector<bool> mutables;
            for (size_t index = 0; index < signature.parameters.size(); ++index) {
                kinds.push_back(model->typeContext.type(signature.parameters[index])->kind);
                pointers.push_back(binding->passesPointer(static_cast<uint32_t>(index)));
                mutables.push_back(declaration->parameters[index]->accessEffect() == syntax::AccessEffect::inout);
            }
            const auto* descriptor = intrinsic::find(binding->symbol);
            if (!importing || descriptor == nullptr ||
                !intrinsic::matches(*descriptor, model->typeContext.type(signature.result)->kind,
                                    kinds, pointers, mutables)) {
                report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                       "'" + binding->symbol + "' is not a compiler intrinsic with this signature");
            }
        }
        if (diagnostics.size() == diagnosticStart) {
            model->nativeBindings.emplace(function, std::move(*binding));
        }
    }

    void validateNativeSymbols() {
        std::unordered_map<std::string, semantic::SymbolId> declarations;
        std::unordered_set<std::string> exports;
        for (const auto& symbol : model->semanticModelValue->symbols()) {
            const auto* binding = model->nativeBinding(symbol.id);
            if (binding == nullptr) continue;
            const auto [previous, inserted] = declarations.emplace(binding->symbol, symbol.id);
            if (!inserted && model->nativeBinding(previous->second)->dataSymbol != binding->dataSymbol) {
                report(TypeCheckingDiagnosticId::conflictingNativeSymbol, binding->span,
                       "native symbol '" + binding->symbol + "' is declared as both a function and a data symbol");
            } else if (!inserted) {
                const auto& left = model->callables.at(previous->second);
                const auto& right = model->callables.at(symbol.id);
                const auto compatible = [&](TypeId a, TypeId b) {
                    const auto lookup = [&](TypeId id) { return model->typeContext.type(id); };
                    const auto leftBuffer = nativeBufferKind(a, lookup);
                    const auto rightBuffer = nativeBufferKind(b, lookup);
                    if (leftBuffer != NativeBufferKind::none || rightBuffer != NativeBufferKind::none) {
                        return leftBuffer == rightBuffer;
                    }
                    return sameNativeRepresentation(model->typeContext.type(a)->kind,
                            model->typeContext.type(b)->kind);
                };
                const auto* previousSymbol = model->semanticModelValue->symbol(previous->second);
                bool sameEffects = previousSymbol->callable->parameters.size() == symbol.callable->parameters.size();
                for (size_t index = 0; sameEffects && index < symbol.callable->parameters.size(); ++index) {
                    sameEffects = previousSymbol->callable->parameters[index].access ==
                            symbol.callable->parameters[index].access;
                }
                // Repeated declarations must also agree on which buffers carry no count.
                auto left_pointers = model->nativeBinding(previous->second)->pointerParameters;
                auto right_pointers = binding->pointerParameters;
                std::sort(left_pointers.begin(), left_pointers.end());
                std::sort(right_pointers.begin(), right_pointers.end());
                sameEffects = sameEffects && left_pointers == right_pointers;
                if (left.parameters.size() != right.parameters.size() ||
                        !std::equal(left.parameters.begin(), left.parameters.end(),
                                right.parameters.begin(), right.parameters.end(), compatible) ||
                        !compatible(left.result, right.result) || !sameEffects) {
                    report(TypeCheckingDiagnosticId::conflictingNativeSymbol, binding->span,
                           "incompatible C signatures for native symbol '" + binding->symbol + "'");
                }
            }
            if (binding->kind == NativeBindingKind::cExport &&
                !exports.insert(binding->symbol).second) {
                report(TypeCheckingDiagnosticId::conflictingNativeSymbol, binding->span,
                       "duplicate C export for native symbol '" + binding->symbol + "'");
            }
        }
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
                enumPayloadChecks.emplace_back(signature.parameters.back(), associatedType->span);
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
                    "' is required",
                "expected '" + model->typeContext.displayName(destination) +
                    "' but found '" + model->typeContext.displayName(source) + "'");
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

        // The parser rewrites `for x in source` into a hidden `$for<N>` copy of the
        // source that is later counted and subscripted. Reject a source that cannot
        // be, and poison the binding so the rewrite adds no follow-on errors.
        if (isForInSourceBinding(declaration) && initializer.has_value() &&
            *initializer != model->typeContext.errorType()) {
            const auto* sourceType = model->typeContext.type(*initializer);
            if (sourceType == nullptr || (sourceType->kind != TypeKind::array &&
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
            if (sourceType != nullptr && sourceType->kind == TypeKind::array &&
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
    static bool isForInSourceBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        if (declaration->name == nullptr) return false;
        const auto& name = declaration->name->rawValue;
        if (name.size() <= 4 || name.compare(0, 4, "$for") != 0) return false;
        return std::all_of(name.begin() + 4, name.end(),
                [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    // The type-level rules of linear types (decision 0015): the `linear` modifier is for
    // toolchain-authorized sources, and a linear value has no `deinit` to run, so a type with a
    // `deinit` may not be or hold one.
    void checkLinearStructure(const syntax::StructDeclSyntax::Ptr& structure) {
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
        if (type.has_value() && model->isLinear(*type)) {
            report(TypeCheckingDiagnosticId::linearInDeinitType, structure->deinitializer->span,
                   "a linear value is never destroyed implicitly, so '" +
                           (structure->name == nullptr ? std::string() : structure->name->rawValue) +
                           "' cannot declare 'deinit'",
                   "remove the 'deinit', or end the linear field explicitly instead");
        }
    }

    // A Dict cannot hold a linear key or value: nothing could drain it.
    void requireNoLinearDictionary(TypeId id, SourceSpan span) {
        const auto* type = model->typeContext.type(id);
        if (type == nullptr) return;
        const auto arguments = type->arguments;
        if (type->kind == TypeKind::dictionary &&
            std::any_of(arguments.begin(), arguments.end(),
                        [this](TypeId argument) { return model->isLinear(argument); })) {
            report(TypeCheckingDiagnosticId::linearCollectionElement, span,
                   "a dictionary cannot hold a linear key or value ('" +
                           model->typeContext.displayName(id) + "')",
                   "use an array and drain it with removeLast");
            return;
        }
        if ((type->kind == TypeKind::channelSender || type->kind == TypeKind::channelReceiver) &&
            arguments.size() == 1) {
            const auto* element = model->typeContext.type(arguments[0]);
            if (model->isLinear(arguments[0])) {
                report(TypeCheckingDiagnosticId::linearChannelElement, span,
                       "'" + model->typeContext.displayName(arguments[0]) +
                               "' is linear, so it cannot be the element type of '" + model->typeContext.displayName(id) + "'",
                       "a value still queued when the channel closes would be dropped; send the result of "
                       "'consume handle.join()' (or another value that can be destroyed) instead of the handle");
                return;
            }
            if (element != nullptr && (element->kind == TypeKind::any || element->kind == TypeKind::never)) {
                report(TypeCheckingDiagnosticId::channelElementType, span,
                       "'" + model->typeContext.displayName(arguments[0]) + "' cannot be the element type of a channel ('" +
                               model->typeContext.displayName(id) + "')",
                       "use a concrete type, or Void for a signal that carries no value");
                return;
            }
        }
        for (const auto argument : arguments) requireNoLinearDictionary(argument, span);
    }


    void checkTopLevelBody(const syntax::NodePtr& node) {
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

    void checkFunctionBody(const syntax::FunctionDeclSyntax::Ptr& function) {
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
        checkBlock(function->body);
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

    TypeId checkBlock(const syntax::BlockExprSyntax::Ptr& block,
            std::optional<TypeId> expected = std::nullopt) {
        if (block == nullptr) return model->typeContext.errorType();
        TypeId result = model->typeContext.voidType();
        for (size_t index = 0; index < block->items.size(); ++index) {
            const auto itemType = checkNode(block->items[index],
                    index + 1 == block->items.size() ? expected : std::nullopt);
            if (result != model->typeContext.neverType()) result = itemType;
        }
        recordNodeType(block, result);
        return result;
    }

    TypeId checkNode(const syntax::NodePtr& node,
            std::optional<TypeId> expected = std::nullopt) {
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
            checkBlock(statement->body);
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

    bool isExpressionKind(syntax::Kind kind) const {
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

    std::optional<TypeId> checkExpression(
            const syntax::ExprPtr& expression,
            std::optional<TypeId> expected = std::nullopt,
            ReceiverMarker receiverMarker = ReceiverMarker::none) {
        if (expression == nullptr) return std::nullopt;
        markDeferredReferenceHandled(expression);

        auto result = model->typeContext.errorType();
        switch (expression->kind) {
            case syntax::Kind::errorExpr:
                break;
            case syntax::Kind::unitExpr:
                result = model->typeContext.voidType();
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
                    std::static_pointer_cast<syntax::PrefixExprSyntax>(expression), expected);
                break;
            case syntax::Kind::accessExpr: {
                const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
                const auto consumes = access->marker != nullptr &&
                        access->marker->kind == kwConsume;
                if (consumes && !isMemberCall(access->operand)) {
                    if (!permittedMoves.contains(expression.get())) {
                        report(
                            TypeCheckingDiagnosticId::invalidConsumeArgument,
                            expression->span,
                            "'consume' is only valid on an argument, the receiver of a consuming "
                            "method call, or where a value is stored (a binding, an assignment, "
                            "or an aggregate element)");
                    } else if (!isConsumable(access->operand)) {
                        report(
                            TypeCheckingDiagnosticId::invalidConsumeArgument,
                            access->operand->span,
                            "consuming argument must be an owning local, consuming parameter, "
                            "or temporary");
                    }
                }
                result = checkExpression(
                        access->operand,
                        expected,
                        consumes ? ReceiverMarker::consume : ReceiverMarker::inout)
                        .value_or(model->typeContext.errorType());
                break;
            }
            case syntax::Kind::propagateExpr:
                result = checkPropagation(
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression),
                        expected);
                break;
            case syntax::Kind::assignmentExpr: {
                const auto assignment =
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                const auto target = checkExpression(assignment->target);
                checkAssignmentAccess(assignment->target);
                if (insideDeinit && assignsWholeSelf(assignment->target)) {
                    report(
                            TypeCheckingDiagnosticId::assignmentToSelfInDeinit,
                            assignment->target->span,
                            "a 'deinit' cannot replace 'self'",
                            "assign to the fields of 'self' instead; replacing the whole value "
                            "would run a destructor on a value that is being destroyed");
                }
                if (assignment->op != nullptr && assignment->op->kind != equal) {
                    result = checkCompoundAssignment(assignment, target);
                    break;
                }
                permitMove(assignment->value);
                const auto value = checkExpression(assignment->value, target);
                requireNoImplicitCopy(assignment->value, value);
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
                        receiverMarker);
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
                result = checkBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression), expected);
                break;
            case syntax::Kind::ifExpr: {
                const auto conditional = std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                const auto condition = checkExpression(conditional->condition).value_or(
                    model->typeContext.errorType());
                requireAssignable(
                    condition,
                    model->typeContext.boolType(),
                    conditional->condition->span);
                const auto thenType = checkBlock(conditional->thenBranch, expected);
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
                // Returning an owning local or a consuming parameter moves it out.
                if (returned->value != nullptr) {
                    requireNoImplicitCopy(returned->value, actual, true);
                }
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
                        std::static_pointer_cast<syntax::BinaryExprSyntax>(expression), expected);
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

    TypeId checkNumericLiteral(const Token::Ptr& literal, std::optional<TypeId> expected,
            bool negative = false) {
        auto result = literal->kind == floatLiteral
                ? model->typeContext.doubleType() : model->typeContext.intType();
        if (expected.has_value()) {
            auto* contextual = model->typeContext.type(*expected);
            if (contextual != nullptr && contextual->kind == TypeKind::optional) {
                expected = contextual->arguments.front();
                contextual = model->typeContext.type(*expected);
            }
            if (contextual != nullptr && isNumeric(contextual->kind)) result = *expected;
        }
        const auto kind = model->typeContext.type(result)->kind;
        bool valid = false;
        if (isFloating(kind)) {
            const auto& text = literal->rawValue;
            const auto fits = [&text](auto value) {
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                        value, std::chars_format::general);
                return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                        std::isfinite(value);
            };
            valid = kind == TypeKind::float32 ? fits(float{}) : fits(double{});
        } else if (literal->kind == decimalLiteral && (!negative || isSignedInteger(kind))) {
            const auto bits = scalarBitWidth(kind);
            const auto maximum = isSignedInteger(kind)
                    ? (uint64_t{1} << (bits - 1)) - (negative ? 0 : 1)
                    : bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
            valid = static_cast<uint64_t>(literal->intValue) <= maximum;
        }
        if (!valid) {
            report(TypeCheckingDiagnosticId::invalidNumericLiteral, literal->span,
                    "numeric literal is not representable as '" +
                            model->typeContext.displayName(result) + "'");
            return model->typeContext.errorType();
        }
        return result;
    }

    TypeId checkLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        if (expression->literal == nullptr) return model->typeContext.errorType();
        switch (expression->literal->kind) {
            case decimalLiteral:
            case floatLiteral: return checkNumericLiteral(expression->literal, expected);
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

    TypeId checkPropagation(
            const syntax::PropagateExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        std::optional<TypeId> operandExpected;
        if (expected.has_value() &&
            *expected != model->typeContext.errorType() &&
            *expected != model->typeContext.anyType() &&
            currentReturnType.has_value()) {
            const auto* returnType = model->typeContext.type(*currentReturnType);
            if (returnType != nullptr && returnType->kind == TypeKind::result) {
                const auto errorType = returnType->arguments[1];
                operandExpected = model->typeContext.resultType(*expected, errorType);
            } else if (returnType != nullptr && returnType->kind == TypeKind::optional) {
                operandExpected = model->typeContext.optionalType(*expected);
            }
        }
        permitMove(expression->operand);
        const auto operand = checkExpression(expression->operand, operandExpected).value_or(
                model->typeContext.errorType());
        if (operand == model->typeContext.errorType()) return operand;

        const auto* operandType = model->typeContext.type(operand);
        if (operandType == nullptr ||
            (operandType->kind != TypeKind::result &&
             operandType->kind != TypeKind::optional)) {
            report(
                    TypeCheckingDiagnosticId::invalidPropagationOperand,
                    expression->span,
                    "postfix '?' requires a Result or Optional operand");
            return model->typeContext.errorType();
        }

        const auto* returnType = currentReturnType.has_value()
                ? model->typeContext.type(*currentReturnType)
                : nullptr;
        auto result = operandType->arguments[0];
        if (returnType == nullptr || returnType->kind != operandType->kind) {
            report(
                    TypeCheckingDiagnosticId::invalidPropagationContext,
                    expression->span,
                    operandType->kind == TypeKind::result
                            ? "postfix '?' on Result requires a Result-returning function"
                            : "postfix '?' on Optional requires an Optional-returning function");
            result = model->typeContext.errorType();
        } else if (operandType->kind == TypeKind::result &&
                   operandType->arguments[1] != returnType->arguments[1]) {
            report(
                    TypeCheckingDiagnosticId::mismatchedPropagationError,
                    expression->span,
                    "postfix '?' requires the same error type: operand has '" +
                            model->typeContext.displayName(operandType->arguments[1]) +
                            "' but function returns '" +
                            model->typeContext.displayName(returnType->arguments[1]) + "'");
            result = model->typeContext.errorType();
        }
        requireNoImplicitCopy(expression->operand, operand);
        return result;
    }

    TypeId checkCast(const syntax::PrefixExprSyntax::Ptr& expression) {
        const auto target = resolveType(expression->castTarget).value_or(
                model->typeContext.errorType());
        if (target == model->typeContext.errorType()) return target;
        if (!isInteger(model->typeContext.type(target)->kind)) {
            reportInvalidOperator(expression->op, { target }, expression->span);
            return model->typeContext.errorType();
        }
        const bool tryCast = expression->op->rawValue == "as?";
        // A literal operand takes the target type, so out-of-range literals
        // are compile-time errors rather than run-time traps. `as?` accepts any
        // value and reports a range failure in its Result, so literals stay Int.
        const auto operand = isNumericLiteral(expression->operand) && !tryCast
                ? checkExpression(expression->operand, target)
                : checkExpression(expression->operand);
        const auto operandType = operand.value_or(model->typeContext.errorType());
        if (operandType == model->typeContext.errorType()) return operandType;
        if (!isInteger(model->typeContext.type(operandType)->kind)) {
            reportInvalidOperator(expression->op, { operandType, target }, expression->span);
            return model->typeContext.errorType();
        }
        if (tryCast) {
            const auto symbol = model->typeContext.builtinSymbol("ConversionError");
            const auto failure = symbol.has_value()
                    ? model->typeContext.typeForSymbol(*symbol) : std::nullopt;
            if (!failure.has_value()) return model->typeContext.errorType();
            return model->typeContext.resultType(target, *failure);
        }
        return target;
    }

    TypeId checkPrefix(const syntax::PrefixExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        if (expression->castTarget != nullptr) return checkCast(expression);
        if (expression->op != nullptr && expression->op->kind == minus &&
                expression->operand->kind == syntax::Kind::literalExpr) {
            const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression->operand);
            if (literal->literal->kind == decimalLiteral) {
                const auto result = checkNumericLiteral(literal->literal, expected, true);
                recordNodeType(literal, result);
                return result;
            }
        }
        const auto operand = checkExpression(expression->operand, expected).value_or(
                model->typeContext.errorType());
        if (operand == model->typeContext.errorType()) return operand;
        if (expression->op != nullptr && expression->op->kind == minus &&
            (isSignedInteger(model->typeContext.type(operand)->kind) ||
             isFloating(model->typeContext.type(operand)->kind))) {
            return operand;
        }
        if (expression->op != nullptr && expression->op->kind == tilde &&
            isInteger(model->typeContext.type(operand)->kind)) {
            return operand;
        }
        if (expression->op != nullptr && expression->op->kind == bang &&
            model->typeContext.isAssignable(operand, model->typeContext.boolType())) {
            return model->typeContext.boolType();
        }
        reportInvalidOperator(expression->op, { operand }, expression->span);
        return model->typeContext.errorType();
    }

    bool isNumericLiteral(const syntax::ExprPtr& expression) const {
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return isNumericLiteral(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        }
        if (expression->kind == syntax::Kind::prefixExpr) {
            return isNumericLiteral(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand);
        }
        if (expression->kind != syntax::Kind::literalExpr) return false;
        const auto kind = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression)->literal->kind;
        return kind == decimalLiteral || kind == floatLiteral;
    }

    // Result type of `left op right`, or nullopt if the operands are invalid.
    std::optional<TypeId> binaryResult(TokenKind op, TypeId left, TypeId right) {
        switch (op) {
            case plus:
                if (isNumeric(model->typeContext.type(left)->kind) && left == right) return left;
                if (left == model->typeContext.stringType() && left == right) return left;
                break;
            case minus:
            case multiply:
            case divide:
                if (isNumeric(model->typeContext.type(left)->kind) && left == right) return left;
                break;
            case percentage:
            case ampersand:
            case verticalBar:
            case caret:
                if (isInteger(model->typeContext.type(left)->kind) && left == right) return left;
                break;
            case shiftLeft:
            case shiftRight:
                if (isInteger(model->typeContext.type(left)->kind) &&
                    isInteger(model->typeContext.type(right)->kind)) {
                    return left;
                }
                break;
            case less:
            case lessEqual:
            case greater:
            case greaterEqual:
                if (left == right &&
                    (isNumeric(model->typeContext.type(left)->kind) ||
                     left == model->typeContext.stringType())) {
                    return model->typeContext.boolType();
                }
                break;
            case equalEqual:
            case notEqual:
                // A noncopyable value (a task handle, a handle with `deinit`) cannot be compared: the
                // comparison would have to copy it.
                // Function types have no equality: identity is not stable across optimization
                // (spec 2.3.1).
                if (left == right && left != model->typeContext.voidType() && model->isCopyable(left) &&
                    !containsFunctionType(left) &&
                    !model->typeContext.mentionsTypeParameter(left) &&
                    model->typeContext.type(left) != nullptr &&
                    model->typeContext.type(left)->kind != TypeKind::enumeration &&
                    model->typeContext.type(left)->kind != TypeKind::structure) {
                    return model->typeContext.boolType();
                }
                break;
            case andAnd:
            case orOr:
                if (model->typeContext.isAssignable(left, model->typeContext.boolType()) &&
                    model->typeContext.isAssignable(right, model->typeContext.boolType())) {
                    return model->typeContext.boolType();
                }
                break;
            default:
                break;
        }
        return std::nullopt;
    }

    TypeId checkBinary(const syntax::BinaryExprSyntax::Ptr& expression,
            std::optional<TypeId> expected) {
        TypeId left;
        TypeId right;
        const auto isShift = expression->op != nullptr &&
                (expression->op->kind == shiftLeft || expression->op->kind == shiftRight);
        if (isShift) {
            // The shifted value takes the contextual type; the count is independent.
            left = checkExpression(expression->left, expected).value_or(model->typeContext.errorType());
            right = checkExpression(expression->right).value_or(model->typeContext.errorType());
        } else if (isNumericLiteral(expression->left) && !isNumericLiteral(expression->right)) {
            right = checkExpression(expression->right, expected).value_or(model->typeContext.errorType());
            left = checkExpression(expression->left, right).value_or(model->typeContext.errorType());
        } else {
            left = checkExpression(expression->left, expected).value_or(model->typeContext.errorType());
            right = checkExpression(expression->right, left).value_or(model->typeContext.errorType());
        }
        if (left == model->typeContext.errorType() ||
            right == model->typeContext.errorType()) {
            return model->typeContext.errorType();
        }

        const auto op = expression->op == nullptr ? invalid : expression->op->kind;
        if (const auto result = binaryResult(op, left, right); result.has_value()) return *result;

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

    // A type may name a constructor or qualify a member, but is not itself a value (spec 5.6.1).
    bool rejectTypeAsValue(const syntax::ExprPtr& expression) {
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

    TypeId checkName(const syntax::NameExprSyntax::Ptr& expression) {
        if (expression.get() != currentCallee) {
            if (const auto value = functionValueType(expression)) return *value;
        }
        if (rejectTypeAsValue(expression)) return model->typeContext.errorType();
        return referencedValueType(expression);
    }

    // A function named where it is not called is a function value (spec 5.6.1). Returns nullopt
    // when `expression` does not name a function, and the error type, with a diagnostic, when it
    // names one that cannot be a value.
    std::optional<TypeId> functionValueType(const syntax::ExprPtr& expression) {
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
        std::vector<syntax::AccessEffect> effects;
        for (const auto& parameter : symbol->callable->parameters) effects.push_back(parameter.access);
        return model->typeContext.functionType(signature->parameters, effects, signature->result);
    }

    // `f(a, &b)` or `(s.f)(a)` where the callee is a value of function type (spec 5.6.1): positional
    // arguments, each checked against the type's parameter effect and type.
    TypeId checkFunctionValueCall(
            const syntax::CallExprSyntax::Ptr& expression,
            TypeId calleeType,
            ReceiverMarker receiverMarker) {
        // Copied out: checking the arguments can grow the type table and invalidate the record.
        const auto parameters = model->typeContext.type(calleeType)->arguments;
        const auto effects = model->typeContext.type(calleeType)->effects;
        const auto result = parameters.back();
        if (receiverMarker != ReceiverMarker::none) {
            report(TypeCheckingDiagnosticId::invalidAccessMarker, expression->span,
                   "'&' and 'consume' mark the arguments of a call through a function value, not the call",
                   "remove the marker before the call");
        }
        for (const auto& argument : expression->arguments) {
            if (argument->label == nullptr) continue;
            report(TypeCheckingDiagnosticId::labeledFunctionValueCall, argument->label->span,
                   "a call through a function value takes positional arguments; remove '" +
                           argument->label->rawValue + ":'",
                   "a function type has no parameter labels");
        }
        if (expression->arguments.size() != effects.size()) {
            report(TypeCheckingDiagnosticId::invalidCallArguments, expression->span,
                   "a function value of type '" + model->typeContext.displayName(calleeType) + "' takes " +
                           std::to_string(effects.size()) + " argument(s), but " +
                           std::to_string(expression->arguments.size()) + " were given");
        }
        std::vector<CallAccess> accesses;
        for (size_t index = 0; index < expression->arguments.size(); ++index) {
            const auto& argument = expression->arguments[index];
            const auto expected = index < effects.size()
                    ? std::optional<TypeId>(parameters[index])
                    : std::optional<TypeId>();
            const auto actual = checkExpression(argument->value, expected);
            if (index >= effects.size()) continue;
            checkArgumentAccess(effects[index], *argument);
            collectCallArgumentAccess(*argument, effects[index], accesses);
            if (actual.has_value()) requireAssignable(*actual, parameters[index], argument->value->span);
        }
        // The callee was evaluated to a copy first and holds no access (spec 4.4), so only the
        // arguments can conflict.
        validateCallExclusivity(accesses);
        return result;
    }

    bool isFunctionType(TypeId type) const {
        const auto* record = model->typeContext.type(type);
        return record != nullptr && record->kind == TypeKind::function;
    }

    // True for a function type, or a builtin container of one.
    bool containsFunctionType(TypeId type) const {
        const auto* record = model->typeContext.type(type);
        if (record == nullptr) return false;
        switch (record->kind) {
            case TypeKind::function: return true;
            case TypeKind::array:
            case TypeKind::dictionary:
            case TypeKind::optional:
            case TypeKind::result:
                return std::any_of(record->arguments.begin(), record->arguments.end(),
                        [this](TypeId argument) { return containsFunctionType(argument); });
            default: return false;
        }
    }

    void rejectInvalidDictionaryKey(TypeId key, SourceSpan span) {
        if (containsFunctionType(key)) {
            report(TypeCheckingDiagnosticId::invalidFunctionType, span,
                   "a function type cannot be a dictionary key: function values have no equality",
                   "key the dictionary by a name or a number and store the function value as the value");
        }
        if (model->typeContext.mentionsTypeParameter(key)) {
            report(TypeCheckingDiagnosticId::invalidTypeArgument, span,
                   "a dictionary key cannot be the type parameter type '" +
                           model->typeContext.displayName(key) + "'",
                   "keys are restricted to Int, Bool, UInt8 and String, and no protocol states that "
                   "yet (spec 20.5)");
        }
    }

    bool isOptionalFunction(TypeId type) const {
        const auto* record = model->typeContext.type(type);
        return record != nullptr && record->kind == TypeKind::optional && isFunctionType(record->arguments[0]);
    }

    TypeId referencedValueType(const syntax::NodePtr& expression) {
        const auto referenced = model->referencedSymbol(expression);
        if (!referenced.has_value()) return model->typeContext.errorType();
        const auto type = model->typeOf(*referenced);
        return type.value_or(model->typeContext.errorType());
    }

    TypeId checkMember(const syntax::MemberExprSyntax::Ptr& expression) {
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
        if (expression.get() != currentCallee && memberSymbol != nullptr) {
            // Only top-level functions are values (spec 5.6.1); a member that must be called is not.
            const bool builtinMethod = memberSymbol->kind == semantic::SymbolKind::builtinMember &&
                    memberSymbol->callable.has_value();
            const bool payloadCase = (memberSymbol->kind == semantic::SymbolKind::enumCase ||
                                      memberSymbol->kind == semantic::SymbolKind::builtinEnumCase) &&
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

    bool requireAccessible(semantic::SymbolId target, SourceSpan use) {
        if (model->semanticModelValue->isAccessible(target, use)) return true;
        report(TypeCheckingDiagnosticId::inaccessibleDeclaration, use,
               "declaration '" + model->semanticModelValue->symbol(target)->name +
               "' is not accessible from this file");
        return false;
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

    // True for `receiver.name(...)`, ignoring parentheses.
    bool isMemberCall(const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return false;
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return isMemberCall(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                            ->expression);
        }
        return expression->kind == syntax::Kind::callExpr &&
                std::static_pointer_cast<syntax::CallExprSyntax>(expression)
                        ->callee->kind == syntax::Kind::memberExpr;
    }

    // How a call accesses its receiver: builtin members by their mutability,
    // user methods by their declared receiver effect.
    std::optional<syntax::AccessEffect> receiverEffectOf(const semantic::Symbol* target) const {
        if (target == nullptr) return std::nullopt;
        if (target->kind == semantic::SymbolKind::builtinMember) {
            if (target->callable.has_value() && target->callable->receiver.has_value()) {
                return target->callable->receiver->access;
            }
            return target->isMutable
                    ? syntax::AccessEffect::inout
                    : syntax::AccessEffect::borrowing;
        }
        if (target->kind == semantic::SymbolKind::method && target->callable.has_value() &&
            target->callable->receiver.has_value()) {
            return target->callable->receiver->access;
        }
        return std::nullopt;
    }

    TypeId checkCall(
            const syntax::CallExprSyntax::Ptr& expression,
            ReceiverMarker receiverMarker = ReceiverMarker::none) {
        const auto* savedCallee = currentCallee;
        currentCallee = expression->callee.get();
        const auto calleeType = checkExpression(expression->callee).value_or(
                model->typeContext.errorType());
        currentCallee = savedCallee;
        auto target = model->callTarget(expression);
        if (!target.has_value() && isFunctionType(calleeType)) {
            const auto callee = model->referencedSymbol(expression->callee);
            const auto* calleeSymbol = callee.has_value() ? model->semanticModelValue->symbol(*callee) : nullptr;
            if (expression->callee->kind == syntax::Kind::memberExpr &&
                !model->semanticModelValue->isModuleQualified(expression->callee)) {
                // `s.f(x)` always names a method (spec 5.4).
                const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
                const auto name = member->member == nullptr ? std::string() : member->member->rawValue;
                report(TypeCheckingDiagnosticId::notCallable, expression->callee->span,
                       "'" + name + "' is a stored function value, not a method; 'value." + name +
                               "(...)' always calls a method",
                       "call the stored function with a parenthesized callee: '(value." + name + ")(...)'");
                for (const auto& argument : expression->arguments) checkExpression(argument->value);
                return model->typeContext.errorType();
            }
            if (expression->callee->kind != syntax::Kind::nameExpr &&
                expression->callee->kind != syntax::Kind::parenthesizedExpr &&
                expression->callee->kind != syntax::Kind::propagateExpr &&
                expression->callee->kind != syntax::Kind::memberExpr) {
                report(TypeCheckingDiagnosticId::notCallable, expression->callee->span,
                       "a call through a function value needs a plain name or a parenthesized callee",
                       "put the callee in parentheses, as in '(table[i])(x)' or '(make())(x)'");
                for (const auto& argument : expression->arguments) checkExpression(argument->value);
                return model->typeContext.errorType();
            }
            if (calleeSymbol == nullptr || !calleeSymbol->callable.has_value()) {
                return checkFunctionValueCall(expression, calleeType, receiverMarker);
            }
        }
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
            if (target.has_value()) {
                if (!requireAccessible(*target, expression->span)) {
                    return model->typeContext.errorType();
                }
                recordResolvedCallTarget(expression, *target);
                for (auto& diagnostic : semantic::validateCallArguments(
                        *expression, *model->semanticModelValue->symbol(*target))) {
                    report(
                            TypeCheckingDiagnosticId::invalidCallArguments,
                            diagnostic.span,
                            std::move(diagnostic.message));
                }
            }
        }

        const auto* signature = target.has_value() ? model->callable(*target) : nullptr;
        // A generic call infers its type arguments (spec 20.4.1), and a requirement call on `T`
        // replaces the protocol's `Self` with `T` (spec 20.5); either way the argument checks below
        // use the substituted signature.
        TypedCallableSignature substitutedSignature;
        std::vector<std::optional<std::optional<TypeId>>> preChecked(expression->arguments.size());
        const auto* calledSymbol = target.has_value() ? model->semanticModelValue->symbol(*target) : nullptr;
        if (signature != nullptr && calledSymbol != nullptr) {
            const auto* genericParameters = model->genericParameters(*target);
            const auto* owner = calledSymbol->containingSymbol.has_value()
                    ? model->semanticModelValue->symbol(*calledSymbol->containingSymbol)
                    : nullptr;
            if (genericParameters != nullptr && calledSymbol->kind == semantic::SymbolKind::function) {
                substitutedSignature = inferGenericCall(
                        expression, *target, *calledSymbol, *signature, *genericParameters, preChecked);
                signature = &substitutedSignature;
            } else if (owner != nullptr && owner->kind == semantic::SymbolKind::protocol &&
                       expression->callee->kind == syntax::Kind::memberExpr) {
                const auto base = model->typeOf(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base);
                const std::unordered_map<semantic::SymbolId, TypeId> self {
                    { owner->id, base.value_or(model->typeContext.errorType()) } };
                substitutedSignature = *signature;
                for (auto& parameter : substitutedSignature.parameters) {
                    parameter = model->typeContext.substitute(parameter, self);
                }
                substitutedSignature.result = model->typeContext.substitute(substitutedSignature.result, self);
                signature = &substitutedSignature;
            }
        }
        if (target.has_value() && model->nativeBinding(*target) != nullptr &&
            !model->semanticModelValue->isRuntimeSource(expression->span)) {
            report(TypeCheckingDiagnosticId::unauthorizedNativeBinding, expression->span,
                   "direct calls to C bindings require a runtime compilation unit; use a checked Joyeer wrapper");
        }
        const auto* semanticTarget = target.has_value()
                ? model->semanticModelValue->symbol(*target)
                : nullptr;
        if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::function &&
            semanticTarget->name == "runProcess" && !semanticTarget->isRootModule) {
            const auto* module = model->semanticModelValue->moduleName(semanticTarget->moduleId);
            if (module != nullptr && *module == "System") model->runProcessCallSpans.push_back(expression->span);
        }
        if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
            semanticTarget->name == "print"
 && model->semanticModelValue->isCoreSource(expression->span)) {
            report(TypeCheckingDiagnosticId::managedTypeInCoreUnit, expression->span,
                   "a runtime core unit must not call print, which is implemented by the runtime");
        }
        if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
            semanticTarget->name == "spawn") {
            model->spawnCallSpans.push_back(expression->span);
            return checkSpawn(expression);
        }
        if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
            semanticTarget->name == "channel") {
            return checkChannel(expression, *semanticTarget);
        }
        if (semanticTarget != nullptr &&
            (semanticTarget->kind == semantic::SymbolKind::method ||
             (semanticTarget->kind == semantic::SymbolKind::builtinMember &&
              (semanticTarget->name == "send" || semanticTarget->name == "receive" ||
               semanticTarget->name == "close" || semanticTarget->name == "join")))) {
            // A method needs a receiver value; `Type.method(...)` has none.
            const auto base = expression->callee->kind == syntax::Kind::memberExpr
                    ? std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base
                    : syntax::ExprPtr();
            const auto baseSymbol = base == nullptr
                    ? std::optional<semantic::SymbolId>()
                    : model->semanticModelValue->referencedSymbol(base);
            const auto* baseDeclaration = baseSymbol.has_value()
                    ? model->semanticModelValue->symbol(*baseSymbol)
                    : nullptr;
            if (base == nullptr ||
                (baseDeclaration != nullptr &&
                 (baseDeclaration->kind == semantic::SymbolKind::structure ||
                  baseDeclaration->kind == semantic::SymbolKind::enumeration ||
                  baseDeclaration->kind == semantic::SymbolKind::typeParameter ||
                  baseDeclaration->kind == semantic::SymbolKind::protocol ||
                  baseDeclaration->kind == semantic::SymbolKind::builtinType))) {
                report(
                        TypeCheckingDiagnosticId::invalidCallArguments,
                        expression->callee->span,
                        "method '" + semanticTarget->name + "' must be called on a value");
            }
        }

        const auto receiverEffect = receiverEffectOf(semanticTarget);
        const auto requiredReceiver = receiverEffect.value_or(syntax::AccessEffect::borrowing);
        const auto suppliedReceiver = receiverMarker == ReceiverMarker::inout
                ? syntax::AccessEffect::inout
                : receiverMarker == ReceiverMarker::consume
                        ? syntax::AccessEffect::consuming
                        : syntax::AccessEffect::borrowing;
        // An unknown member already has its own diagnostic; a marker complaint would only cascade.
        if (requiredReceiver != suppliedReceiver && semanticTarget != nullptr) {
            const bool consumeInvolved = requiredReceiver == syntax::AccessEffect::consuming ||
                    suppliedReceiver == syntax::AccessEffect::consuming;
            report(
                    consumeInvolved
                            ? TypeCheckingDiagnosticId::invalidConsumeArgument
                            : TypeCheckingDiagnosticId::invalidAccessMarker,
                    expression->callee->span,
                    requiredReceiver == syntax::AccessEffect::inout
                            ? "mutating method call requires '&' on the receiver"
                            : requiredReceiver == syntax::AccessEffect::consuming
                                    ? "consuming method call requires 'consume' on the receiver"
                                    : suppliedReceiver == syntax::AccessEffect::inout
                                            ? "non-mutating call must not use '&' on the receiver"
                                            : "non-consuming call must not use 'consume' on the receiver",
                    requiredReceiver == syntax::AccessEffect::consuming
                            ? std::optional<std::string>(
                                      "write 'consume' before the receiver, as in 'let v = consume x." +
                                      (semanticTarget != nullptr ? semanticTarget->name : std::string("join")) +
                                      "()'; to use a result as an argument or before '?', bind or parenthesize it first")
                            : std::nullopt);

        }

        std::vector<CallAccess> accesses;
        if (receiverEffect.has_value() && expression->callee->kind == syntax::Kind::memberExpr) {
            const auto member =
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
            if (requiredReceiver == syntax::AccessEffect::inout) {
                const auto storage = analyzeStorage(member->base);
                if (!storage.writable) {
                    report(
                            TypeCheckingDiagnosticId::assignmentToImmutable,
                            member->base->span,
                            "cannot call mutating method through immutable binding '" +
                                    storage.immutableName + "'");
                }
            } else if (requiredReceiver == syntax::AccessEffect::consuming &&
                       !isConsumable(member->base)) {
                report(
                        TypeCheckingDiagnosticId::invalidConsumeArgument,
                        member->base->span,
                        "consuming receiver must be an owning local, consuming parameter, or temporary",
                        "declare the parameter 'consuming' or the method 'consuming', or bind the "
                        "value to a local first");
            } else if (requiredReceiver == syntax::AccessEffect::consuming &&
                       semanticTarget != nullptr &&
                       semanticTarget->kind == semantic::SymbolKind::builtinMember) {
                // A builtin consuming member works on the receiver's storage in place, so it needs a
                // named place: not a temporary, an array element or a file-scope constant.
                const auto path = storagePath(member->base);
                const auto* root = path.has_value()
                        ? model->semanticModelValue->symbol(path->root)
                        : nullptr;
                const auto* rootScope = root != nullptr
                        ? model->semanticModelValue->scope(root->ownerScope)
                        : nullptr;
                const bool elementPath = path.has_value() &&
                        std::any_of(path->projections.begin(), path->projections.end(),
                                [](const auto& projection) { return !projection.has_value(); });
                if (!path.has_value() || elementPath ||
                    (rootScope != nullptr && (rootScope->kind == semantic::ScopeKind::file ||
                                              rootScope->kind == semantic::ScopeKind::module))) {
                    report(
                            TypeCheckingDiagnosticId::invalidConsumeArgument,
                            member->base->span,
                            "'" + semanticTarget->name + "' needs a value stored in a variable, "
                            "parameter or field, not a temporary, an array element or a top-level constant",
                            elementPath
                                    ? "remove the element with '&array.removeLast()' and call '" +
                                              semanticTarget->name + "' on the result"
                                    : "bind the value to a 'var' first, then consume that");
                }
            }
        }
        collectCallReceiverAccess(expression, semanticTarget, accesses);

        std::optional<TypeId> receiverArgumentType;
        std::optional<TypeId> receiverResultType;
        if (semanticTarget != nullptr &&
            semanticTarget->kind == semantic::SymbolKind::builtinMember &&
            expression->callee->kind == syntax::Kind::memberExpr) {
            const auto member =
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
            const auto base = model->typeOf(member->base);
            const auto* baseType = base.has_value()
                    ? model->typeContext.type(*base)
                    : nullptr;
            if (semanticTarget->name == "append" &&
                baseType != nullptr && baseType->kind == TypeKind::array &&
                baseType->arguments.size() == 1) {
                receiverArgumentType = baseType->arguments[0];
            } else if (semanticTarget->name == "removeLast" &&
                       baseType != nullptr && baseType->kind == TypeKind::array &&
                       baseType->arguments.size() == 1) {
                const auto elementType = baseType->arguments[0];
                receiverResultType = model->typeContext.optionalType(elementType);
            } else if (semanticTarget->name == "join" &&
                       baseType != nullptr && baseType->kind == TypeKind::task &&
                       baseType->arguments.size() == 1) {
                receiverResultType = baseType->arguments[0];
            } else if (semanticTarget->name == "send" &&
                       baseType != nullptr && baseType->kind == TypeKind::channelSender &&
                       baseType->arguments.size() == 1) {
                receiverArgumentType = baseType->arguments[0];
                receiverResultType = model->typeContext.channelResultType(model->typeContext.voidType());
            } else if (semanticTarget->name == "receive" &&
                       baseType != nullptr && baseType->kind == TypeKind::channelReceiver &&
                       baseType->arguments.size() == 1) {
                const auto element = baseType->arguments[0];
                receiverResultType = model->typeContext.channelResultType(element);
            } else if (semanticTarget->name == "get" &&
                       baseType != nullptr && baseType->kind == TypeKind::dictionary &&
                       baseType->arguments.size() == 2) {
                receiverArgumentType = baseType->arguments[0];
                // Copy the value type out first: creating the Optional can grow the type table and
                // invalidate `baseType`.
                const auto valueType = baseType->arguments[1];
                receiverResultType = model->typeContext.optionalType(valueType);
                if (const auto reason = model->noncopyableReason(valueType)) {
                    report(
                            TypeCheckingDiagnosticId::copyOfNoncopyable,
                            expression->span,
                            "'get' returns a copy of the stored value, and '" +
                                    model->typeContext.displayName(valueType) +
                                    "' is noncopyable: " + *reason,
                            "a noncopyable element cannot be read without removing it, which "
                            "is not supported yet");
                }
            }
        }
        const bool literalAddressCall = target.has_value() &&
                model->nativeBinding(*target) != nullptr &&
                intrinsic::readsLiteral(model->nativeBinding(*target)->symbol);
        for (size_t index = 0; index < expression->arguments.size(); ++index) {
            const auto& argument = expression->arguments[index];
            if (literalAddressCall && index == 0) {
                // The data is read at compile time: it must be a string literal.
                const auto literal = argument->value != nullptr &&
                        argument->value->kind == syntax::Kind::literalExpr
                        ? std::static_pointer_cast<syntax::LiteralExprSyntax>(argument->value)
                        : nullptr;
                if (literal == nullptr || literal->literal == nullptr ||
                    literal->literal->kind != stringLiteral) {
                    report(TypeCheckingDiagnosticId::invalidCallArguments,
                           argument->value != nullptr ? argument->value->span : argument->span,
                           model->nativeBinding(*target)->symbol + " requires a string literal argument");
                } else {
                    literalAddressArgument = argument->value.get();
                }
            }
            const auto parameterIndex = semanticTarget == nullptr
                    ? std::optional<size_t>()
                    : parameterIndexForArgument(*semanticTarget, *argument, index);
            const auto expected = receiverArgumentType.has_value() &&
                                  parameterIndex == std::optional<size_t>(0)
                    ? receiverArgumentType
                    : signature != nullptr && parameterIndex.has_value() &&
                      *parameterIndex < signature->parameters.size()
                    ? std::optional<TypeId>(signature->parameters[*parameterIndex])
                    : std::optional<TypeId>();
            const auto actual = preChecked[index].has_value()
                    ? *preChecked[index]
                    : checkExpression(argument->value, expected);
            // A memberwise initializer, an enum case and `append` store their argument, so it
            // is an owned context; every other parameter borrows or is marked `consume`.
            const auto storesArgument = semanticTarget != nullptr && storesItsArguments(*semanticTarget);
            if (storesArgument) requireStoredArgument(*argument, actual);
            if (semanticTarget != nullptr && parameterIndex.has_value() &&
                !(storesArgument && consumesArgument(*argument))) {
                checkCallArgumentAccess(*semanticTarget, *parameterIndex, *argument);
                const auto effect = semanticTarget->callable->parameters[*parameterIndex].access;
                collectCallArgumentAccess(*argument, effect, accesses);
            }
            if (expected.has_value() && actual.has_value()) {
                requireAssignable(*actual, *expected, argument->value->span);
            }
            if (semanticTarget != nullptr && semanticTarget->kind == semantic::SymbolKind::builtinFunction &&
                semanticTarget->name == "print" && actual.has_value() && (containsFunctionType(*actual) ||
                                                       model->typeContext.mentionsTypeParameter(*actual))) {
                report(TypeCheckingDiagnosticId::typeMismatch, argument->value->span,
                       "'print' cannot print a value of type '" +
                               model->typeContext.displayName(*actual) + "'",
                       "print a number, a Bool or a String");
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
                    (symbol == nullptr ? std::string("this expression") : "value '" + symbol->name + "'") +
                            " of type '" + model->typeContext.displayName(calleeType) +
                            "' is not callable",
                    isOptionalFunction(calleeType)
                            ? std::optional<std::string>(
                                      "unwrap it first with a 'match', or write 'value?(...)' to return nil from the "
                                      "enclosing function when it is nil")
                            : std::nullopt);
        }
        return receiverResultType.value_or(
                signature == nullptr ? model->typeContext.errorType() : signature->result);
    }

    // `channel(capacity: n, sender: &s, receiver: &r)` (spec 21.9.1): the two outputs are `initializing`
    // arguments of a sender and a receiver type with the same element type.
    TypeId checkChannel(const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol& target) {
        const auto error = model->typeContext.errorType();
        // Labels, their order and the argument count are name resolution's ordinary call rules.
        if (expression->arguments.size() != 3) return error;
        const auto capacityType = checkExpression(expression->arguments[0]->value, model->typeContext.intType());
        if (capacityType.has_value()) {
            requireAssignable(*capacityType, model->typeContext.intType(), expression->arguments[0]->value->span);
        }
        // An integer literal capacity (optionally negated) below 1 is a compile-time error.
        auto literal = expression->arguments[0]->value;
        bool negated = false;
        if (literal != nullptr && literal->kind == syntax::Kind::prefixExpr) {
            const auto prefix = std::static_pointer_cast<syntax::PrefixExprSyntax>(literal);
            if (prefix->op != nullptr && prefix->op->kind == minus) {
                negated = true;
                literal = prefix->operand;
            }
        }
        if (literal != nullptr && literal->kind == syntax::Kind::literalExpr) {
            const auto value = std::static_pointer_cast<syntax::LiteralExprSyntax>(literal);
            if (value->literal != nullptr && value->literal->kind == decimalLiteral) {
                const bool belowOne = negated || value->literal->intValue < 1;
                if (belowOne) {
                    report(TypeCheckingDiagnosticId::channelCapacity, expression->arguments[0]->value->span,
                           "a channel needs a capacity of at least 1",
                           "pass 1 or more, for example channel(capacity: 8, sender: &sender, receiver: &receiver)");
                }
            }
        }
        checkCallArgumentAccess(target, 0, *expression->arguments[0]);
        const auto diagnosticsBefore = diagnostics.size();
        std::vector<CallAccess> accesses;
        std::vector<TypeId> endTypes;
        for (size_t index = 1; index < 3; ++index) {
            const auto& argument = expression->arguments[index];
            const auto type = checkExpression(argument->value);
            checkCallArgumentAccess(target, index, *argument);
            collectCallArgumentAccess(*argument, syntax::AccessEffect::initializing, accesses);
            endTypes.push_back(type.value_or(error));
        }
        validateCallExclusivity(accesses);
        const auto* senderType = model->typeContext.type(endTypes[0]);
        const auto* receiverType = model->typeContext.type(endTypes[1]);
        // An operand that already failed (a marker, a non-variable, the same variable twice) has its own diagnostic.
        if (endTypes[0] == error || endTypes[1] == error || diagnostics.size() != diagnosticsBefore) {
            return model->typeContext.voidType();
        }
        if (senderType == nullptr || receiverType == nullptr ||
            senderType->kind != TypeKind::channelSender || receiverType->kind != TypeKind::channelReceiver ||
            senderType->arguments.size() != 1 || receiverType->arguments.size() != 1 ||
            senderType->arguments[0] != receiverType->arguments[0]) {
            report(TypeCheckingDiagnosticId::channelEndTypes, expression->span,
                   "'channel' needs a ChannelSender<T> and a ChannelReceiver<T> of the same element type, but got '" +
                           model->typeContext.displayName(endTypes[0]) + "' and '" +
                           model->typeContext.displayName(endTypes[1]) + "'",
                   "declare var sender: ChannelSender<Int> and var receiver: ChannelReceiver<Int>, then "
                   "channel(capacity: 8, sender: &sender, receiver: &receiver)");
        }
        return model->typeContext.voidType();
    }

    // `spawn(function: f, input: consume x)` (spec 21.2): `f` must name one top-level function with a
    // single consuming parameter; the call has type `JoinHandle<R>` for the function's result `R`.
    TypeId checkSpawn(const syntax::CallExprSyntax::Ptr& expression) {
        const auto error = model->typeContext.errorType();
        // Labels, their order and the argument count are checked with the ordinary call rules.
        if (expression->arguments.size() != 2) return error;
        const auto& functionArgument = expression->arguments[0];
        const auto& inputArgument = expression->arguments[1];
        const auto span = functionArgument->value != nullptr ? functionArgument->value->span : functionArgument->span;

        // `function:` is read as a name, not checked as a value, so a member reference that name
        // resolution deferred (an instance's member) must be marked handled here or it is reported twice.
        auto functionExpression = functionArgument->value;
        while (functionExpression != nullptr && functionExpression->kind == syntax::Kind::parenthesizedExpr) {
            functionExpression = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(functionExpression)->expression;
        }
        if (functionExpression != nullptr) markDeferredReferenceHandled(functionExpression);
        std::optional<semantic::SymbolId> named;
        if (functionExpression != nullptr &&
            (functionExpression->kind == syntax::Kind::nameExpr ||
             functionExpression->kind == syntax::Kind::memberExpr)) {
            named = model->referencedSymbol(functionExpression);
        }

        const auto* target = named.has_value() ? model->semanticModelValue->symbol(*named) : nullptr;
        if (target == nullptr || (target->kind != semantic::SymbolKind::function &&
                                  target->kind != semantic::SymbolKind::method &&
                                  target->kind != semantic::SymbolKind::builtinFunction)) {
            // An unknown name is name resolution's diagnostic; this is a name that is not a function
            // (a variable, a call, any other expression).
            if (functionExpression != nullptr && !model->referencedSymbol(functionExpression).has_value() &&
                functionExpression->kind == syntax::Kind::nameExpr) {
                return error;
            }
            report(TypeCheckingDiagnosticId::spawnTargetNotFunction, span,
                   "'function:' must name a top-level function; a function value held in a variable or an expression is not accepted here (spec 21.2.1)",
                   "pass the name of a top-level function, for example spawn(function: work, input: consume value)");
            return error;
        }
        const bool isMain = target->isRootModule && target->name == "main";
        const bool isGeneric = model->genericParameters(*named) != nullptr;
        if (target->kind != semantic::SymbolKind::function || isMain || isGeneric ||
            model->nativeBinding(*named) != nullptr) {
            report(TypeCheckingDiagnosticId::spawnTargetUnsupported, span,
                   "'" + target->name + "' cannot be a task: " +
                           (target->kind == semantic::SymbolKind::method ? "it is a method"
                            : isMain ? "it is the program entry point"
                            : isGeneric ? "it is generic (spec 20.4.1)"
                            : target->kind == semantic::SymbolKind::builtinFunction ? "it is a builtin function"
                                                                                    : "it is a native binding"),
                   "a task runs a top-level Joyeer function other than 'main'");
            return error;
        }
        const auto* signature = model->callable(*named);
        const auto& parameters = target->callable.has_value()
                ? target->callable->parameters
                : std::vector<semantic::CallableParameter> {};
        if (signature == nullptr || parameters.size() != 1 ||
            parameters[0].access != syntax::AccessEffect::consuming) {
            const bool wrongCount = parameters.size() != 1;
            report(TypeCheckingDiagnosticId::spawnTargetParameters, span,
                   "'" + target->name + "' must have exactly one parameter, and it must be 'consuming'; it has " +
                           (wrongCount ? std::to_string(parameters.size()) + " parameter(s)"
                                       : std::string("one parameter that is not 'consuming'")),
                   wrongCount
                           ? "wrap the input in a struct (an empty struct if there is none) and take it as the one consuming parameter"
                           : "declare the parameter 'consuming'; a borrowed or inout parameter cannot cross to another thread");

            return error;
        }
        if (signature->result == model->typeContext.neverType()) {
            report(TypeCheckingDiagnosticId::spawnTargetNever, span,
                   "'" + target->name + "' never returns, so its task could not be joined",
                   "declare a result type, or call the function directly");
            return error;
        }

        // The input is the target's one consuming argument, checked as for a direct call.
        const auto expected = signature->parameters.at(0);
        const auto actual = checkExpression(inputArgument->value, expected);
        checkCallArgumentAccess(*target, 0, *inputArgument);
        if (actual.has_value()) requireAssignable(*actual, expected, inputArgument->value->span);
        std::vector<CallAccess> accesses;
        collectCallArgumentAccess(*inputArgument, syntax::AccessEffect::consuming, accesses);
        validateCallExclusivity(accesses);
        return model->typeContext.taskType(signature->result);
    }

    void collectCallReceiverAccess(
            const syntax::CallExprSyntax::Ptr& expression,
            const semantic::Symbol* target,
            std::vector<CallAccess>& accesses,
            bool sustained = true) const {
        const auto receiverEffect = receiverEffectOf(target);
        if (!receiverEffect.has_value() ||
            expression->callee->kind != syntax::Kind::memberExpr) {
            collectEvaluationAccesses(expression->callee, accesses);
            return;
        }

        const auto member =
                std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
        const auto path = storagePath(member->base);
        if (path.has_value()) {
            collectSubscriptIndexAccesses(member->base, accesses);
            accesses.push_back(CallAccess {
                *path,
                *receiverEffect,
                member->base->span,
                sustained,
            });
        } else {
            collectEvaluationAccesses(member->base, accesses);
        }
    }

    void collectCallArgumentAccess(
            const syntax::CallArgumentSyntax& argument,
            syntax::AccessEffect effect,
            std::vector<CallAccess>& accesses,
            bool sustained = true) const {
        const auto path = storagePath(argument.value);
        if (path.has_value()) {
            collectSubscriptIndexAccesses(argument.value, accesses);
            accesses.push_back(CallAccess {
                *path,
                effect,
                argument.value->span,
                sustained,
            });
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
            const syntax::NodePtr& expression,
            std::vector<CallAccess>& accesses) const {
        if (expression == nullptr) return;
        const auto value = isExpressionKind(expression->kind)
                ? std::static_pointer_cast<syntax::ExprSyntax>(expression)
                : nullptr;
        const auto path = storagePath(value);
        if (path.has_value()) {
            collectSubscriptIndexAccesses(value, accesses);
            accesses.push_back(CallAccess {
                *path,
                syntax::AccessEffect::borrowing,
                expression->span,
                false,
            });
            return;
        }
        switch (expression->kind) {
            case syntax::Kind::bindingDecl:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(expression)
                                ->initializer,
                        accesses);
                break;
            case syntax::Kind::whileStmt: {
                const auto loop = std::static_pointer_cast<syntax::WhileStmtSyntax>(expression);
                collectEvaluationAccesses(loop->condition, accesses);
                collectEvaluationAccesses(loop->body, accesses);
                break;
            }
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
            case syntax::Kind::propagateExpr:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression)->operand,
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
                    collectSubscriptIndexAccesses(assignment->target, accesses);
                    accesses.push_back(CallAccess {
                        *target,
                        syntax::AccessEffect::inout,
                        assignment->target->span,
                        false,
                    });
                } else {
                    collectEvaluationAccesses(assignment->target, accesses);
                }
                collectEvaluationAccesses(assignment->value, accesses);
                break;
            }
            case syntax::Kind::memberExpr:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base,
                        accesses);
                break;
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                collectEvaluationAccesses(subscript->base, accesses);
                collectEvaluationAccesses(subscript->index, accesses);
                break;
            }
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                const auto target = model->callTarget(call);
                const auto* symbol = target.has_value()
                        ? model->semanticModelValue->symbol(*target)
                        : nullptr;
                collectCallReceiverAccess(call, symbol, accesses, false);
                // A call through a function value takes its effects from the value's type, by
                // position (spec 5.6.1).
                std::vector<syntax::AccessEffect> valueEffects;
                if (!target.has_value()) {
                    const auto calleeType = model->typeOf(call->callee);
                    const auto* record = calleeType.has_value() ? model->typeContext.type(*calleeType) : nullptr;
                    if (record != nullptr && record->kind == TypeKind::function) valueEffects = record->effects;
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
                            : index < valueEffects.size() ? valueEffects[index]
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
            case syntax::Kind::blockExpr: {
                const auto block =
                        std::static_pointer_cast<syntax::BlockExprSyntax>(expression);
                for (const auto& item : block->items) {
                    collectEvaluationAccesses(item, accesses);
                }
                break;
            }
            case syntax::Kind::ifExpr: {
                const auto conditional =
                        std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                collectEvaluationAccesses(conditional->condition, accesses);
                collectEvaluationAccesses(conditional->thenBranch, accesses);
                collectEvaluationAccesses(conditional->elseBranch, accesses);
                break;
            }
            case syntax::Kind::matchExpr: {
                const auto match = std::static_pointer_cast<syntax::MatchExprSyntax>(expression);
                collectEvaluationAccesses(match->scrutinee, accesses);
                for (const auto& arm : match->arms) {
                    collectEvaluationAccesses(arm->body, accesses);
                }
                break;
            }
            case syntax::Kind::returnExpr:
                collectEvaluationAccesses(
                        std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value,
                        accesses);
                break;
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
                // Transient evaluations finish before later arguments acquire storage.
                if (!accesses[left].sustained) continue;
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
                                "' in the same call",
                            std::nullopt,
                            std::nullopt,
                            { DiagnosticSourceNote {
                                accesses[left].span.offset,
                                accesses[left].span.length,
                                "conflicting " +
                                    std::string(accessEffectName(accesses[left].effect)) +
                                    " access occurs here",
                            } });
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

    // `target op= value` reads the target, applies the binary operator, and
    // stores the result, so the operator's result type must be the target type.
    TypeId checkCompoundAssignment(const syntax::AssignmentExprSyntax::Ptr& assignment,
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
            if (baseType != nullptr && baseType->kind == TypeKind::dictionary) {
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
        checkArgumentAccess(parameters[parameterIndex].access, argument);
    }

    // The marker and the storage an argument needs for a parameter with effect `required`, shared by
    // calls by name and calls through a function value (spec 4.2, 5.6.1).
    void checkArgumentAccess(syntax::AccessEffect required, const syntax::CallArgumentSyntax& argument) {
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

    static bool storesItsArguments(const semantic::Symbol& target) {
        if (target.kind == semantic::SymbolKind::builtinMember) return target.name == "append";
        return target.callable.has_value() &&
                (target.callable->kind == semantic::CallableKind::structureInitializer ||
                 target.callable->kind == semantic::CallableKind::enumCase);
    }

    // A place names existing storage; using one where an owned value is needed copies it.
    // Anything else (a call result, a literal, a constructed value) is a temporary.
    bool isPlaceExpression(const syntax::ExprPtr& expression) const {
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
    bool assignsWholeSelf(const syntax::ExprPtr& target) const {
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

    static bool consumesArgument(const syntax::CallArgumentSyntax& argument) {
        return argument.accessMarker != nullptr && argument.accessMarker->kind == kwConsume;
    }

    // `consume x` is a move wherever a value is stored: a binding, an assignment, an aggregate
    // element. Elsewhere it is only an argument marker or a consuming receiver.
    void permitMove(const syntax::ExprPtr& expression) {
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
    void requireStoredArgument(
            const syntax::CallArgumentSyntax& argument,
            std::optional<TypeId> type) {
        if (!consumesArgument(argument)) {
            requireNoImplicitCopy(argument.value, type);
            return;
        }
        if (!isConsumable(argument.value)) {
            report(
                    TypeCheckingDiagnosticId::invalidConsumeArgument,
                    argument.value->span,
                    "consuming argument must be an owning local, consuming parameter, or temporary");
        }
    }

    // The places a value-producing expression copies: the expression itself when it names
    // storage, or the branch values of an `if`, `match` or block, or the operand of `?`.
    void collectCopiedPlaces(const syntax::ExprPtr& expression,
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
            case syntax::Kind::ifExpr: {
                const auto branch = std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                collectCopiedPlaces(branch->thenBranch, places);
                collectCopiedPlaces(branch->elseBranch, places);
                return;
            }
            case syntax::Kind::matchExpr:
                for (const auto& arm :
                     std::static_pointer_cast<syntax::MatchExprSyntax>(expression)->arms) {
                    collectCopiedPlaces(arm->body, places);
                }
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
    void requireNoImplicitCopy(
            const syntax::ExprPtr& expression,
            std::optional<TypeId> type,
            bool movesOwningName = false) {
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

        if (!requireAccessible(*caseSymbol, expression->span)) {
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
        } else if (type->kind == TypeKind::enumeration) {
            for (const auto& parameter : symbol->callable->parameters) {
                if (!parameter.type.has_value()) return std::nullopt;
                const auto payload = model->typeContext.typeForSymbol(*parameter.type);
                if (!payload.has_value()) return std::nullopt;
                signature.parameters.push_back(*payload);
            }
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
            requireStoredArgument(*argument, actual);
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
        std::vector<PatternCoverage> payloads;
    };

    TypeId checkMatch(
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

        for (const auto& arm : expression->arms) {
            const auto previousDiagnostics = diagnostics.size();
            coverage.push_back(checkPattern(arm->pattern, scrutinee));
            validPatterns &= diagnostics.size() == previousDiagnostics;
            const auto armType = checkExpression(arm->body, expected).value_or(
                    model->typeContext.errorType());
            recordNodeType(arm, armType);
            result = result.has_value()
                    ? std::optional<TypeId>(commonType(*result, armType, arm->body->span))
                    : std::optional<TypeId>(armType);
        }

        if (validPatterns) checkMatchExhaustiveness(expression, scrutinee, coverage);
        requireOwnedScrutineeForMovedBindings(expression);
        return result.value_or(model->typeContext.voidType());
    }

    bool patternBindsNoncopyable(const syntax::PatternPtr& pattern) const {
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
    void requireOwnedScrutineeForMovedBindings(const syntax::MatchExprSyntax::Ptr& expression) {
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

    PatternCoverage checkPattern(
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

    TypeId patternLiteralType(const Token::Ptr& literal, TypeId expected) {
        if (literal == nullptr) return model->typeContext.errorType();
        switch (literal->kind) {
            case decimalLiteral:
            case floatLiteral: return checkNumericLiteral(literal, expected);
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

        if (!requireAccessible(*caseSymbol, pattern->span)) return {};
        recordResolvedReference(pattern, *caseSymbol);
        return PatternCoverage {
            false,
            caseSymbol,
            std::nullopt,
            validatePatternArguments(*pattern, *caseSymbol, *signature),
        };
    }

    std::vector<PatternCoverage> validatePatternArguments(
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

    using PatternRow = std::vector<const PatternCoverage*>;

    bool patternMatrixIsExhaustive(
            const std::vector<TypeId>& columns,
            const std::vector<PatternRow>& rows,
            size_t& remainingSpecializations,
            size_t depth = 0) const {
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

    void checkMatchExhaustiveness(
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
        rejectInvalidDictionaryKey(*keyType, expression->entries.empty()
                ? expression->span : expression->entries.front()->key->span);
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
        case TypeKind::task: return "task";
        case TypeKind::channelSender: return "channel-sender";
        case TypeKind::channelReceiver: return "channel-receiver";
        case TypeKind::function: return "function";
        case TypeKind::typeParameter: return "type-parameter";
        case TypeKind::int8: return "int8";
        case TypeKind::int16: return "int16";
        case TypeKind::int32: return "int32";
        case TypeKind::int64: return "int64";
        case TypeKind::uint16: return "uint16";
        case TypeKind::uint32: return "uint32";
        case TypeKind::uint64: return "uint64";
        case TypeKind::unsignedInteger: return "uint";
        case TypeKind::float32: return "float32";
        case TypeKind::float64: return "float64";
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
        case TypeCheckingDiagnosticId::invalidPropagationOperand:
            return "type-checking.invalid-propagation-operand";
        case TypeCheckingDiagnosticId::invalidPropagationContext:
            return "type-checking.invalid-propagation-context";
        case TypeCheckingDiagnosticId::mismatchedPropagationError:
            return "type-checking.mismatched-propagation-error";
        case TypeCheckingDiagnosticId::unknownMember:
            return "type-checking.unknown-member";
        case TypeCheckingDiagnosticId::inaccessibleDeclaration:
            return "type-checking.inaccessible-declaration";
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
        case TypeCheckingDiagnosticId::copyOfNoncopyable:
            return "type-checking.copy-of-noncopyable";
        case TypeCheckingDiagnosticId::assignmentToSelfInDeinit:
            return "type-checking.assignment-to-self-in-deinit";
        case TypeCheckingDiagnosticId::linearReserved:
            return "type-checking.linear-reserved";
        case TypeCheckingDiagnosticId::linearInDeinitType:
            return "type-checking.linear-in-deinit-type";
        case TypeCheckingDiagnosticId::linearCollectionElement:
            return "type-checking.linear-collection-element";
        case TypeCheckingDiagnosticId::linearTopLevelBinding:
            return "type-checking.linear-top-level-binding";
        case TypeCheckingDiagnosticId::linearStructWithoutFields:
            return "type-checking.linear-struct-without-fields";
        case TypeCheckingDiagnosticId::spawnTargetNotFunction:
            return "type-checking.spawn-target-not-function";
        case TypeCheckingDiagnosticId::spawnTargetUnsupported:
            return "type-checking.spawn-target-unsupported";
        case TypeCheckingDiagnosticId::spawnTargetParameters:
            return "type-checking.spawn-target-parameters";
        case TypeCheckingDiagnosticId::spawnTargetNever:
            return "type-checking.spawn-target-never";
        case TypeCheckingDiagnosticId::channelEndTypes:
            return "type-checking.channel-end-types";
        case TypeCheckingDiagnosticId::channelCapacity:
            return "type-checking.channel-capacity";
        case TypeCheckingDiagnosticId::linearChannelElement:
            return "type-checking.linear-channel-element";
        case TypeCheckingDiagnosticId::channelElementType:
            return "type-checking.channel-element-type";
        case TypeCheckingDiagnosticId::invalidFunctionType:
            return "type-checking.invalid-function-type";
        case TypeCheckingDiagnosticId::notAFunctionValue:
            return "type-checking.not-a-function-value";
        case TypeCheckingDiagnosticId::labeledFunctionValueCall:
            return "type-checking.labeled-function-value-call";
        case TypeCheckingDiagnosticId::protocolUsedAsType:
            return "type-checking.protocol-used-as-type";
        case TypeCheckingDiagnosticId::invalidProtocolMember:
            return "type-checking.invalid-protocol-member";
        case TypeCheckingDiagnosticId::reservedProtocolName:
            return "type-checking.reserved-protocol-name";
        case TypeCheckingDiagnosticId::invalidConformance:
            return "type-checking.invalid-conformance";
        case TypeCheckingDiagnosticId::missingWitness:
            return "type-checking.missing-witness";
        case TypeCheckingDiagnosticId::witnessMismatch:
            return "type-checking.witness-mismatch";
        case TypeCheckingDiagnosticId::witnessNotAccessible:
            return "type-checking.witness-not-accessible";
        case TypeCheckingDiagnosticId::orphanConformance:
            return "type-checking.orphan-conformance";
        case TypeCheckingDiagnosticId::duplicateConformance:
            return "type-checking.duplicate-conformance";
        case TypeCheckingDiagnosticId::invalidGenericDeclaration:
            return "type-checking.invalid-generic-declaration";
        case TypeCheckingDiagnosticId::unsatisfiedConstraint:
            return "type-checking.unsatisfied-constraint";
        case TypeCheckingDiagnosticId::cannotInferTypeParameter:
            return "type-checking.cannot-infer-type-parameter";
        case TypeCheckingDiagnosticId::conflictingTypeArgument:
            return "type-checking.conflicting-type-argument";
        case TypeCheckingDiagnosticId::linearTypeArgument:
            return "type-checking.linear-type-argument";
        case TypeCheckingDiagnosticId::invalidTypeArgument:
            return "type-checking.invalid-type-argument";
        case TypeCheckingDiagnosticId::unknownMemberOnTypeParameter:
            return "type-checking.unknown-member-on-type-parameter";
        case TypeCheckingDiagnosticId::genericNativeBinding:
            return "type-checking.generic-native-binding";
        case TypeCheckingDiagnosticId::expandingGenericRecursion:
            return "type-checking.expanding-generic-recursion";
        case TypeCheckingDiagnosticId::instantiationLimit:
            return "type-checking.instantiation-limit";
        case TypeCheckingDiagnosticId::invalidConsumeArgument:
            return "type-checking.invalid-consume-argument";
        case TypeCheckingDiagnosticId::invalidInitializingArgument:
            return "type-checking.invalid-initializing-argument";
        case TypeCheckingDiagnosticId::overlappingAccess:
            return "type-checking.overlapping-access";
        case TypeCheckingDiagnosticId::notCallable:
            return "type-checking.not-callable";
        case TypeCheckingDiagnosticId::invalidCallArguments:
            return "type-checking.invalid-call-arguments";
        case TypeCheckingDiagnosticId::invalidLoopControl:
            return "type-checking.invalid-loop-control";
        case TypeCheckingDiagnosticId::notIterable:
            return "type-checking.not-iterable";
        case TypeCheckingDiagnosticId::unresolvedReference:
            return "type-checking.unresolved-reference";
        case TypeCheckingDiagnosticId::invalidAnnotation:
            return "type-checking.invalid-annotation";
        case TypeCheckingDiagnosticId::invalidNativeDeclaration:
            return "type-checking.invalid-native-declaration";
        case TypeCheckingDiagnosticId::unsupportedNativeSignature:
            return "type-checking.unsupported-native-signature";
        case TypeCheckingDiagnosticId::unauthorizedNativeBinding:
            return "type-checking.unauthorized-native-binding";
        case TypeCheckingDiagnosticId::conflictingNativeSymbol:
            return "type-checking.conflicting-native-symbol";
        case TypeCheckingDiagnosticId::invalidNumericLiteral:
            return "type-checking.invalid-numeric-literal";
        case TypeCheckingDiagnosticId::managedTypeInCoreUnit:
            return "type-checking.managed-type-in-core-unit";
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