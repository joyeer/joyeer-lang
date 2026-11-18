// Module-wide checks of the type table and the definitions built on it: type kinds and
// function-type effects, which types are copyable and concrete, struct and enum layouts and
// their language roles (decisions 0015, 0024-0027), and the signatures of the functions those
// roles name (deinit, copier, element addressor, literal factory). See internal.h for the order.

#include "internal.h"

#include <algorithm>
#include <map>
#include <utility>

namespace joyeer::ir::verifier {

// Fills `types`, reporting duplicate IDs.
void ModuleVerifier::indexTypes() {
    for (const auto& type : module.types) {
        if (type.declarationSpan.has_value()) {
            verifyDebugLocation(
                    DebugLocation { *type.declarationSpan },
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "type '" + type.name + "'");
        }
        if (!types.emplace(type.id, &type).second) {
            report(
                    VerificationErrorId::duplicateId,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "duplicate type id " + std::to_string(type.id));
        }
    }
}

// Each type has a live kind, known type arguments, and (for a function type) valid parameter effects.
void ModuleVerifier::verifyTypeNames() {
    for (const auto& type : module.types) {
        const auto kindSlot = static_cast<uint64_t>(type.kind);
        if (kindSlot == 10 || kindSlot == 11 || kindSlot > static_cast<uint64_t>(typing::TypeKind::typeParameter)) {
            report(VerificationErrorId::typeMismatch, std::nullopt, std::nullopt, std::nullopt,
                   "type '" + type.name + "' has an unknown or removed type kind");
        }
        if (type.kind == typing::TypeKind::typeParameter) {
            report(
                    VerificationErrorId::typeMismatch,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "type parameter '" + type.name + "' must be substituted before IR lowering");
        }
        for (const auto argument : type.arguments) {
            if (!types.contains(argument)) {
                report(
                        VerificationErrorId::invalidReference,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "type '" + type.name + "' references unknown type argument " +
                                std::to_string(argument));
            }
        }
        // A function type has one effect per parameter and its result last; the wire reader does not
        // range-check enumerations, so each effect is checked here.
        const bool isFunction = type.kind == typing::TypeKind::function;
        bool effectsValid = isFunction ? type.effects.size() + 1 == type.arguments.size() : type.effects.empty();
        for (const auto effect : type.effects) {
            effectsValid &= static_cast<uint64_t>(effect) <= static_cast<uint64_t>(syntax::AccessEffect::initializing);
        }
        // `Never` is not a parameter type: its LLVM type is void (spec 2.3.1).
        for (size_t index = 0; isFunction && effectsValid && index + 1 < type.arguments.size(); ++index) {
            const auto found = types.find(type.arguments[index]);
            effectsValid &= found == types.end() || found->second->kind != typing::TypeKind::never;
        }
        if (!effectsValid) {
            report(
                    VerificationErrorId::typeMismatch,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "type '" + type.name + "' has invalid parameter effects");
        }
    }
}

// Fills `noncopyableTypes`: a struct with a deinit and no copier, and every type that holds one
// (a field, a payload, or a collection's owned type argument).
void ModuleVerifier::computeNoncopyableTypes() {
    std::unordered_map<TypeId, std::vector<TypeId>> copyDependents;
    std::vector<TypeId> pendingNoncopyable;
    for (const auto& structure : module.structures) {
        if (structure.deinitializer.has_value() &&
            !(isInertZero(structure.role) && structure.copier.has_value()) &&
            noncopyableTypes.insert(structure.type).second) {
            pendingNoncopyable.push_back(structure.type);
        }
        for (const auto& field : structure.fields) copyDependents[field.type].push_back(structure.type);
        const auto type = types.find(structure.type);
        // A collection's copy depends on every type argument it owns: the element, or the key and the value.
        if (type != types.end()) {
            const auto owned = std::min(collectionOwnedArguments(structure.role), type->second->arguments.size());
            for (size_t index = 0; index < owned; ++index) {
                copyDependents[type->second->arguments[index]].push_back(structure.type);
            }
        }
    }
    for (const auto& enumeration : module.enumerations) {
        for (const auto& element : enumeration.cases) {
            for (const auto payload : element.payloadTypes) copyDependents[payload].push_back(enumeration.type);
        }
    }
    for (size_t index = 0; index < pendingNoncopyable.size(); ++index) {
        for (const auto dependent : copyDependents[pendingNoncopyable[index]]) {
            if (noncopyableTypes.insert(dependent).second) pendingNoncopyable.push_back(dependent);
        }
    }
}

// Fills `nonConcreteTypes`: error and type-parameter types and every type built from one.
void ModuleVerifier::computeNonConcreteTypes() {
    std::unordered_map<TypeId, std::vector<TypeId>> typeDependents;
    std::vector<TypeId> pendingTypes;
    for (const auto& type : module.types) {
        for (const auto argument : type.arguments) typeDependents[argument].push_back(type.id);
        if (type.kind == typing::TypeKind::error || type.kind == typing::TypeKind::typeParameter) {
            if (nonConcreteTypes.insert(type.id).second) pendingTypes.push_back(type.id);
        }
    }
    for (size_t next = 0; next < pendingTypes.size(); ++next) {
        const auto dependents = typeDependents.find(pendingTypes[next]);
        if (dependents == typeDependents.end()) continue;
        for (const auto dependent : dependents->second) {
            if (nonConcreteTypes.insert(dependent).second) pendingTypes.push_back(dependent);
        }
    }
}

void ModuleVerifier::verifyConstants() {
    for (const auto& constant : module.constants) {
        verifyConcreteType(constant.type, std::nullopt, std::nullopt, std::nullopt, "constant");
    }
}

// Fills `structures`, and checks each struct's fields and the shape its role requires.
void ModuleVerifier::verifyStructures() {
    std::optional<semantic::SymbolId> arrayRoleSymbol;
    std::optional<semantic::SymbolId> dictRoleSymbol;
    std::optional<semantic::SymbolId> dictEntryRoleSymbol;
    // A field symbol repeats across the instances of a generic type (spec 20.12), so it is unique per structure.
    std::map<std::pair<TypeId, semantic::SymbolId>, const FieldDefinition*> fields;
    for (const auto& structure : module.structures) {
        verifyConcreteType(structure.type, std::nullopt, std::nullopt, std::nullopt, "structure");
        if (!types.contains(structure.type)) {
            report(
                    VerificationErrorId::invalidReference,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "structure '" + structure.name + "' references an unknown type");
        }
        if (!structures.emplace(structure.type, &structure).second) {
            report(
                    VerificationErrorId::duplicateId,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "duplicate structure definition for type " +
                            std::to_string(structure.type));
        }
        if (structure.role != StructureRole::none && structure.role != StructureRole::array &&
            structure.role != StructureRole::dict && structure.role != StructureRole::dictEntry) {
            report(VerificationErrorId::invalidReference, std::nullopt, std::nullopt, std::nullopt,
                   "structure '" + structure.name + "' has an unknown structure role");
        }
        if (isInertZero(structure.role)) {
            // An array or dictionary handle (decisions 0025, 0027): its owned type arguments and three Int words.
            const bool isDict = structure.role == StructureRole::dict;
            const auto found = types.find(structure.type);
            bool shaped = found != types.end() && found->second->kind == typing::TypeKind::structure &&
                    found->second->symbol == structure.symbol &&
                    found->second->arguments.size() == collectionOwnedArguments(structure.role) &&
                    structure.fields.size() == 3;
            static constexpr const char* names[] = { "address", "count", "capacity" };
            for (size_t index = 0; shaped && index < 3; ++index) {
                const auto field = types.find(structure.fields[index].type);
                shaped = structure.fields[index].name == names[index] && field != types.end() &&
                        field->second->kind == typing::TypeKind::integer;
            }
            if (!shaped) {
                report(VerificationErrorId::typeMismatch, std::nullopt, std::nullopt, std::nullopt,
                       std::string(isDict ? "dictionary" : "array") + " role structure '" + structure.name +
                               "' needs " + (isDict ? "two type arguments" : "one type argument") +
                               " and the Int fields address, count, capacity");
            }
            auto& roleSymbol = isDict ? dictRoleSymbol : arrayRoleSymbol;
            if (roleSymbol.has_value() && *roleSymbol != structure.symbol) {
                report(VerificationErrorId::invalidReference, std::nullopt, std::nullopt, std::nullopt,
                       std::string("different declarations cannot carry the ") + (isDict ? "dictionary" : "array") +
                               " structure role");
            }
            roleSymbol = structure.symbol;
        }
        if (structure.role == StructureRole::dictEntry) {
            // The entry a dictionary literal packs: exactly `key: K` then `value: V` of its two type arguments, so
            // its ordinary layout is the runtime's entry layout; never a value, so nothing to run or copy.
            const auto found = types.find(structure.type);
            bool shaped = found != types.end() && found->second->kind == typing::TypeKind::structure &&
                    found->second->symbol == structure.symbol && found->second->arguments.size() == 2 &&
                    structure.fields.size() == 2 && !structure.deinitializer.has_value() &&
                    !structure.copier.has_value();
            static constexpr const char* entryNames[] = { "key", "value" };
            for (size_t index = 0; shaped && index < 2; ++index) {
                shaped = structure.fields[index].name == entryNames[index] &&
                        structure.fields[index].type == found->second->arguments[index] &&
                        structure.fields[index].symbol != semantic::invalidSymbolId;
            }
            if (!shaped) {
                report(VerificationErrorId::typeMismatch, std::nullopt, std::nullopt, std::nullopt,
                       "dictionary entry structure '" + structure.name +
                               "' needs two type arguments, exactly the fields key and value of those types, "
                               "and no deinit or copier");
            }
            if (dictEntryRoleSymbol.has_value() && *dictEntryRoleSymbol != structure.symbol) {
                report(VerificationErrorId::invalidReference, std::nullopt, std::nullopt, std::nullopt,
                       "different declarations cannot carry the dictionary entry structure role");
            }
            dictEntryRoleSymbol = structure.symbol;
        }
        for (const auto& field : structure.fields) {
            verifyConcreteType(field.type, std::nullopt, std::nullopt, std::nullopt,
                               "field '" + field.name + "'");
            if (!types.contains(field.type)) {
                report(
                        VerificationErrorId::invalidReference,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "field '" + field.name + "' references an unknown type");
            }
            // Only the hidden live flag of a `deinit` struct, which is always last, lacks a source
            // symbol; it is not a lookup target.
            if (field.symbol == semantic::invalidSymbolId) {
                const auto isLiveFlag = field.name == liveFieldName &&
                        structure.deinitializer.has_value() && structure.role == StructureRole::none &&
                        &field == &structure.fields.back();
                if (!isLiveFlag) {
                    report(
                            VerificationErrorId::invalidReference,
                            std::nullopt,
                            std::nullopt,
                            std::nullopt,
                            "field '" + field.name + "' has no source symbol");
                }
            }
            if (field.symbol != semantic::invalidSymbolId &&
                !fields.emplace(std::pair { structure.type, field.symbol }, &field).second) {
                report(
                        VerificationErrorId::duplicateId,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "duplicate field symbol " + std::to_string(field.symbol));
            }
        }
    }
}

// Fills `enumerations`, and checks each enum's cases and payload types.
void ModuleVerifier::verifyEnumerations() {
    for (const auto& enumeration : module.enumerations) {
        verifyConcreteType(enumeration.type, std::nullopt, std::nullopt, std::nullopt, "enumeration");
        if (!types.contains(enumeration.type)) {
            report(
                    VerificationErrorId::invalidReference,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "enumeration '" + enumeration.name + "' references an unknown type");
        }
        if (!enumerations.emplace(enumeration.type, &enumeration).second) {
            report(
                    VerificationErrorId::duplicateId,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "duplicate enumeration definition for type " +
                            std::to_string(enumeration.type));
        }
        std::unordered_map<semantic::SymbolId, const EnumCaseDefinition*> cases;
        for (const auto& enumCase : enumeration.cases) {
            if (!cases.emplace(enumCase.symbol, &enumCase).second) {
                report(
                        VerificationErrorId::duplicateId,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "duplicate enum case symbol " +
                                std::to_string(enumCase.symbol) + " in type " +
                                std::to_string(enumeration.type));
            }
            for (const auto payloadType : enumCase.payloadTypes) {
                verifyConcreteType(payloadType, std::nullopt, std::nullopt, std::nullopt,
                                   "enum case '" + enumCase.name + "'");
                if (!types.contains(payloadType)) {
                    report(
                            VerificationErrorId::invalidReference,
                            std::nullopt,
                            std::nullopt,
                            std::nullopt,
                            "enum case '" + enumCase.name +
                                    "' references an unknown payload type");
                }
            }
        }
    }
}

// Role-bound enums (decision 0024): the case indices must name cases of the required shape, and
// every definition of one role is an instance of the same declaration.
void ModuleVerifier::verifyEnumerationRoles() {
    std::unordered_map<EnumerationRole, semantic::SymbolId> roleDeclarations;
    std::unordered_map<semantic::SymbolId, EnumerationRole> declarationRoles;
    for (const auto& enumeration : module.enumerations) {
        if (enumeration.role == EnumerationRole::none) continue;
        const bool isOptional = enumeration.role == EnumerationRole::optional;
        bool shaped = enumeration.role == EnumerationRole::optional || enumeration.role == EnumerationRole::result;
        if (shaped) {
            // One declaration per role, and one role per declaration.
            const auto declaration = roleDeclarations.emplace(enumeration.role, enumeration.symbol);
            const auto bound = declarationRoles.emplace(enumeration.symbol, enumeration.role);
            shaped = declaration.first->second == enumeration.symbol && bound.first->second == enumeration.role &&
                    enumeration.cases.size() == 2 &&
                    enumeration.firstRoleCase < 2 && enumeration.secondRoleCase < 2 &&
                    enumeration.firstRoleCase != enumeration.secondRoleCase;
            // Lowering binds a result's success to case 0 and its failure to case 1; the payload
            // types alone cannot tell the two apart when both type arguments are equal.
            if (shaped && !isOptional) shaped = enumeration.firstRoleCase == 0 && enumeration.secondRoleCase == 1;
        }
        if (shaped) {
            const auto& first = enumeration.cases[enumeration.firstRoleCase];
            const auto& second = enumeration.cases[enumeration.secondRoleCase];
            shaped = first.payloadTypes.size() == 1 &&
                    second.payloadTypes.size() == (isOptional ? 0u : 1u);
        }
        if (shaped) {
            // The payloads are the instance's type arguments: the first case carries the first one
            // and the result's failure case carries the second.
            const auto type = types.find(enumeration.type);
            shaped = type != types.end() && type->second->kind == typing::TypeKind::enumeration &&
                    type->second->symbol == enumeration.symbol &&
                    type->second->arguments.size() == (isOptional ? 1u : 2u) &&
                    enumeration.cases[enumeration.firstRoleCase].payloadTypes[0] == type->second->arguments[0] &&
                    (isOptional ||
                     enumeration.cases[enumeration.secondRoleCase].payloadTypes[0] == type->second->arguments[1]);
        }
        if (!shaped) {
            report(
                    VerificationErrorId::typeMismatch,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "enumeration '" + enumeration.name + "' does not have the shape of its " +
                            (enumeration.role == EnumerationRole::optional ? "optional"
                             : enumeration.role == EnumerationRole::result ? "result" : "unknown") + " role");
        }
    }
}

// A deinit takes only its struct's address and returns nothing (decision 0015). A struct with one
// ends with the hidden live flag, which an inert-zero struct must not carry.
void ModuleVerifier::verifyDeinitializers() {
    for (const auto& structure : module.structures) {
        if (!structure.deinitializer.has_value()) continue;
        const auto deinitializer = functions.find(*structure.deinitializer);
        const auto signatureMatches = deinitializer != functions.end() &&
                deinitializer->second->parameters.size() == 1 &&
                deinitializer->second->parameters[0].value.type == structure.type &&
                deinitializer->second->parameters[0].value.category == ValueCategory::address &&
                !deinitializer->second->returnsValue &&
                !deinitializer->second->isExternal;
        if (!signatureMatches) {
            report(
                    deinitializer == functions.end()
                            ? VerificationErrorId::invalidReference
                            : VerificationErrorId::invalidInstruction,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "deinit of structure '" + structure.name +
                            "' must be a function taking only the structure's address and "
                            "returning nothing");
        }
        const auto liveFlagIsLast = !structure.fields.empty() &&
                structure.fields.back().name == liveFieldName &&
                structure.fields.back().symbol == semantic::invalidSymbolId &&
                types.contains(structure.fields.back().type) &&
                types.at(structure.fields.back().type)->kind == typing::TypeKind::boolean;
        if (isInertZero(structure.role)) {
            // No live flag: the zero value must itself be destroyable (decisions 0025, 0027).
            if (liveFlagIsLast) {
                report(VerificationErrorId::invalidReference, std::nullopt, std::nullopt, std::nullopt,
                       "structure '" + structure.name + "' is inert-zero and must not carry a hidden live flag");
            }
        } else if (!liveFlagIsLast) {
            report(
                    VerificationErrorId::invalidReference,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "structure '" + structure.name +
                            "' declares a deinit but does not end with the hidden live flag");
        }
    }
}

// Fills `entryTypes`. A dictionary entry is never a value (decision 0027): no other type holds one
// or takes it as a type argument.
void ModuleVerifier::verifyEntryTypes() {
    for (const auto& structure : module.structures) {
        if (structure.role == StructureRole::dictEntry) entryTypes.insert(structure.type);
    }
    if (!entryTypes.empty()) {
        const auto holds = [&](const std::string& holder) {
            report(VerificationErrorId::invalidReference, std::nullopt, std::nullopt, std::nullopt,
                   "a dictionary entry is never a value, but '" + holder + "' holds one");
        };
        for (const auto& structure : module.structures) {
            for (const auto& field : structure.fields) {
                if (entryTypes.contains(field.type)) holds(structure.name);
            }
        }
        for (const auto& enumeration : module.enumerations) {
            for (const auto& enumCase : enumeration.cases) {
                for (const auto payload : enumCase.payloadTypes) {
                    if (entryTypes.contains(payload)) holds(enumeration.name);
                }
            }
        }
        for (const auto& type : module.types) {
            for (const auto argument : type.arguments) {
                if (entryTypes.contains(argument)) holds(type.name);
            }
        }
    }
}

// The `array_element` addressor of a collection (decision 0025) has its one fixed signature.
void ModuleVerifier::verifyElementAddressors() {
    for (const auto& function : module.functions) {
        if (!function.elementAddressor) continue;
        // An array's addressor takes an Int index, a dictionary's a key of its first type argument.
        const auto receiver = function.parameters.empty() ? structures.end()
                : structures.find(function.parameters[0].value.type);
        const auto* receiverType = receiver == structures.end() || !types.contains(receiver->first)
                ? nullptr : types.at(receiver->first);
        const bool shaped = function.runtimeContext && !function.isExternal && function.returnsValue &&
                function.parameters.size() == 2 && types.contains(function.resultType) &&
                types.at(function.resultType)->kind == typing::TypeKind::integer &&
                function.parameters[0].value.category == ValueCategory::value && !function.parameters[0].isConsuming &&
                receiverType != nullptr && isInertZero(receiver->second->role) &&
                receiverType->arguments.size() == collectionOwnedArguments(receiver->second->role) &&
                function.parameters[1].value.category == ValueCategory::value && !function.parameters[1].isConsuming &&
                types.contains(function.parameters[1].value.type) &&
                (receiver->second->role == StructureRole::dict
                         ? function.parameters[1].value.type == receiverType->arguments[0]
                         : types.at(function.parameters[1].value.type)->kind == typing::TypeKind::integer);
        if (!shaped) {
            report(VerificationErrorId::invalidInstruction, function.id, std::nullopt, std::nullopt,
                   "element addressor '" + function.name +
                           "' must be a runtime function borrowing an inert-zero struct and an Int index or a key, "
                           "and returning an Int");
        }
    }
}

// An inert-zero struct has a deinit. A copier belongs only to a copyable inert-zero struct, and
// borrows that struct and returns it.
void ModuleVerifier::verifyCopiers() {
    for (const auto& structure : module.structures) {
        if (isInertZero(structure.role) && !structure.deinitializer.has_value()) {
            report(VerificationErrorId::invalidReference, std::nullopt, std::nullopt, std::nullopt,
                   "inert-zero structure '" + structure.name + "' needs a deinit");
        }
        if (!structure.copier.has_value()) continue;
        const auto copier = functions.find(*structure.copier);
        const bool matches = isInertZero(structure.role) && copier != functions.end() &&
                copier->second->parameters.size() == 1 &&
                copier->second->parameters[0].value.type == structure.type &&
                copier->second->parameters[0].value.category == ValueCategory::value &&
                !copier->second->parameters[0].isConsuming &&
                copier->second->returnsValue && copier->second->resultType == structure.type &&
                !copier->second->isExternal && !noncopyableTypes.contains(structure.type);
        if (!matches) {
            report(copier == functions.end() ? VerificationErrorId::invalidReference
                                             : VerificationErrorId::invalidInstruction,
                   std::nullopt, std::nullopt, std::nullopt,
                   "copier of structure '" + structure.name +
                           "' must be a function borrowing only the structure and returning the structure");
        }
    }
}

// Fills `literalFactories` with the well-formed literal methods, one per collection instance.
void ModuleVerifier::verifyLiteralFactories() {
    std::unordered_set<TypeId> literalFactoryTypes;
    for (const auto& function : module.functions) {
        if (!function.literalFactory) continue;
        const auto receiver = function.parameters.empty() ? structures.end()
                : structures.find(function.parameters[0].value.type);
        // Rule 1 of the closed literal protocol (decisions 0026, 0027): the receiver is an array or dictionary
        // handle, and the result the same instance with its one or two owned type arguments.
        const bool shaped = function.runtimeContext && !function.isExternal && function.returnsValue &&
                function.parameters.size() == 3 && receiver != structures.end() &&
                isInertZero(receiver->second->role) &&
                function.parameters[0].value.category == ValueCategory::value &&
                function.parameters[0].isConsuming &&
                function.resultType == function.parameters[0].value.type &&
                types.contains(function.resultType) &&
                types.at(function.resultType)->arguments.size() == collectionOwnedArguments(receiver->second->role) &&
                [&] {
                    for (size_t index = 1; index < 3; ++index) {
                        const auto& parameter = function.parameters[index];
                        const auto type = types.find(parameter.value.type);
                        if (parameter.value.category != ValueCategory::value || parameter.isConsuming ||
                            parameter.isInitializing || type == types.end() ||
                            type->second->kind != typing::TypeKind::integer) return false;
                    }
                    return true;
                }();
        if (!shaped || !literalFactoryTypes.insert(function.resultType).second) {
            report(VerificationErrorId::invalidInstruction, function.id, std::nullopt, std::nullopt,
                   "literal factory '" + function.name +
                           "' must uniquely consume an array or dictionary role struct, borrow two Ints, and return "
                           "that struct");
        } else {
            literalFactories.insert(function.id);
        }
    }
}

} // namespace joyeer::ir::verifier
