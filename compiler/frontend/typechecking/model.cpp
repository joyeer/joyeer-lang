#include "compiler/frontend/typechecking.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <utility>

namespace joyeer::typing {

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

bool TypeCheckedModel::isArrayRoleType(TypeId type) const {
    return typeContext.isArray(type);
}

bool TypeCheckedModel::isOwnedArgumentContext(semantic::SymbolId function, size_t parameter) const {
    const auto* method = semanticModelValue->symbol(function);
    const auto* owner = method != nullptr && method->containingSymbol.has_value()
            ? semanticModelValue->symbol(*method->containingSymbol) : nullptr;
    const auto* signature = callable(function);
    const auto record = owner != nullptr ? collectionRoles.find(owner->id) : collectionRoles.end();
    if (method == nullptr || method->kind != semantic::SymbolKind::method || owner == nullptr ||
        record == collectionRoles.end() ||
        !method->callable.has_value() || parameter >= method->callable->parameters.size() ||
        method->callable->parameters[parameter].access != syntax::AccessEffect::consuming ||
        signature == nullptr || parameter >= signature->parameters.size()) {
        return false;
    }
    // An owned argument context is a consuming parameter declared with exactly one of the type parameters the
    // collection owns elements of (decision 0026 A2).
    const auto* declared = typeContext.type(signature->parameters[parameter]);
    return declared != nullptr && declared->kind == TypeKind::typeParameter &&
            std::find(record->second.ownedParameters.begin(), record->second.ownedParameters.end(),
                      declared->symbol) != record->second.ownedParameters.end();
}

std::optional<semantic::SymbolId> TypeCheckedModel::arrayRoleMethod(
        TypeId type, semantic::LibraryRole role) const {
    if (!isArrayRoleType(type)) return std::nullopt;
    return collectionRoleMethod(type, role);
}

bool TypeCheckedModel::isCollectionRoleType(TypeId type) const {
    if (isArrayRoleType(type)) return true;
    const auto* record = typeContext.type(type);
    return record != nullptr && record->kind == TypeKind::structure && collectionRoles.contains(record->symbol);
}

bool TypeCheckedModel::isDictRoleType(TypeId type) const {
    const auto* record = typeContext.type(type);
    if (record == nullptr || record->kind != TypeKind::structure) return false;
    const auto found = collectionRoles.find(record->symbol);
    return found != collectionRoles.end() && found->second.kind == semantic::LibraryRole::dictType;
}

std::optional<std::array<semantic::SymbolId, 2>> TypeCheckedModel::dictEntryFields() const {
    return dictEntryFieldSymbols;
}

std::optional<TypeId> TypeCheckedModel::dictEntryType(TypeId dictionary) const {
    const auto found = dictEntryTypes.find(dictionary);
    if (found == dictEntryTypes.end()) return std::nullopt;
    return found->second;
}

std::optional<semantic::SymbolId> TypeCheckedModel::stringRoleFunction(semantic::LibraryRole role) const {
    const auto found = stringRoles.find(role);
    if (found == stringRoles.end()) return std::nullopt;
    return found->second;
}

std::optional<semantic::SymbolId> TypeCheckedModel::collectionRoleMethod(
        TypeId type, semantic::LibraryRole role) const {
    const auto* record = typeContext.type(type);
    if (record == nullptr || record->kind != TypeKind::structure) return std::nullopt;
    const auto found = collectionRoles.find(record->symbol);
    if (found == collectionRoles.end()) return std::nullopt;
    const auto method = found->second.methods.find(role);
    if (method == found->second.methods.end()) return std::nullopt;
    return method->second;
}

bool TypeCheckedModel::isDeclaredLinear(TypeId type) const {
    const auto structure = structureDeclaration(type);
    return structure != nullptr && structure->isLinear;
}

// A field or payload type declared inside a generic type, seen from one instance `owner` of it:
// the type's parameters replaced by the instance's arguments (spec 20.12).
TypeId TypeCheckedModel::memberTypeOf(TypeId owner, TypeId declared) const {
    const auto* record = typeContext.type(owner);
    if (record == nullptr || record->arguments.empty()) return declared;
    const auto* parameters = genericParameters(record->symbol);
    if (parameters == nullptr || parameters->size() != record->arguments.size()) return declared;
    std::unordered_map<semantic::SymbolId, TypeId> substitution;
    for (size_t index = 0; index < parameters->size(); ++index) {
        substitution.emplace((*parameters)[index], record->arguments[index]);
    }
    return typeContext.substitute(declared, substitution);
}

bool TypeCheckedModel::isLinear(TypeId type) const {
    std::vector<TypeId> visiting;
    std::unordered_set<TypeId> clean;
    return isLinear(type, visiting, clean);
}

syntax::ExprPtr TypeCheckedModel::linearConstructionMoveSource(
        const syntax::ExprPtr& expression, bool marked) const {
    const auto unwrap = [](syntax::ExprPtr value) {
        while (value != nullptr && value->kind == syntax::Kind::parenthesizedExpr) {
            value = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(value)->expression;
        }
        return value;
    };
    auto source = unwrap(expression);
    if (!marked && source != nullptr && source->kind == syntax::Kind::accessExpr) {
        const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(source);
        marked = access->marker != nullptr && access->marker->kind == kwConsume;
        source = unwrap(access->operand);
    }
    if (!marked || source == nullptr) return nullptr;
    const auto type = typeOf(source);
    if (!type.has_value() || !isLinear(*type)) return nullptr;

    auto place = source;
    while (place != nullptr && place->kind == syntax::Kind::memberExpr) {
        const auto field = referencedSymbol(place);
        const auto* declaration = field.has_value() ? semanticModelValue->symbol(*field) : nullptr;
        if (declaration == nullptr || declaration->kind != semantic::SymbolKind::structureField) return nullptr;
        place = unwrap(std::static_pointer_cast<syntax::MemberExprSyntax>(place)->base);
    }
    if (place == nullptr || place->kind != syntax::Kind::nameExpr) return nullptr;
    const auto id = referencedSymbol(place);
    const auto* root = id.has_value() ? semanticModelValue->symbol(*id) : nullptr;
    if (root == nullptr || (root->kind != semantic::SymbolKind::binding &&
                            root->kind != semantic::SymbolKind::parameter &&
                            root->kind != semantic::SymbolKind::patternBinding)) return nullptr;
    return source;
}

std::optional<std::string> TypeCheckedModel::linearEndingMethod(TypeId type, bool leaf) const {
    std::unordered_set<TypeId> visited;
    std::function<std::optional<std::string>(TypeId)> visit = [&](TypeId id) -> std::optional<std::string> {
        if (!visited.insert(id).second || !isLinear(id)) return std::nullopt;
        const auto* record = typeContext.type(id);
        if (record == nullptr) return std::nullopt;
        const auto arguments = record->arguments;
        const auto* declaration = semanticModelValue->symbol(record->symbol);
        if (!leaf && declaration != nullptr && declaration->libraryRole == semantic::LibraryRole::arrayType &&
            declaration->memberScope.has_value()) {
            const auto* members = semanticModelValue->scope(*declaration->memberScope);
            for (const auto& [name, symbol] : members->values) {
                const auto* method = semanticModelValue->symbol(symbol);
                if (method->libraryRole == semantic::LibraryRole::containerEnd) return name;
            }
        }
        if (declaration != nullptr && declaration->libraryRole == semantic::LibraryRole::taskHandle &&
            declaration->memberScope.has_value()) {
            const auto* members = semanticModelValue->scope(*declaration->memberScope);
            for (const auto& [name, symbol] : members->values) {
                const auto* method = semanticModelValue->symbol(symbol);
                if (method->libraryRole == semantic::LibraryRole::storedConsumer) return name;
            }
        }
        const bool arrayRole = declaration != nullptr && declaration->libraryRole == semantic::LibraryRole::arrayType;
        if (const auto structure = structureDeclaration(id); structure != nullptr && !(leaf && arrayRole)) {
            for (const auto& field : structure->fields) {
                const auto symbol = semanticModelValue->declaredSymbol(field);
                const auto fieldType = symbol.has_value() ? typeOf(*symbol) : std::optional<TypeId>();
                if (fieldType.has_value()) {
                    if (const auto ending = visit(memberTypeOf(id, *fieldType))) return ending;
                }
            }
        } else if (record->kind != TypeKind::function) {
            for (const auto argument : arguments) {
                if (const auto ending = visit(argument)) return ending;
            }
        }
        return std::nullopt;
    };
    return visit(type);
}

bool TypeCheckedModel::isLinear(
        TypeId id, std::vector<TypeId>& visiting, std::unordered_set<TypeId>& clean) const {
    const auto* type = typeContext.type(id);
    if (type == nullptr || clean.contains(id)) return false;
    if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) return false;
    visiting.push_back(id);
    bool linear = false;
    switch (type->kind) {
        case TypeKind::typeParameter: {
            const auto* parameter = semanticModelValue->symbol(type->symbol);
            linear = parameter != nullptr && parameter->mustTransferParameter;
            break;
        }
        case TypeKind::structure: {
            const auto structure = structureDeclaration(id);
            if (structure == nullptr) break;
            if (isCollectionRoleType(id)) {
                // Owns its elements through an address, so they decide (decisions 0025, 0027). The arguments are
                // copied first: the recursion can intern types and invalidate `type`.
                const auto arguments = type->arguments;
                for (const auto argument : arguments) {
                    if (isLinear(argument, visiting, clean)) { linear = true; break; }
                }
                break;
            }
            if (structure->isLinear) { linear = true; break; }
            for (const auto& field : structure->fields) {
                const auto fieldSymbol = semanticModelValue->declaredSymbol(field);
                const auto fieldType = fieldSymbol.has_value()
                        ? typeOf(*fieldSymbol)
                        : std::optional<TypeId>();
                if (fieldType.has_value() && isLinear(memberTypeOf(id, *fieldType), visiting, clean)) {
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
                for (const auto declaredPayload : signature->parameters) {
                    if (isLinear(memberTypeOf(id, declaredPayload), visiting, clean)) { linear = true; break; }
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
    // consumed whole. So is a library array, which holds its elements through an address.
    if (structure == nullptr || isArrayRoleType(type)) return { {} };
    // A struct ends field by field. One declared `linear` has no linear field to point at, so each
    // of its fields is a leaf, copyable ones included (destructuring); one that is linear only
    // through its fields needs just those fields consumed.
    std::vector<std::vector<semantic::SymbolId>> paths;
    for (const auto& field : structure->fields) {
        const auto fieldSymbol = semanticModelValue->declaredSymbol(field);
        const auto fieldType = fieldSymbol.has_value() ? typeOf(*fieldSymbol) : std::optional<TypeId>();
        if (!fieldSymbol.has_value() || !fieldType.has_value()) continue;
        auto fieldPaths = linearLeafPaths(memberTypeOf(type, *fieldType));
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
        case TypeKind::structure: {
            const auto* declaration = semanticModelValue->symbol(type->symbol);
            if (declaration != nullptr && declaration->libraryRole == semantic::LibraryRole::taskHandle) {
                reason = name + " is a task handle, which has exactly one owner";
                break;
            }
            if (declaration != nullptr &&
                (declaration->libraryRole == semantic::LibraryRole::channelSender ||
                 declaration->libraryRole == semantic::LibraryRole::channelReceiver)) {
                reason = name + " is a channel end, which has exactly one owner";
                break;
            }
            const auto structure = structureDeclaration(id);
            if (structure == nullptr) break;
            if (isCollectionRoleType(id)) {
                // Copyable exactly when the owned types are, although it declares `deinit` (decisions 0025, 0027).
                // The arguments are copied first: the recursion can intern types and invalidate `type`.
                const auto arguments = type->arguments;
                for (const auto argument : arguments) {
                    if (noncopyableReason(argument, visiting, clean).has_value()) {
                        reason = name + " holds the noncopyable type '" + typeContext.displayName(argument) + "'";
                        break;
                    }
                }
                break;
            }
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
                    noncopyableReason(memberTypeOf(id, *fieldType), visiting, clean).has_value()) {
                    reason = name + " has the noncopyable field '" +
                            (field->name == nullptr ? std::string() : field->name->rawValue) +
                            "' of type '" + typeContext.displayName(memberTypeOf(id, *fieldType)) + "'";
                    break;
                }
            }
            break;
        }
        case TypeKind::enumeration: {
            if (typeContext.isOptional(type) || typeContext.isResult(type)) {
                // Role-bound enums keep their wording: the instance holds the noncopyable argument.
                for (const auto argument : type->arguments) {
                    if (noncopyableReason(argument, visiting, clean).has_value()) {
                        reason = name + " holds the noncopyable type '" +
                                typeContext.displayName(argument) + "'";
                        break;
                    }
                }
                break;
            }
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
                for (const auto declaredPayload : signature->parameters) {
                    const auto payload = memberTypeOf(id, declaredPayload);
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

bool TypeCheckedModel::isValueDiscarded(const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    return id.has_value() && discardedValues.contains(*id);
}

std::optional<semantic::SymbolId> TypeCheckedModel::equatableProtocol() const {
    return semanticModelValue->roleSymbol(semantic::LibraryRole::equatable);
}

std::optional<semantic::SymbolId> TypeCheckedModel::equatableRequirement() const {
    const auto protocol = equatableProtocol();
    const auto* symbol = protocol.has_value() ? semanticModelValue->symbol(*protocol) : nullptr;
    if (symbol == nullptr || !symbol->memberScope.has_value()) return std::nullopt;
    const auto* members = semanticModelValue->scope(*symbol->memberScope);
    if (members == nullptr) return std::nullopt;
    const auto found = members->values.find("equals");
    if (found == members->values.end()) return std::nullopt;
    return found->second;
}

bool TypeCheckedModel::isBuiltinEquatable(TypeId type) const {
    const auto* record = typeContext.type(type);
    if (record == nullptr) return false;
    switch (record->kind) {
        case TypeKind::voidType:
        case TypeKind::integer:
        case TypeKind::boolean:
        case TypeKind::string:
        case TypeKind::uint8:
        case TypeKind::int8:
        case TypeKind::int16:
        case TypeKind::int32:
        case TypeKind::int64:
        case TypeKind::uint16:
        case TypeKind::uint32:
        case TypeKind::uint64:
        case TypeKind::unsignedInteger:
        case TypeKind::float32:
        case TypeKind::float64:
            return true;
        default:
            return false;
    }
}

const Conformance* TypeCheckedModel::equatableConformance(TypeId type) const {
    const auto protocol = equatableProtocol();
    if (!protocol.has_value()) return nullptr;
    for (const auto& conformance : conformanceList) {
        if (conformance.protocol == *protocol && conformanceSubjectMatches(conformance.type, type)) return &conformance;
    }
    return nullptr;
}

bool TypeCheckedModel::isDerivedEquatable(TypeId type) const {
    const auto* conformance = equatableConformance(type);
    return conformance != nullptr && conformance->derived;
}

std::vector<EquatableMember> TypeCheckedModel::equatableMembers(TypeId type) const {
    std::vector<EquatableMember> members;
    if (const auto structure = structureDeclaration(type)) {
        for (const auto& field : structure->fields) {
            const auto symbol = semanticModelValue->declaredSymbol(field);
            const auto fieldType = symbol.has_value() ? typeOf(*symbol) : std::optional<TypeId>();
            if (!fieldType.has_value()) continue;
            const auto* fieldSymbol = semanticModelValue->symbol(*symbol);
            members.push_back({ fieldSymbol == nullptr ? std::string() : fieldSymbol->name,
                                memberTypeOf(type, *fieldType) });
        }
        return members;
    }
    const auto* record = typeContext.type(type);
    if (record == nullptr || record->kind != TypeKind::enumeration) return members;
    const auto* symbol = semanticModelValue->symbol(record->symbol);
    if (symbol == nullptr || !symbol->declaration.has_value()) return members;
    const auto declaration = semanticModelValue->node(*symbol->declaration);
    if (declaration == nullptr || declaration->kind != syntax::Kind::enumDecl) return members;
    for (const auto& enumCase : std::static_pointer_cast<syntax::EnumDeclSyntax>(declaration)->cases) {
        const auto caseSymbol = semanticModelValue->declaredSymbol(enumCase);
        const auto* signature = caseSymbol.has_value() ? callable(*caseSymbol) : nullptr;
        if (signature == nullptr) continue;
        const auto* caseInfo = semanticModelValue->symbol(*caseSymbol);
        const auto caseName = caseInfo == nullptr ? std::string() : caseInfo->name;
        const auto parameters = signature->parameters;
        for (size_t index = 0; index < parameters.size(); ++index) {
            members.push_back({ caseName + "." + std::to_string(index),
                                memberTypeOf(type, parameters[index]) });
        }
    }
    return members;
}

std::optional<bool> TypeCheckedModel::presenceTest(const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (!id.has_value()) return std::nullopt;
    const auto found = presenceTests.find(*id);
    if (found == presenceTests.end()) return std::nullopt;
    return found->second;
}

std::optional<TypeId> TypeCheckedModel::layoutIntrinsicType(const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (!id.has_value()) return std::nullopt;
    const auto found = layoutIntrinsicTypes.find(*id);
    if (found == layoutIntrinsicTypes.end()) return std::nullopt;
    return found->second;
}

const GenericCall* TypeCheckedModel::genericCall(const syntax::NodePtr& node) const {
    const auto id = semanticModelValue->nodeId(node);
    if (!id.has_value()) return nullptr;
    const auto found = genericCalls.find(*id);
    return found == genericCalls.end() ? nullptr : &found->second;
}

// A conformance of a generic type (`extension Box<T>: P`) holds for every instance (spec 20.12.2), so
// it is recorded against the declaration's own instance and matches any instance of that type.
bool TypeCheckedModel::conformanceSubjectMatches(TypeId subject, TypeId type) const {
    if (subject == type) return true;
    const auto* left = typeContext.type(subject);
    const auto* right = typeContext.type(type);
    if (left == nullptr || right == nullptr || left->arguments.empty()) return false;
    if (left->kind != right->kind || left->symbol != right->symbol) return false;
    return left->kind == TypeKind::structure || left->kind == TypeKind::enumeration;
}

std::optional<semantic::SymbolId> TypeCheckedModel::witness(
        TypeId type, semantic::SymbolId requirement) const {
    for (const auto& conformance : conformanceList) {
        if (!conformanceSubjectMatches(conformance.type, type)) continue;
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

} // namespace joyeer::typing
