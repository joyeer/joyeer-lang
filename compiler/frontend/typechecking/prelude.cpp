#include "builder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

namespace joyeer::typing {

namespace {

// The collection structure role that owns a method role (decisions 0025-0027), or `none`.
semantic::LibraryRole collectionOwnerRole(semantic::LibraryRole method) {
    switch (method) {
        case semantic::LibraryRole::arrayElement:
        case semantic::LibraryRole::arrayCopy:
        case semantic::LibraryRole::containerEnd:
        case semantic::LibraryRole::arrayLiteral:
        case semantic::LibraryRole::stringUtf8:
            return semantic::LibraryRole::arrayType;
        case semantic::LibraryRole::dictElement:
        case semantic::LibraryRole::dictSet:
        case semantic::LibraryRole::dictGet:
        case semantic::LibraryRole::dictCopy:
        case semantic::LibraryRole::dictLiteral:
            return semantic::LibraryRole::dictType;
        default:
            return semantic::LibraryRole::none;
    }
}

// The annotation name of a collection role (decisions 0025-0027).
const char* collectionRoleName(semantic::LibraryRole role) {
    switch (role) {
        case semantic::LibraryRole::arrayType: return "array_type";
        case semantic::LibraryRole::arrayElement: return "array_element";
        case semantic::LibraryRole::arrayCopy: return "array_copy";
        case semantic::LibraryRole::containerEnd: return "container_end";
        case semantic::LibraryRole::arrayLiteral: return "array_literal";
        case semantic::LibraryRole::dictType: return "dict_type";
        case semantic::LibraryRole::dictElement: return "dict_element";
        case semantic::LibraryRole::dictSet: return "dict_set";
        case semantic::LibraryRole::dictGet: return "dict_get";
        case semantic::LibraryRole::dictCopy: return "dict_copy";
        case semantic::LibraryRole::dictLiteral: return "dict_literal";
        case semantic::LibraryRole::dictEntry: return "dict_entry";
        case semantic::LibraryRole::stringCount: return "string_count";
        case semantic::LibraryRole::stringByte: return "string_byte";
        case semantic::LibraryRole::stringConcat: return "string_concat";
        case semantic::LibraryRole::stringEqual: return "string_equal";
        case semantic::LibraryRole::stringOrder: return "string_order";
        case semantic::LibraryRole::stringUtf8: return "string_utf8";
        case semantic::LibraryRole::equatable: return "equatable";
        case semantic::LibraryRole::arrayEqual: return "array_equal";
        default: return "unknown";
    }
}

// The method roles a collection structure role requires exactly once.
std::vector<semantic::LibraryRole> requiredCollectionMethodRoles(semantic::LibraryRole collection) {
    switch (collection) {
        case semantic::LibraryRole::arrayType:
            return { semantic::LibraryRole::arrayElement, semantic::LibraryRole::arrayCopy,
                     semantic::LibraryRole::containerEnd, semantic::LibraryRole::arrayLiteral };
        case semantic::LibraryRole::dictType:
            return { semantic::LibraryRole::dictElement, semantic::LibraryRole::dictSet,
                     semantic::LibraryRole::dictGet, semantic::LibraryRole::dictCopy,
                     semantic::LibraryRole::dictLiteral };
        default:
            return {};
    }
}

} // namespace

// The closed shape of a dictionary method role (decision 0027, Decision 3). Every role but `dict_get` is private:
// only the compiler calls it.
bool TypeCheckingBuilder::dictMethodRoleMatches(const semantic::Symbol& symbol, const TypedCallableSignature& signature,
                                         const semantic::Symbol& owner, std::string& failure) {
    const auto* ownerParameters = model->genericParameters(owner.id);
    if (ownerParameters == nullptr || ownerParameters->size() != 2) {
        failure = std::string("the ") + collectionRoleName(symbol.libraryRole) +
                " role requires a method of a dict_type struct with two type parameters";
        return false;
    }
    const auto key = model->typeContext.typeParameterType((*ownerParameters)[0]);
    const auto value = model->typeContext.typeParameterType((*ownerParameters)[1]);
    const auto receiver = symbol.callable->receiver->access;
    const auto& written = symbol.callable->parameters;
    const auto* result = model->typeContext.type(signature.result);
    const bool returnsSelf = result != nullptr && result->kind == TypeKind::structure &&
            result->symbol == owner.id && result->arguments == std::vector<TypeId> { key, value };
    const bool isPrivate = symbol.visibility == syntax::Visibility::private_;
    const auto labelled = [&](std::initializer_list<const char*> labels) {
        if (written.size() != labels.size()) return false;
        size_t index = 0;
        for (const auto* label : labels) {
            if (written[index++].label != std::optional<std::string>(label)) return false;
        }
        return true;
    };
    const auto accesses = [&](syntax::AccessEffect access) {
        return std::all_of(written.begin(), written.end(),
                           [access](const auto& parameter) { return parameter.access == access; });
    };
    switch (symbol.libraryRole) {
        case semantic::LibraryRole::dictElement:
            // The one addressor: read, borrow, consume and mutate a stored value; traps on a missing key.
            if (isPrivate && receiver == syntax::AccessEffect::borrowing && labelled({ "key" }) &&
                accesses(syntax::AccessEffect::borrowing) &&
                signature.parameters == std::vector<TypeId> { key } &&
                signature.result == model->typeContext.intType()) {
                return true;
            }
            failure = "the dict_element role requires 'private borrowing func name(key: K): Int'";
            return false;
        case semantic::LibraryRole::dictSet:
            // Insert-or-replace: the runtime adopts the owned key and value.
            if (isPrivate && receiver == syntax::AccessEffect::inout && labelled({ "key", "value" }) &&
                accesses(syntax::AccessEffect::consuming) &&
                signature.parameters == std::vector<TypeId> { key, value } &&
                signature.result == model->typeContext.voidType()) {
                return true;
            }
            failure = "the dict_set role requires 'private mutating func name(key: consuming K, value: "
                    "consuming V): Void'";
            return false;
        case semantic::LibraryRole::dictGet: {
            // The one public method role: user code calls it as `d.get(key: k)`.
            const auto optional = model->semanticModelValue->roleSymbol(semantic::LibraryRole::optional);
            if (symbol.name == "get" && symbol.visibility == syntax::Visibility::public_ &&
                receiver == syntax::AccessEffect::borrowing && labelled({ "key" }) &&
                accesses(syntax::AccessEffect::borrowing) &&
                signature.parameters == std::vector<TypeId> { key } && result != nullptr &&
                result->kind == TypeKind::enumeration && optional.has_value() && result->symbol == *optional &&
                result->arguments == std::vector<TypeId> { value }) {
                return true;
            }
            failure = "the dict_get role requires 'public borrowing func get(key: K): Optional<V>'";
            return false;
        }
        case semantic::LibraryRole::dictCopy:
            if (isPrivate && receiver == syntax::AccessEffect::borrowing && written.empty() && returnsSelf) {
                return true;
            }
            failure = "the dict_copy role requires 'private borrowing func name(): Self' returning the declaring "
                    "type";
            return false;
        case semantic::LibraryRole::dictLiteral:
            // Called on an inert zero value with the packed entries.
            if (isPrivate && receiver == syntax::AccessEffect::consuming && labelled({ "address", "count" }) &&
                accesses(syntax::AccessEffect::borrowing) &&
                signature.parameters == std::vector<TypeId> { model->typeContext.intType(),
                                                              model->typeContext.intType() } &&
                returnsSelf) {
                return true;
            }
            failure = "the dict_literal role requires 'private consuming func name(address: Int, count: Int): "
                    "Self' returning the declaring type";
            return false;
        default:
            failure = "not a dictionary method role";
            return false;
    }
}

// The `equatable` role (decision 0031): exactly one role-prelude protocol, whose one requirement is
// `func equals(other: Self): Bool` with a borrowing receiver and argument.
void TypeCheckingBuilder::validateEquatableRole() {
    std::vector<const semantic::Symbol*> protocols;
    for (const auto& symbol : model->semanticModelValue->symbols()) {
        if (symbol.kind == semantic::SymbolKind::protocol && symbol.libraryRole == semantic::LibraryRole::equatable &&
            model->semanticModelValue->isPreludeSource(symbol.span)) {
            protocols.push_back(&symbol);
        }
    }
    for (size_t index = 1; index < protocols.size(); ++index) {
        report(TypeCheckingDiagnosticId::invalidPrelude, protocols[index]->span,
               "the compiler prelude declares a second protocol for the equatable role");
    }
    if (protocols.empty()) return;
    const auto protocol = protocols.front()->id;
    const auto requirements = requirementsOf(protocol);
    // `Self` in a requirement is the type parameter whose symbol is the protocol (spec 20.2).
    const auto self = model->typeContext.typeParameterType(protocol);
    const auto shape = requirements.size() == 1 && symbolName(requirements.front()) == "equals"
            ? methodShape(requirements.front(), {}) : std::nullopt;
    const bool shaped = shape.has_value() && shape->receiver == syntax::AccessEffect::borrowing &&
            shape->parameters == std::vector<TypeId> { self } &&
            shape->labels == std::vector<std::optional<std::string>> { std::string("other") } &&
            shape->effects == std::vector<syntax::AccessEffect> { syntax::AccessEffect::borrowing } &&
            shape->result == model->typeContext.boolType();
    if (!shaped) {
        report(TypeCheckingDiagnosticId::invalidPrelude, protocols.front()->span,
               "the equatable role requires exactly the requirement 'func equals(other: Self): Bool'");
    }
}

void TypeCheckingBuilder::validatePreludeContracts() {
    validateEquatableRole();
    const auto isRoleType = [&](TypeId id, semantic::LibraryRole role) {
        const auto* type = model->typeContext.type(id);
        const auto* symbol = type == nullptr ? nullptr : model->semanticModelValue->symbol(type->symbol);
        return type != nullptr && type->kind == TypeKind::structure &&
                symbol != nullptr && symbol->libraryRole == role;
    };
    std::unordered_map<semantic::LibraryRole, int> roleDeclarations;
    for (const auto& symbol : model->semanticModelValue->symbols()) {
        if (!symbol.declaration.has_value() || !model->semanticModelValue->isPreludeSource(symbol.span) ||
            (symbol.kind != semantic::SymbolKind::function && symbol.kind != semantic::SymbolKind::method &&
             symbol.kind != semantic::SymbolKind::structure && symbol.kind != semantic::SymbolKind::enumeration)) {
            continue;
        }
        const auto node = model->semanticModelValue->node(*symbol.declaration);
        const auto declaration = std::static_pointer_cast<syntax::DeclSyntax>(node);
        const auto* parameters = model->genericParameters(symbol.id);
        const auto* signature = model->callable(symbol.id);
        std::unordered_set<std::string> annotations;
        bool valid = true;
        for (const auto& attribute : declaration->attributes) {
            const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
            if (name != "compiler_transfer" && name != "compiler_role") continue;
            if (!annotations.insert(name).second) {
                report(TypeCheckingDiagnosticId::invalidPrelude, attribute.span,
                       "a compiler prelude declaration may carry each metadata annotation only once");
                valid = false;
            } else if (name == "compiler_transfer" &&
                       (!attribute.arguments.empty() || parameters == nullptr || parameters->empty())) {
                report(TypeCheckingDiagnosticId::invalidPrelude, attribute.span,
                       "compiler transfer metadata requires an argument-free annotation on a generic declaration");
                valid = false;
            } else if (name == "compiler_role" &&
                       (attribute.arguments.size() != 1 || symbol.libraryRole == semantic::LibraryRole::none)) {
                report(TypeCheckingDiagnosticId::invalidPrelude, attribute.span,
                       "compiler role metadata requires one recognized role");
                valid = false;
            }
        }
        if (!valid || symbol.libraryRole == semantic::LibraryRole::none) continue;
        bool matches = false;
        std::string roleFailure;
        switch (symbol.libraryRole) {
            case semantic::LibraryRole::taskHandle:
            case semantic::LibraryRole::channelSender:
            case semantic::LibraryRole::channelReceiver:
                if (node->kind == syntax::Kind::structDecl && parameters != nullptr && parameters->size() == 1) {
                    const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                    matches = symbol.libraryRole == semantic::LibraryRole::taskHandle
                            ? structure->isLinear && structure->deinitializer == nullptr &&
                                      symbol.acceptsLinearArguments
                            : !structure->isLinear && structure->deinitializer != nullptr &&
                                      !symbol.acceptsLinearArguments;
                }
                break;
            case semantic::LibraryRole::storedConsumer:
                matches = symbol.kind == semantic::SymbolKind::method && symbol.callable.has_value() &&
                        symbol.callable->receiver.has_value() &&
                        symbol.callable->receiver->access == syntax::AccessEffect::consuming &&
                        symbol.callable->parameters.empty();
                break;
            case semantic::LibraryRole::taskStart:
                if (symbol.kind == semantic::SymbolKind::function && parameters != nullptr &&
                    parameters->size() == 2 && symbol.acceptsLinearArguments && signature != nullptr &&
                    signature->parameters.size() == 2 && symbol.callable.has_value()) {
                    const auto* function = model->typeContext.type(signature->parameters[0]);
                    const auto* result = model->typeContext.type(signature->result);
                    const auto input = model->typeContext.typeParameterType((*parameters)[0]);
                    const auto output = model->typeContext.typeParameterType((*parameters)[1]);
                    matches = function != nullptr && function->kind == TypeKind::function &&
                            function->arguments == std::vector<TypeId> {input, output} &&
                            function->effects == std::vector<syntax::AccessEffect> {syntax::AccessEffect::consuming} &&
                            signature->parameters[1] == input &&
                            symbol.callable->parameters[0].access == syntax::AccessEffect::borrowing &&
                            symbol.callable->parameters[1].access == syntax::AccessEffect::consuming &&
                            isRoleType(signature->result, semantic::LibraryRole::taskHandle) &&
                            result->arguments == std::vector<TypeId> {output};
                }
                break;
            case semantic::LibraryRole::channelCreate:
                if (symbol.kind == semantic::SymbolKind::function && parameters != nullptr &&
                    parameters->size() == 1 && !symbol.acceptsLinearArguments && signature != nullptr &&
                    signature->parameters.size() == 3 && symbol.callable.has_value()) {
                    const auto* sender = model->typeContext.type(signature->parameters[1]);
                    const auto* receiver = model->typeContext.type(signature->parameters[2]);
                    const auto element = model->typeContext.typeParameterType(parameters->front());
                    matches = signature->parameters[0] == model->typeContext.intType() &&
                            signature->result == model->typeContext.voidType() &&
                            isRoleType(signature->parameters[1], semantic::LibraryRole::channelSender) &&
                            isRoleType(signature->parameters[2], semantic::LibraryRole::channelReceiver) &&
                            sender->arguments == std::vector<TypeId> {element} &&
                            receiver->arguments == std::vector<TypeId> {element} &&
                            symbol.callable->parameters[0].access == syntax::AccessEffect::borrowing &&
                            symbol.callable->parameters[1].access == syntax::AccessEffect::initializing &&
                            symbol.callable->parameters[2].access == syntax::AccessEffect::initializing;
                }
                break;
            case semantic::LibraryRole::optional:
            case semantic::LibraryRole::result: {
                // Positional shape (decision 0024): the binding belongs to the declaration.
                const bool isOptional = symbol.libraryRole == semantic::LibraryRole::optional;
                const std::string roleName = isOptional ? "optional" : "result";
                if (node->kind != syntax::Kind::enumDecl) {
                    roleFailure = "the " + roleName + " role requires an enum";
                    break;
                }
                if (parameters == nullptr || parameters->size() != (isOptional ? 1u : 2u)) {
                    roleFailure = "the " + roleName + " role requires " + (isOptional ? "one" : "two") +
                            " type parameter" + (isOptional ? "" : "s");
                    break;
                }
                if (symbol.acceptsLinearArguments || std::any_of(parameters->begin(), parameters->end(),
                        [&](const auto parameter) { return !model->constraints(parameter).empty(); })) {
                    roleFailure = "the " + roleName + " role's type parameters take no constraints and no "
                            "linear marker";
                    break;
                }
                if (symbol.visibility != syntax::Visibility::public_) {
                    roleFailure = "the " + roleName + " role requires a public enum";
                    break;
                }
                const auto enumeration = std::static_pointer_cast<syntax::EnumDeclSyntax>(node);
                if (enumeration->cases.size() != 2 || !enumeration->methods.empty()) {
                    roleFailure = "the " + roleName + " role requires exactly two cases and no methods";
                    break;
                }
                const auto parameterType = [&](size_t index) {
                    return model->typeContext.typeParameterType((*parameters)[index]);
                };
                std::vector<std::vector<TypeId>> payloads;
                bool shaped = true;
                for (const auto& enumCase : enumeration->cases) {
                    const auto caseSymbol = model->semanticModelValue->declaredSymbol(enumCase);
                    const auto* caseSignature = caseSymbol.has_value() ? model->callable(*caseSymbol) : nullptr;
                    if (caseSignature == nullptr) { shaped = false; break; }
                    for (const auto& associated : enumCase->associatedTypes) {
                        if (associated->label != nullptr) shaped = false;
                    }
                    payloads.push_back(caseSignature->parameters);
                }
                if (!shaped) {
                    roleFailure = "the " + roleName + " role's cases take unlabelled payloads";
                    break;
                }
                if (isOptional) {
                    const auto present = payloads[0].empty() ? 1u : 0u;
                    matches = payloads[present] == std::vector<TypeId> { parameterType(0) } &&
                            payloads[1 - present].empty();
                    if (!matches) roleFailure = "the optional role requires one case carrying exactly the "
                            "type parameter and one case without a payload";
                } else {
                    // Lowering binds success to the first case and failure to the second.
                    matches = payloads[0] == std::vector<TypeId> { parameterType(0) } &&
                            payloads[1] == std::vector<TypeId> { parameterType(1) };
                    if (!matches) roleFailure = "the result role requires its first case to carry exactly "
                            "the first type parameter and its second case the second";
                }
                // Each of these two roles is declared exactly once across both prelude sources.
                if (matches && ++roleDeclarations[symbol.libraryRole] > 1) {
                    report(TypeCheckingDiagnosticId::invalidPrelude, symbol.span,
                           "the compiler prelude declares a second enum for the same role");
                }
                break;
            }
            case semantic::LibraryRole::arrayType:
            case semantic::LibraryRole::dictType: {
                // The library array (decisions 0025, 0026) and dictionary (decision 0027): three private Int
                // words, the owned type parameters (the element, or the key and the value) and the method roles
                // the compiler calls.
                const bool isDict = symbol.libraryRole == semantic::LibraryRole::dictType;
                const std::string roleName = collectionRoleName(symbol.libraryRole);
                const size_t owned = isDict ? 2 : 1;
                if (node->kind != syntax::Kind::structDecl) {
                    roleFailure = "the " + roleName + " role requires a struct";
                    break;
                }
                if (++roleDeclarations[symbol.libraryRole] > 1) {
                    report(TypeCheckingDiagnosticId::invalidPrelude, symbol.span,
                           "the compiler prelude declares a second struct for the " + roleName + " role");
                }
                if (parameters == nullptr || parameters->size() != owned ||
                    std::any_of(parameters->begin(), parameters->end(),
                                [&](const auto parameter) { return !model->constraints(parameter).empty(); })) {
                    roleFailure = "the " + roleName + " role requires exactly " + (isDict ? "two" : "one") +
                            " unconstrained type parameter" + (isDict ? "s" : "");
                    break;
                }
                if (symbol.visibility != syntax::Visibility::public_) {
                    roleFailure = "the " + roleName + " role requires a public struct";
                    break;
                }
                if (isDict && symbol.acceptsLinearArguments) {
                    // The dictionary owns no linear key or value (decision 0027): no transfer marker.
                    roleFailure = "the dict_type role's type parameters take no linear marker";
                    break;
                }
                const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                if (structure->deinitializer == nullptr) {
                    roleFailure = "the " + roleName + " role requires a deinit that destroys the " +
                            (isDict ? "entries" : "elements") + " and frees the storage";
                    break;
                }
                static const char* const wordNames[3] = { "address", "count", "capacity" };
                bool shaped = structure->fields.size() == 3 && !structure->isLinear;
                for (size_t index = 0; shaped && index < 3; ++index) {
                    const auto& field = structure->fields[index];
                    const auto fieldSymbol = model->semanticModelValue->declaredSymbol(field);
                    const auto fieldType = fieldSymbol.has_value() ? model->typeOf(*fieldSymbol)
                                                                   : std::optional<TypeId>();
                    shaped = field->name != nullptr && field->name->rawValue == wordNames[index] &&
                            field->visibility == syntax::Visibility::private_ &&
                            fieldType == model->typeContext.intType();
                }
                if (!shaped) {
                    roleFailure = "the " + roleName + " role requires the private Int fields address, count "
                            "and capacity, in that order";
                    break;
                }
                // Every method role the collection requires, each exactly once (decision 0026's four for an
                // array, decision 0027's five for a dictionary); the record keeps them for lowering and
                // instance discovery.
                std::unordered_map<semantic::LibraryRole, int> methodRoles;
                TypeCheckedModel::CollectionRole record;
                record.kind = symbol.libraryRole;
                record.ownedParameters = *parameters;
                for (const auto& member : model->semanticModelValue->symbols()) {
                    if (member.kind == semantic::SymbolKind::method && member.containingSymbol == symbol.id &&
                        collectionOwnerRole(member.libraryRole) == symbol.libraryRole) {
                        ++methodRoles[member.libraryRole];
                        record.methods[member.libraryRole] = member.id;
                    }
                }
                for (const auto role : requiredCollectionMethodRoles(symbol.libraryRole)) {
                    if (methodRoles[role] != 1) {
                        roleFailure = "the " + roleName + " role requires exactly one method with the role " +
                                collectionRoleName(role);
                        break;
                    }
                }
                // `s.utf8()` needs the array's `string_utf8` method (decision 0028 stage 3); a prelude without
                // it only cannot lower that call.
                if (roleFailure.empty() && methodRoles[semantic::LibraryRole::stringUtf8] > 1) {
                    roleFailure = "the " + roleName + " role allows at most one method with the role string_utf8";
                }
                if (roleFailure.empty() && isDict) {
                    // The entry struct a literal packs; its own role checks its shape.
                    const auto entry = model->semanticModelValue->roleSymbol(semantic::LibraryRole::dictEntry);
                    if (!entry.has_value()) {
                        roleFailure = "the dict_type role requires a private dict_entry struct";
                    } else {
                        record.entry = *entry;
                    }
                }
                matches = roleFailure.empty();
                if (matches) model->collectionRoles[symbol.id] = std::move(record);
                break;
            }
            case semantic::LibraryRole::dictEntry: {
                // The entry a dictionary literal packs (decision 0027): its ordinary struct layout is the
                // runtime's entry layout, so the shape is closed, and it is never a value.
                if (node->kind != syntax::Kind::structDecl) {
                    roleFailure = "the dict_entry role requires a struct";
                    break;
                }
                if (++roleDeclarations[symbol.libraryRole] > 1) {
                    report(TypeCheckingDiagnosticId::invalidPrelude, symbol.span,
                           "the compiler prelude declares a second struct for the dict_entry role");
                }
                if (symbol.visibility != syntax::Visibility::private_) {
                    roleFailure = "the dict_entry role requires a private struct";
                    break;
                }
                const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                bool shaped = parameters != nullptr && parameters->size() == 2 &&
                        !symbol.acceptsLinearArguments &&
                        std::none_of(parameters->begin(), parameters->end(), [&](const auto parameter) {
                            return !model->constraints(parameter).empty();
                        }) &&
                        structure->fields.size() == 2 && structure->deinitializer == nullptr &&
                        structure->methods.empty() && !structure->isLinear;
                static const char* const entryFieldNames[2] = { "key", "value" };
                std::array<semantic::SymbolId, 2> entryFields {};
                for (size_t index = 0; shaped && index < 2; ++index) {
                    const auto& field = structure->fields[index];
                    const auto fieldSymbol = model->semanticModelValue->declaredSymbol(field);
                    const auto fieldType = fieldSymbol.has_value() ? model->typeOf(*fieldSymbol)
                                                                   : std::optional<TypeId>();
                    shaped = field->name != nullptr && field->name->rawValue == entryFieldNames[index] &&
                            fieldType == model->typeContext.typeParameterType((*parameters)[index]);
                    if (shaped) entryFields[index] = *fieldSymbol;
                }
                matches = shaped;
                // Lowering packs a literal through these two fields (decision 0027).
                if (matches) model->dictEntryFieldSymbols = entryFields;
                if (!matches) {
                    roleFailure = "the dict_entry role requires exactly the fields 'key: K' then 'value: V' of "
                            "its two unconstrained, unmarked type parameters, no methods and no deinit";
                }
                break;
            }
            case semantic::LibraryRole::arrayElement:
            case semantic::LibraryRole::arrayCopy:
            case semantic::LibraryRole::arrayLiteral:
            case semantic::LibraryRole::stringUtf8:
            case semantic::LibraryRole::containerEnd:
            case semantic::LibraryRole::dictElement:
            case semantic::LibraryRole::dictSet:
            case semantic::LibraryRole::dictGet:
            case semantic::LibraryRole::dictCopy:
            case semantic::LibraryRole::dictLiteral: {
                const std::string roleName = collectionRoleName(symbol.libraryRole);
                const auto* owner = symbol.containingSymbol.has_value() ? symbolOf(*symbol.containingSymbol)
                                                                        : nullptr;
                // Fails closed: a method role missing from `collectionOwnerRole` has no owner role and is rejected.
                const auto ownerRole = collectionOwnerRole(symbol.libraryRole);
                if (symbol.kind != semantic::SymbolKind::method || owner == nullptr ||
                    ownerRole == semantic::LibraryRole::none || owner->libraryRole != ownerRole ||
                    !symbol.callable.has_value() ||
                    !symbol.callable->receiver.has_value() || signature == nullptr) {
                    roleFailure = "the " + roleName + " role requires a method of " +
                            (ownerRole == semantic::LibraryRole::dictType ? "a dict_type" : "an array_type") +
                            " struct";
                    break;
                }
                const auto receiver = symbol.callable->receiver->access;
                const auto& written = symbol.callable->parameters;
                const auto* ownerParameters = model->genericParameters(owner->id);
                const auto* result = model->typeContext.type(signature->result);
                if (ownerRole == semantic::LibraryRole::dictType) {
                    matches = dictMethodRoleMatches(symbol, *signature, *owner, roleFailure);
                    break;
                }
                if (symbol.libraryRole == semantic::LibraryRole::arrayElement) {
                    matches = receiver == syntax::AccessEffect::borrowing && written.size() == 1 &&
                            written[0].label == std::optional<std::string>("index") &&
                            written[0].access == syntax::AccessEffect::borrowing &&
                            signature->parameters == std::vector<TypeId> { model->typeContext.intType() } &&
                            signature->result == model->typeContext.intType();
                    if (!matches) roleFailure = "the array_element role requires 'borrowing func name(index: Int): Int'";
                } else if (symbol.libraryRole == semantic::LibraryRole::arrayCopy) {
                    matches = receiver == syntax::AccessEffect::borrowing && written.empty() &&
                            result != nullptr && result->kind == TypeKind::structure &&
                            result->symbol == owner->id && ownerParameters != nullptr &&
                            ownerParameters->size() == 1 &&
                            result->arguments == std::vector<TypeId> {
                                    model->typeContext.typeParameterType(ownerParameters->front()) };
                    if (!matches) roleFailure = "the array_copy role requires 'borrowing func name(): Self' returning "
                            "the declaring type";
                } else if (symbol.libraryRole == semantic::LibraryRole::arrayLiteral) {
                    // `consuming func name(address: Int, count: Int): Self`, called on an inert zero value.
                    matches = symbol.visibility == syntax::Visibility::private_ &&
                            receiver == syntax::AccessEffect::consuming && written.size() == 2 &&
                            written[0].label == std::optional<std::string>("address") &&
                            written[1].label == std::optional<std::string>("count") &&
                            written[0].access == syntax::AccessEffect::borrowing &&
                            written[1].access == syntax::AccessEffect::borrowing &&
                            signature->parameters == std::vector<TypeId> { model->typeContext.intType(),
                                                                           model->typeContext.intType() } &&
                            result != nullptr && result->kind == TypeKind::structure &&
                            result->symbol == owner->id && ownerParameters != nullptr &&
                            ownerParameters->size() == 1 &&
                            result->arguments == std::vector<TypeId> {
                                    model->typeContext.typeParameterType(ownerParameters->front()) };
                    if (!matches) roleFailure = "the array_literal role requires 'private consuming func name(address: Int, "
                            "count: Int): Self' returning the declaring type";
                } else if (symbol.libraryRole == semantic::LibraryRole::stringUtf8) {
                    // `private consuming func name(text: String): Self`, called on an inert zero `[UInt8]`
                    // (decision 0028 stage 3).
                    matches = symbol.visibility == syntax::Visibility::private_ &&
                            receiver == syntax::AccessEffect::consuming && written.size() == 1 &&
                            written[0].label == std::optional<std::string>("text") &&
                            written[0].access == syntax::AccessEffect::borrowing &&
                            signature->parameters == std::vector<TypeId> { model->typeContext.stringType() } &&
                            result != nullptr && result->kind == TypeKind::structure &&
                            result->symbol == owner->id && ownerParameters != nullptr &&
                            ownerParameters->size() == 1 &&
                            result->arguments == std::vector<TypeId> {
                                    model->typeContext.typeParameterType(ownerParameters->front()) };
                    if (!matches) roleFailure = "the string_utf8 role requires 'private consuming func name(text: "
                            "String): Self' returning the declaring type";
                } else {
                    matches = receiver == syntax::AccessEffect::consuming && written.empty() &&
                            signature->result == model->typeContext.voidType();
                    if (!matches) roleFailure = "the container_end role requires 'consuming func name()' returning Void";
                }
                break;
            }
            case semantic::LibraryRole::stringCount:
            case semantic::LibraryRole::stringByte:
            case semantic::LibraryRole::stringConcat:
            case semantic::LibraryRole::stringEqual:
            case semantic::LibraryRole::stringOrder: {
                // A private, non-generic function of borrowed operands whose signature is closed by the role;
                // lowering passes the operands positionally (decision 0028 stage 3).
                const auto role = symbol.libraryRole;
                const std::string roleName = collectionRoleName(role);
                const auto string = model->typeContext.stringType();
                const auto integer = model->typeContext.intType();
                std::vector<TypeId> operands { string, string };
                std::vector<std::string> labels { "left", "right" };
                TypeId result = integer;
                std::string shape;
                switch (role) {
                    case semantic::LibraryRole::stringCount:
                        operands = { string };
                        labels = { "text" };
                        result = integer;
                        shape = "(text: String): Int";
                        break;
                    case semantic::LibraryRole::stringByte:
                        operands = { string, integer };
                        labels = { "text", "index" };
                        result = model->typeContext.uint8Type();
                        shape = "(text: String, index: Int): UInt8";
                        break;
                    case semantic::LibraryRole::stringConcat:
                        result = string;
                        shape = "(left: String, right: String): String";
                        break;
                    case semantic::LibraryRole::stringEqual:
                        result = model->typeContext.boolType();
                        shape = "(left: String, right: String): Bool";
                        break;
                    default:
                        shape = "(left: String, right: String): Int";
                        break;
                }
                // Only these scalar and String types: a non-generic prelude function's signature is checked in
                // every module, so it must not intern a library instance such as `[UInt8]`, whose methods would
                // then be instantiated everywhere.
                const bool resultMatches = signature != nullptr && signature->result == result;
                // Lowering calls and lowers the body, so the role is never a C binding.
                const bool hasBody = node->kind == syntax::Kind::functionDecl &&
                        std::static_pointer_cast<syntax::FunctionDeclSyntax>(node)->body != nullptr &&
                        model->nativeBinding(symbol.id) == nullptr;
                matches = symbol.kind == semantic::SymbolKind::function && hasBody &&
                        (parameters == nullptr || parameters->empty()) &&
                        symbol.visibility == syntax::Visibility::private_ && symbol.callable.has_value() &&
                        !symbol.acceptsLinearArguments && signature != nullptr &&
                        signature->parameters == operands && resultMatches &&
                        symbol.callable->parameters.size() == labels.size();
                for (size_t index = 0; matches && index < labels.size(); ++index) {
                    const auto& parameter = symbol.callable->parameters[index];
                    matches = parameter.access == syntax::AccessEffect::borrowing &&
                            parameter.label == std::optional<std::string>(labels[index]);
                }
                if (!matches) {
                    roleFailure = "the " + roleName + " role requires 'private func name" + shape +
                            "' with borrowed parameters and a body";
                    break;
                }
                if (!model->stringRoles.emplace(role, symbol.id).second) {
                    report(TypeCheckingDiagnosticId::invalidPrelude, symbol.span,
                           "the compiler prelude declares a second function for the " + roleName + " role");
                }
                break;
            }
            case semantic::LibraryRole::arrayEqual: {
                // `private func name<T: Equatable>(left: [T], right: [T]): Bool` with a body (decision 0031).
                const auto* parameters = model->genericParameters(symbol.id);
                const auto* signature = model->callable(symbol.id);
                const auto function = symbol.kind == semantic::SymbolKind::function && node != nullptr &&
                        node->kind == syntax::Kind::functionDecl
                        ? std::static_pointer_cast<syntax::FunctionDeclSyntax>(node) : nullptr;
                bool shaped = function != nullptr && function->body != nullptr && parameters != nullptr &&
                        parameters->size() == 1 && symbol.callable.has_value() && signature != nullptr &&
                        symbol.callable->parameters.size() == 2 && signature->parameters.size() == 2 &&
                        symbol.visibility == syntax::Visibility::private_ &&
                        signature->result == model->typeContext.boolType();
                if (shaped) {
                    const auto equatable = model->equatableProtocol();
                    const auto& constraints = model->constraints((*parameters)[0]);
                    const auto element = model->typeContext.typeParameterType((*parameters)[0]);
                    shaped = equatable.has_value() &&
                            std::find(constraints.begin(), constraints.end(), *equatable) != constraints.end();
                    for (size_t index = 0; shaped && index < 2; ++index) {
                        const auto& parameter = symbol.callable->parameters[index];
                        const auto* array = model->typeContext.type(signature->parameters[index]);
                        shaped = parameter.access == syntax::AccessEffect::borrowing &&
                                parameter.label == std::optional<std::string>(index == 0 ? "left" : "right") &&
                                model->isArrayRoleType(signature->parameters[index]) && array != nullptr &&
                                array->arguments.size() == 1 && array->arguments[0] == element;
                    }
                }
                if (!shaped) {
                    roleFailure = "the array_equal role requires 'private func name<T: Equatable>(left: [T], "
                                  "right: [T]): Bool'";
                    break;
                }
                if (model->arrayEqualSymbol.has_value()) {
                    report(TypeCheckingDiagnosticId::invalidPrelude, symbol.span,
                           "the compiler prelude declares a second function for the array_equal role");
                }
                model->arrayEqualSymbol = symbol.id;
                matches = true;
                break;
            }
            case semantic::LibraryRole::none:
                break;
        }
        if (!matches) {
            report(TypeCheckingDiagnosticId::invalidPrelude, symbol.span,
                   roleFailure.empty()
                           ? "compiler prelude role does not match the declaration's kind, ownership, or signature"
                           : roleFailure);
        }
    }
}

void TypeCheckingBuilder::resolveBuiltinSignatures() {
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
        if (symbol.kind == semantic::SymbolKind::builtinEnumCase && symbol.containingSymbol.has_value()) {
            const auto type = model->typeContext.typeForSymbol(*symbol.containingSymbol);
            if (!type.has_value()) continue;
            auto signature = enumCaseSignature(symbol.id, *type);
            if (!signature.has_value()) continue;
            model->symbolTypes[symbol.id] = *type;
            model->callables[symbol.id] = std::move(*signature);
            continue;
        }
        if (symbol.kind != semantic::SymbolKind::builtinMember) continue;
        if (symbol.name == "utf8" && symbol.callable.has_value()) {
            model->symbolTypes[symbol.id] = model->typeContext.anyType();
            model->callables[symbol.id] = TypedCallableSignature {
                semantic::CallableKind::function,
                true,
                {},
                model->typeContext.anyType(),
            };
            continue;
        }
        if (!symbol.declaredType.has_value()) continue;
        const auto type = model->typeContext.typeForSymbol(*symbol.declaredType);
        if (type.has_value()) model->symbolTypes[symbol.id] = *type;
    }
}

} // namespace joyeer::typing
