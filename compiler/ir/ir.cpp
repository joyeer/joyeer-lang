#include "ir.h"
#include "compiler/frontend/intrinsics.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace joyeer::ir {

namespace {

bool validFloatingLiteral(typing::TypeKind kind, std::string_view text) {
    const auto fits = [text](auto value) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                value, std::chars_format::general);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                std::isfinite(value);
    };
    return kind == typing::TypeKind::float32 ? fits(float{}) :
            kind == typing::TypeKind::float64 && fits(double{});
}

// How an argument for a parameter with `effect` is passed: `inout` and `initializing` by address
// (spec 4.2), the others by value.
ValueCategory effectCategory(syntax::AccessEffect effect) {
    return effect == syntax::AccessEffect::inout || effect == syntax::AccessEffect::initializing
            ? ValueCategory::address
            : ValueCategory::value;
}

bool producesValue(Opcode opcode) {
    switch (opcode) {
        case Opcode::integerConstant:
        case Opcode::floatingConstant:
        case Opcode::booleanConstant:
        case Opcode::stringConstant:
        case Opcode::byteConstant:
        case Opcode::unitConstant:
        case Opcode::stackAllocate:
        case Opcode::load:
        case Opcode::copyValue:
        case Opcode::take:
        case Opcode::add:
        case Opcode::subtract:
        case Opcode::multiply:
        case Opcode::divide:
        case Opcode::remainder:
        case Opcode::less:
        case Opcode::lessEqual:
        case Opcode::greater:
        case Opcode::greaterEqual:
        case Opcode::equal:
        case Opcode::notEqual:
        case Opcode::logicalAnd:
        case Opcode::bitAnd:
        case Opcode::bitOr:
        case Opcode::bitXor:
        case Opcode::shiftLeft:
        case Opcode::shiftRight:
        case Opcode::convertChecked:
        case Opcode::convertWrapping:
        case Opcode::constantValue:
        case Opcode::integerFits:
        case Opcode::constructStruct:
        case Opcode::constructDictionary:
        case Opcode::dictionaryGet:
        case Opcode::fieldAddress:
        case Opcode::extractField:
        case Opcode::constructEnum:
        case Opcode::extractPayload:
        case Opcode::payloadAddress:
        case Opcode::stringUtf8:
        case Opcode::count:
        case Opcode::subscript:
        case Opcode::subscriptAddress:
        case Opcode::functionAddress:
        case Opcode::typeSize:
        case Opcode::typeAlignment:
        case Opcode::integerAddress:
        case Opcode::pointerToInteger:
        case Opcode::cloneCallback:
        case Opcode::destroyCallback:
        case Opcode::slotAddress:
        case Opcode::stackBlock:
            return true;
        default:
            return false;
    }
}

std::string escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\0': result += "\\0"; break;
            default: result += character; break;
        }
    }
    return result;
}

std::string valueName(ValueId value) {
    return "%" + std::to_string(value);
}

std::string typeName(
        const std::unordered_map<TypeId, const TypeName*>& types,
        TypeId type) {
    const auto found = types.find(type);
    return found == types.end()
            ? "<type#" + std::to_string(type) + ">"
            : found->second->name;
}

std::string patternText(
        const Pattern& pattern,
        const std::unordered_map<TypeId, const TypeName*>& types) {
    std::ostringstream out;
    switch (pattern.kind) {
        case PatternKind::wildcard:
            out << '_';
            break;
        case PatternKind::integerLiteral:
            if (types.contains(pattern.type) && typing::isUnsignedInteger(types.at(pattern.type)->kind)) {
                out << static_cast<uint64_t>(pattern.integerValue);
            } else {
                out << pattern.integerValue;
            }
            break;
        case PatternKind::floatingLiteral:
            out << pattern.text;
            break;
        case PatternKind::booleanLiteral:
            out << (pattern.integerValue == 0 ? "false" : "true");
            break;
        case PatternKind::stringLiteral:
            out << '"' << escape(pattern.text) << '"';
            break;
        case PatternKind::byteLiteral:
            out << "byte(" << pattern.integerValue << ')';
            break;
        case PatternKind::enumCase:
            out << "case#" << pattern.symbol.value_or(semantic::invalidSymbolId);
            if (!pattern.payloads.empty()) {
                out << '(';
                for (size_t index = 0; index < pattern.payloads.size(); ++index) {
                    if (index != 0) out << ", ";
                    out << patternText(pattern.payloads[index], types);
                }
                out << ')';
            }
            break;
    }
    out << ':' << typeName(types, pattern.type);
    return out.str();
}

} // namespace

DestructionAnalysis::DestructionAnalysis(const Module& module) {
    std::unordered_map<TypeId, const TypeName*> types;
    std::unordered_map<TypeId, const StructureDefinition*> structures;
    std::unordered_map<TypeId, const EnumerationDefinition*> enumerations;
    for (const auto& type : module.types) types.emplace(type.id, &type);
    for (const auto& structure : module.structures) {
        structures.emplace(structure.type, &structure);
    }
    for (const auto& enumeration : module.enumerations) {
        enumerations.emplace(enumeration.type, &enumeration);
    }

    std::unordered_set<TypeId> visiting;
    std::function<bool(TypeId)> visit = [&](TypeId id) {
        const auto cached = required.find(id);
        if (cached != required.end()) return cached->second;
        const auto found = types.find(id);
        if (found == types.end()) return false;
        if (!visiting.insert(id).second) return true;

        bool result = false;
        switch (found->second->kind) {
            case typing::TypeKind::string:
            case typing::TypeKind::dictionary:
                result = true;
                break;
            case typing::TypeKind::structure: {
                const auto structure = structures.find(id);
                if (structure != structures.end()) {
                    result = isInertZero(structure->second->role) ||
                            structure->second->deinitializer.has_value() ||
                            std::any_of(
                            structure->second->fields.begin(),
                            structure->second->fields.end(),
                            [&visit](const auto& field) { return visit(field.type); });
                }
                break;
            }
            case typing::TypeKind::enumeration: {
                const auto enumeration = enumerations.find(id);
                if (enumeration != enumerations.end()) {
                    result = std::any_of(
                            enumeration->second->cases.begin(),
                            enumeration->second->cases.end(),
                            [&visit](const auto& enumCase) {
                                return std::any_of(
                                        enumCase.payloadTypes.begin(),
                                        enumCase.payloadTypes.end(),
                                        [&visit](TypeId payload) { return visit(payload); });
                            });
                }
                break;
            }
            default:
                break;
        }
        visiting.erase(id);
        required.emplace(id, result);
        return result;
    };
    for (const auto& type : module.types) visit(type.id);
}

bool DestructionAnalysis::requiresDestruction(TypeId type) const {
    const auto found = required.find(type);
    return found != required.end() && found->second;
}

bool requiresDestruction(const Module& module, TypeId type) {
    return DestructionAnalysis(module).requiresDestruction(type);
}

VerificationResult Verifier::verify(const Module& module) const {
    VerificationResult result;
    auto report = [&result](
            VerificationErrorId id,
            std::optional<FunctionId> function,
            std::optional<BlockId> block,
            std::optional<size_t> instruction,
            std::string message) {
        result.errors.push_back(VerificationError {
            id,
            function,
            block,
            instruction,
            std::move(message),
        });
    };

    auto verifySourceInfo = [&report](const SourceInfo& source) {
        if (source.byteLength > std::numeric_limits<uint32_t>::max()) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "source byte length exceeds the 32-bit SourceSpan limit");
        }
        if (source.fileName.empty()) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "source info requires a file name");
        }
        if (source.lineStarts.empty() || source.lineStarts.front() != 0) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "source line starts must begin with byte offset 0");
        }
        for (size_t index = 0; index < source.lineStarts.size(); ++index) {
            if (source.lineStarts[index] > source.byteLength) {
                report(
                        VerificationErrorId::invalidSourceLocation,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "source line start exceeds the source byte length");
                break;
            }
            if (index > 0 && source.lineStarts[index - 1] >= source.lineStarts[index]) {
                report(
                        VerificationErrorId::invalidSourceLocation,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "source line starts must be strictly increasing");
                break;
            }
        }
    };
    if (!module.sourceFiles.empty()) {
        for (const auto& source : module.sourceFiles) verifySourceInfo(source);
    } else if (module.sourceInfo.has_value()) {
        verifySourceInfo(*module.sourceInfo);
    } else {
        const auto hasDebugLocations = !module.debugScopes.empty() ||
                !module.debugVariables.empty() || std::any_of(
                module.functions.begin(),
                module.functions.end(),
                [](const auto& function) {
                    if (function.debugLocation.has_value() || function.debugScope.has_value() ||
                        !function.entryDebugVariableBindings.empty()) {
                        return true;
                    }
                    return std::any_of(
                            function.blocks.begin(),
                            function.blocks.end(),
                            [](const auto& block) {
                                return std::any_of(
                                        block.instructions.begin(),
                                        block.instructions.end(),
                                        [](const auto& instruction) {
                                            return instruction.debugLocation.has_value() ||
                                                    !instruction.debugVariableBindings.empty();
                                        });
                            });
                });
        if (hasDebugLocations) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "module has debug locations but no source info");
        }
    }

    auto verifyDebugLocation = [&module, &report](
            const DebugLocation& location,
            std::optional<FunctionId> function,
            std::optional<BlockId> block,
            std::optional<size_t> instruction,
            const std::string& owner) {
        if (module.sourceFile(0) == nullptr) {
            return;
        }
        const auto* source = module.sourceFile(location.span.sourceId);
        if (source == nullptr) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    function,
                    block,
                    instruction,
                    owner + " debug span references unknown source file " +
                            std::to_string(location.span.sourceId));
            return;
        }
        const auto end = static_cast<uint64_t>(location.span.offset) +
                static_cast<uint64_t>(location.span.length);
        if (location.span.offset > source->byteLength || end > source->byteLength) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    function,
                    block,
                    instruction,
                    owner + " debug span exceeds the source byte length");
        }
    };

    std::unordered_map<TypeId, const TypeName*> types;
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
    for (const auto& type : module.types) {
        const auto kindSlot = static_cast<uint64_t>(type.kind);
        if (kindSlot == 10 || kindSlot > static_cast<uint64_t>(typing::TypeKind::typeParameter)) {
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

    std::unordered_map<TypeId, std::vector<TypeId>> copyDependents;
    std::unordered_set<TypeId> noncopyableTypes;
    std::vector<TypeId> pendingNoncopyable;
    for (const auto& type : module.types) {
        if (type.kind == typing::TypeKind::dictionary) {
            for (const auto argument : type.arguments) copyDependents[argument].push_back(type.id);
        }
    }
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

    std::unordered_map<TypeId, std::vector<TypeId>> typeDependents;
    std::unordered_set<TypeId> nonConcreteTypes;
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
    const auto verifyConcreteType = [&](TypeId type,
                                        std::optional<FunctionId> function,
                                        std::optional<BlockId> block,
                                        std::optional<size_t> instruction,
                                        const std::string& owner) {
        if (!nonConcreteTypes.contains(type)) return;
        report(VerificationErrorId::typeMismatch, function, block, instruction,
               owner + " uses non-concrete type '" + types.at(type)->name + "'");
    };
    for (const auto& constant : module.constants) {
        verifyConcreteType(constant.type, std::nullopt, std::nullopt, std::nullopt, "constant");
    }

    std::unordered_map<TypeId, const StructureDefinition*> structures;
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

    std::unordered_map<TypeId, const EnumerationDefinition*> enumerations;
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

    // Role-bound enums (decision 0024): the case indices must name cases of the required shape, and
    // every definition of one role is an instance of the same declaration.
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
    // The payload of an optional instance's present case.
    const auto optionalPayload = [&enumerations](TypeId type) -> std::optional<TypeId> {
        const auto found = enumerations.find(type);
        if (found == enumerations.end() || found->second->role != EnumerationRole::optional ||
            found->second->firstRoleCase >= found->second->cases.size()) {
            return std::nullopt;
        }
        const auto& payloads = found->second->cases[found->second->firstRoleCase].payloadTypes;
        if (payloads.size() != 1) return std::nullopt;
        return payloads[0];
    };

    auto enumCaseFor = [&enumerations](
            TypeId type,
            semantic::SymbolId symbol) -> const EnumCaseDefinition* {
        const auto enumeration = enumerations.find(type);
        if (enumeration == enumerations.end()) return nullptr;
        const auto found = std::find_if(
                enumeration->second->cases.begin(),
                enumeration->second->cases.end(),
                [symbol](const auto& enumCase) { return enumCase.symbol == symbol; });
        return found == enumeration->second->cases.end() ? nullptr : &*found;
    };

    std::unordered_map<FunctionId, const Function*> functions;
    for (const auto& function : module.functions) {
        if (!functions.emplace(function.id, &function).second) {
            report(
                    VerificationErrorId::duplicateId,
                    function.id,
                    std::nullopt,
                    std::nullopt,
                    "duplicate function id " + std::to_string(function.id));
        }
    }

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

    // A dictionary entry is never a value (decision 0027): no other type holds one or takes it as a type argument.
    std::unordered_set<TypeId> entryTypes;
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

    std::unordered_set<FunctionId> literalFactories;
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

    std::unordered_map<DebugScopeId, const DebugScope*> debugScopes;
    for (const auto& scope : module.debugScopes) {
        if (!debugScopes.emplace(scope.id, &scope).second) {
            report(
                    VerificationErrorId::duplicateId,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "duplicate debug scope id " + std::to_string(scope.id));
        }
        if (!functions.contains(scope.function)) {
            report(
                    VerificationErrorId::invalidReference,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "debug scope references unknown function " +
                            std::to_string(scope.function));
        }
        verifyDebugLocation(
                DebugLocation { scope.span },
                scope.function,
                std::nullopt,
                std::nullopt,
                "debug scope " + std::to_string(scope.id));
    }
    for (const auto& scope : module.debugScopes) {
        if (scope.kind == DebugScopeKind::function && scope.parent.has_value()) {
            report(
                    VerificationErrorId::invalidReference,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "function debug scope must not have a parent");
        }
        if (scope.parent.has_value()) {
            const auto parent = debugScopes.find(*scope.parent);
            if (parent == debugScopes.end()) {
                report(
                        VerificationErrorId::invalidReference,
                        scope.function,
                        std::nullopt,
                        std::nullopt,
                        "debug scope " + std::to_string(scope.id) +
                                " references unknown parent " +
                                std::to_string(*scope.parent));
            } else if (parent->second->function != scope.function) {
                report(
                        VerificationErrorId::invalidReference,
                        scope.function,
                        std::nullopt,
                        std::nullopt,
                        "debug scope parent belongs to another function");
            }
        } else if (scope.kind != DebugScopeKind::function) {
            report(
                    VerificationErrorId::invalidReference,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "lexical debug scope requires a parent");
        }
        std::unordered_set<DebugScopeId> visited;
        auto current = std::optional<DebugScopeId>(scope.id);
        while (current.has_value()) {
            if (!visited.insert(*current).second) {
                report(
                        VerificationErrorId::invalidReference,
                        scope.function,
                        std::nullopt,
                        std::nullopt,
                        "cyclic debug scope hierarchy at scope " +
                                std::to_string(scope.id));
                break;
            }
            const auto found = debugScopes.find(*current);
            if (found == debugScopes.end()) break;
            current = found->second->parent;
        }
    }

    std::unordered_map<DebugVariableId, const DebugVariable*> debugVariables;
    std::unordered_map<FunctionId, std::unordered_set<uint32_t>> parameterIndices;
    for (const auto& variable : module.debugVariables) {
        verifyConcreteType(variable.type, variable.function, std::nullopt, std::nullopt,
                           "debug variable '" + variable.name + "'");
        if (!debugVariables.emplace(variable.id, &variable).second) {
            report(
                    VerificationErrorId::duplicateId,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "duplicate debug variable id " + std::to_string(variable.id));
        }
        const auto scope = debugScopes.find(variable.scope);
        if (scope == debugScopes.end() || scope->second->function != variable.function) {
            report(
                    VerificationErrorId::invalidReference,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "debug variable '" + variable.name +
                            "' references an invalid scope");
        }
        if (!types.contains(variable.type)) {
            report(
                    VerificationErrorId::invalidReference,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "debug variable '" + variable.name +
                            "' references unknown type " + std::to_string(variable.type));
        }
        verifyDebugLocation(
                DebugLocation { variable.span },
                variable.function,
                std::nullopt,
                std::nullopt,
                "debug variable '" + variable.name + "'");
        if (variable.kind == DebugVariableKind::parameter) {
            if (!variable.parameterIndex.has_value() || *variable.parameterIndex == 0 ||
                !functions.contains(variable.function) ||
                *variable.parameterIndex > functions.at(variable.function)->parameters.size() ||
                !parameterIndices[variable.function].insert(*variable.parameterIndex).second) {
                report(
                        VerificationErrorId::invalidReference,
                        variable.function,
                        std::nullopt,
                        std::nullopt,
                        "debug parameter '" + variable.name +
                                "' has an invalid or duplicate parameter index");
            }
        } else if (variable.parameterIndex.has_value()) {
            report(
                    VerificationErrorId::invalidReference,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "non-parameter debug variable '" + variable.name +
                            "' has a parameter index");
        }
    }

    const DestructionAnalysis destruction(module);
    std::unordered_map<std::string, const Function*> nativeSymbols;
    std::unordered_set<std::string> nativeExports;
    for (const auto& function : module.functions) {
        const auto functionId = std::optional<FunctionId>(function.id);
        if (function.alwaysInline && (!function.runtimeContext || function.isExternal)) {
            report(VerificationErrorId::invalidInstruction, functionId, {}, {},
                   "an always-inline runtime wrapper must be an authorized function with a body");
        }
        if (module.isRuntimeObject && function.isRootModule &&
            !function.isExternal && !function.runtimeContext) {
            report(VerificationErrorId::invalidInstruction, functionId, {}, {},
                   "runtime-object root functions require runtime authorization");
        }
        if (function.nativeBinding.has_value()) {
            const auto& binding = *function.nativeBinding;
            const auto hasKind = [&](TypeId id, typing::TypeKind kind) {
                const auto found = types.find(id);
                return found != types.end() && found->second->kind == kind;
            };
            const bool importing = binding.kind == NativeBindingKind::cImport;
            bool valid = (importing || binding.kind == NativeBindingKind::cExport) &&
                    function.runtimeContext && isCIdentifier(binding.symbol) &&
                    function.isExternal == importing &&
                    !(function.isRootModule && function.name == "main") &&
                    (!function.coreContext || function.runtimeContext) &&
                    (importing || !isCEntrySymbol(binding.symbol) ||
                     (function.coreContext && isCoreEntrySymbol(binding.symbol)));
            const auto scalar = [&](TypeId id) {
                return types.contains(id) && typing::isNativeScalar(types.at(id)->kind);
            };
            const bool scalarResult = scalar(function.resultType);
            // A data symbol is an import with no parameters whose result is its address.
            valid &= !binding.dataSymbol ||
                    (importing && function.parameters.empty() && binding.pointerParameters.empty() &&
                     hasKind(function.resultType, typing::TypeKind::integer) &&
                     !isCEntrySymbol(binding.symbol) && !intrinsic::reserved(binding.symbol));
            valid &= (scalarResult || hasKind(function.resultType, typing::TypeKind::voidType) ||
                       hasKind(function.resultType, typing::TypeKind::never)) &&
                    function.returnsValue == scalarResult;
            const auto bufferKind = [&](TypeId id) {
                return typing::nativeBufferKind(id, [&](TypeId value) {
                    const auto found = types.find(value);
                    return found == types.end() ? nullptr : found->second;
                }, [&](TypeId value) {
                    const auto found = structures.find(value);
                    return found != structures.end() && found->second->role == StructureRole::array;
                });
            };
            for (const auto pointer : binding.pointerParameters) {
                valid &= importing && pointer < function.parameters.size();
            }
            for (size_t parameterIndex = 0; parameterIndex < function.parameters.size(); ++parameterIndex) {
                const auto& parameter = function.parameters[parameterIndex];
                const auto buffer = bufferKind(parameter.value.type);
                if (binding.passesPointer(parameterIndex)) {
                    // A bare data pointer: borrowed bytes, or a mutable byte buffer.
                    valid &= buffer == typing::NativeBufferKind::bytes &&
                            !parameter.isConsuming && !parameter.isInitializing && !parameter.acceptsAnyType &&
                            ((parameter.value.category == ValueCategory::value && !parameter.isMutable) ||
                             (parameter.value.category == ValueCategory::address && parameter.isMutable));
                    continue;
                }
                const bool borrowed =
                        (scalar(parameter.value.type) ||
                         (importing && buffer != typing::NativeBufferKind::none)) &&
                        parameter.value.category == ValueCategory::value &&
                        !parameter.isMutable && !parameter.isConsuming &&
                        !parameter.isInitializing && !parameter.acceptsAnyType;
                const bool output = importing && buffer != typing::NativeBufferKind::none &&
                        buffer != typing::NativeBufferKind::byteArrays &&
                        parameter.value.category == ValueCategory::address &&
                        parameter.isMutable && parameter.isInitializing &&
                        !parameter.isConsuming && !parameter.acceptsAnyType;
                valid &= borrowed || output;
            }
            if (intrinsic::reserved(binding.symbol)) {
                // A compiler intrinsic must be a known import with exactly its signature.
                const auto* descriptor = intrinsic::find(binding.symbol);
                std::vector<typing::TypeKind> kinds;
                std::vector<bool> pointers;
                std::vector<bool> mutables;
                for (size_t index = 0; index < function.parameters.size(); ++index) {
                    const auto found = types.find(function.parameters[index].value.type);
                    kinds.push_back(found == types.end() ? typing::TypeKind::error : found->second->kind);
                    pointers.push_back(binding.passesPointer(index));
                    mutables.push_back(function.parameters[index].isMutable);
                }
                const auto result = types.find(function.resultType);
                valid &= importing && descriptor != nullptr && result != types.end() &&
                        intrinsic::matches(*descriptor, result->second->kind, kinds, pointers, mutables);
            }
            if (!valid) {
                report(VerificationErrorId::invalidInstruction, functionId, {}, {},
                       "invalid or unauthorized C binding; expected scalar ABI or supported C import buffers");
            }
            const auto [previous, inserted] = nativeSymbols.emplace(binding.symbol, &function);
            if (!inserted) {
                const auto& other = *previous->second;
                const auto compatible = [&](TypeId a, TypeId b) {
                    if (bufferKind(a) != typing::NativeBufferKind::none ||
                        bufferKind(b) != typing::NativeBufferKind::none) {
                        return bufferKind(a) == bufferKind(b);
                    }
                    return types.contains(a) && types.contains(b) &&
                            typing::sameNativeRepresentation(types.at(a)->kind, types.at(b)->kind);
                };
                bool matches = binding.dataSymbol == other.nativeBinding->dataSymbol &&
                        compatible(function.resultType, other.resultType) &&
                        function.parameters.size() == other.parameters.size();
                for (size_t index = 0; index < std::min(function.parameters.size(), other.parameters.size()); ++index) {
                    matches &= compatible(function.parameters[index].value.type, other.parameters[index].value.type);
                    matches &= function.parameters[index].isInitializing == other.parameters[index].isInitializing;
                    matches &= function.parameters[index].isMutable == other.parameters[index].isMutable;
                    matches &= binding.passesPointer(index) == other.nativeBinding->passesPointer(index);
                }
                if (!matches) {
                    report(VerificationErrorId::callMismatch, functionId, {}, {},
                           "conflicting signatures for C symbol '" + binding.symbol + "'");
                }
            }
            if (binding.kind == NativeBindingKind::cExport &&
                !nativeExports.insert(binding.symbol).second) {
                report(VerificationErrorId::duplicateId, functionId, {}, {},
                       "duplicate C export '" + binding.symbol + "'");
            }
        }
        if (function.debugScope.has_value()) {
            const auto scope = debugScopes.find(*function.debugScope);
            if (scope == debugScopes.end() || scope->second->function != function.id ||
                scope->second->kind != DebugScopeKind::function ||
                scope->second->parent.has_value()) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "function '" + function.name +
                                "' references an invalid root debug scope");
            }
        }
        size_t functionScopeCount = 0;
        for (const auto& scope : module.debugScopes) {
            if (scope.function != function.id) continue;
            if (scope.kind == DebugScopeKind::function) ++functionScopeCount;
            if (!function.debugScope.has_value()) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "function '" + function.name +
                                "' owns debug scopes but has no root scope");
                continue;
            }
            std::unordered_set<DebugScopeId> visited;
            auto current = std::optional<DebugScopeId>(scope.id);
            DebugScopeId root = invalidDebugScopeId;
            while (current.has_value() && visited.insert(*current).second) {
                const auto found = debugScopes.find(*current);
                if (found == debugScopes.end()) break;
                root = found->second->id;
                current = found->second->parent;
            }
            if (root != *function.debugScope) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "debug scope " + std::to_string(scope.id) +
                                " is detached from the function root scope");
            }
            if (scope.id != *function.debugScope &&
                scope.kind != DebugScopeKind::lexicalBlock) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "only the function root may have function debug-scope kind");
            }
        }
        if (function.debugScope.has_value() && functionScopeCount != 1) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function '" + function.name + "' has " +
                            std::to_string(functionScopeCount) +
                            " function debug scopes; exactly one is required");
        }
        const auto hasInstructionDebugLocations = std::any_of(
                function.blocks.begin(),
                function.blocks.end(),
                [](const auto& block) {
                    return std::any_of(
                            block.instructions.begin(),
                            block.instructions.end(),
                            [](const auto& instruction) {
                                return instruction.debugLocation.has_value();
                            });
                });
        if (hasInstructionDebugLocations && !function.debugLocation.has_value()) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function '" + function.name +
                            "' has instruction debug locations but no function location");
        }
        if (function.debugLocation.has_value()) {
            verifyDebugLocation(
                    *function.debugLocation,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function '" + function.name + "'");
                    if (function.debugLocation->scope != function.debugScope) {
                    report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "function location does not reference its root debug scope");
                    }
        }
        verifyConcreteType(function.resultType, functionId, std::nullopt, std::nullopt, "function result");
        if (!types.contains(function.resultType)) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function result references unknown type " +
                            std::to_string(function.resultType));
        }

        std::unordered_map<ValueId, Value> values;
        for (const auto& parameter : function.parameters) {
            verifyConcreteType(parameter.value.type, functionId, std::nullopt, std::nullopt,
                               "parameter '" + parameter.name + "'");
            if (!types.contains(parameter.value.type)) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "parameter '" + parameter.name + "' references unknown type " +
                                std::to_string(parameter.value.type));
            }
                const auto expectedCategory = parameter.isMutable
                    ? ValueCategory::address
                    : ValueCategory::value;
                if (parameter.value.category != expectedCategory) {
                report(
                        VerificationErrorId::invalidInstruction,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "parameter '" + parameter.name +
                            (parameter.isMutable
                                ? "' must be an address for inout access"
                                : "' must be an object value"));
            }
            if (parameter.isMutable && parameter.isConsuming) {
                report(
                        VerificationErrorId::invalidInstruction,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "parameter '" + parameter.name +
                                "' cannot be both inout and consuming");
            }
                        if (parameter.isInitializing && !parameter.isMutable) {
                        report(
                            VerificationErrorId::invalidInstruction,
                            functionId,
                            std::nullopt,
                            std::nullopt,
                            "initializing parameter '" + parameter.name +
                                "' must be an address projection");
                        }
            if (!values.emplace(parameter.value.id, parameter.value).second) {
                report(
                        VerificationErrorId::duplicateId,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "duplicate value id " + std::to_string(parameter.value.id));
            }
        }

        bool hasFunctionDebugVariables = false;
        for (const auto& variable : module.debugVariables) {
            if (variable.function != function.id) continue;
            hasFunctionDebugVariables = true;
            if (variable.kind != DebugVariableKind::parameter) continue;
            if (!variable.parameterIndex.has_value() || *variable.parameterIndex == 0 ||
                *variable.parameterIndex > function.parameters.size()) {
                continue;
            }
            const auto& parameter = function.parameters[*variable.parameterIndex - 1];
            if (variable.type != parameter.value.type || variable.name != parameter.name ||
                variable.scope != function.debugScope) {
                report(
                        VerificationErrorId::typeMismatch,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "debug parameter '" + variable.name +
                                "' does not match its function parameter or root scope");
            }
        }

        if (function.isExternal) {
            if (function.debugScope.has_value() || function.debugLocation.has_value() ||
                hasFunctionDebugVariables || !function.entryDebugVariableBindings.empty()) {
                report(
                        VerificationErrorId::invalidInstruction,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "external function must not carry source debug scopes or variables");
            }
            if (!function.blocks.empty() || function.entry != invalidBlockId) {
                report(
                        VerificationErrorId::invalidInstruction,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "external function must not contain blocks or an entry block");
            }
            continue;
        }

        std::unordered_map<BlockId, const BasicBlock*> blocks;
        for (const auto& block : function.blocks) {
            if (!blocks.emplace(block.id, &block).second) {
                report(
                        VerificationErrorId::duplicateId,
                        functionId,
                        block.id,
                        std::nullopt,
                        "duplicate block id " + std::to_string(block.id));
            }
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.debugLocation.has_value()) {
                    verifyDebugLocation(
                            *instruction.debugLocation,
                            functionId,
                            block.id,
                            index,
                            std::string(opcodeName(instruction.opcode)) + " instruction");
                    if (instruction.debugLocation->scope.has_value()) {
                        const auto scope = debugScopes.find(*instruction.debugLocation->scope);
                        if (scope == debugScopes.end() ||
                            scope->second->function != function.id) {
                            report(
                                    VerificationErrorId::invalidReference,
                                    functionId,
                                    block.id,
                                    index,
                                    "instruction location references a scope from another function");
                        }
                    } else if (!function.isExternal && function.debugScope.has_value()) {
                        report(
                                VerificationErrorId::invalidReference,
                                functionId,
                                block.id,
                                index,
                                "source instruction location has no lexical scope");
                    }
                }
                if (!instruction.result.has_value()) continue;
                verifyConcreteType(instruction.result->type, functionId, block.id, index, "instruction result");
                if (!types.contains(instruction.result->type)) {
                    report(
                            VerificationErrorId::invalidReference,
                            functionId,
                            block.id,
                            index,
                            "result references unknown type " +
                                    std::to_string(instruction.result->type));
                }
                if (!values.emplace(instruction.result->id, *instruction.result).second) {
                    report(
                            VerificationErrorId::duplicateId,
                            functionId,
                            block.id,
                            index,
                            "duplicate value id " +
                                    std::to_string(instruction.result->id));
                }
            }
        }

        std::unordered_map<DebugVariableId, size_t> debugBindingCounts;
        std::unordered_map<ValueId, std::pair<BlockId, size_t>> valueDefinitions;
        for (const auto& block : function.blocks) {
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.result.has_value()) {
                    valueDefinitions.emplace(
                            instruction.result->id,
                            std::make_pair(block.id, index));
                }
            }
        }
        std::unordered_map<BlockId, std::unordered_set<BlockId>> predecessors;
        for (const auto& block : function.blocks) {
            if (block.instructions.empty()) continue;
            for (const auto target : block.instructions.back().targets) {
                if (blocks.contains(target)) predecessors[target].insert(block.id);
            }
            if (block.instructions.back().opcode == Opcode::switchPattern) {
                for (const auto& switchCase : block.instructions.back().switchCases) {
                    if (blocks.contains(switchCase.target)) {
                        predecessors[switchCase.target].insert(block.id);
                    }
                }
            }
        }
        std::unordered_map<BlockId, std::unordered_set<BlockId>> dominators;
        std::unordered_set<BlockId> allBlocks;
        for (const auto& [blockId, block] : blocks) allBlocks.insert(blockId);
        for (const auto& [blockId, block] : blocks) {
            dominators[blockId] = blockId == function.entry
                    ? std::unordered_set<BlockId> { blockId }
                    : allBlocks;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [blockId, block] : blocks) {
                if (blockId == function.entry) continue;
                std::unordered_set<BlockId> next = allBlocks;
                const auto incoming = predecessors.find(blockId);
                if (incoming == predecessors.end() || incoming->second.empty()) {
                    next.clear();
                } else {
                    bool first = true;
                    for (const auto predecessor : incoming->second) {
                        if (first) {
                            next = dominators[predecessor];
                            first = false;
                        } else {
                            for (auto candidate = next.begin(); candidate != next.end();) {
                                if (!dominators[predecessor].contains(*candidate)) {
                                    candidate = next.erase(candidate);
                                } else {
                                    ++candidate;
                                }
                            }
                        }
                    }
                }
                next.insert(blockId);
                if (next != dominators[blockId]) {
                    dominators[blockId] = std::move(next);
                    changed = true;
                }
            }
        }
        struct LiteralUse {
            const Instruction* instruction;
            BlockId block;
            size_t index;
            size_t operand;
        };
        std::unordered_map<ValueId, std::vector<LiteralUse>> literalUses;
        for (const auto& block : function.blocks) {
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                for (size_t operand = 0; operand < instruction.operands.size(); ++operand) {
                    literalUses[instruction.operands[operand]].push_back({ &instruction, block.id, index, operand });
                }
            }
        }
        const auto definingInstruction = [&](ValueId value) -> const Instruction* {
            const auto found = valueDefinitions.find(value);
            if (found == valueDefinitions.end()) return nullptr;
            const auto block = blocks.find(found->second.first);
            if (block == blocks.end() || found->second.second >= block->second->instructions.size()) return nullptr;
            return &block->second->instructions[found->second.second];
        };
        std::unordered_map<ValueId, TypeId> literalBlocks;
        std::unordered_map<ValueId, TypeId> literalSlots;
        std::unordered_map<ValueId, TypeId> literalPlaces;
        // The key and value field addresses of a verified dictionary literal: the only field addresses of an entry.
        std::unordered_set<ValueId> literalEntryFields;
        // A value stored into a literal slot is evaluated before the storage: earlier in its block, in a block that
        // dominates it, or a parameter.
        const auto evaluatedBefore = [&](ValueId value, BlockId blockId, size_t storageIndex) {
            const auto definition = valueDefinitions.find(value);
            return definition == valueDefinitions.end() ||
                    (definition->second.first == blockId && definition->second.second < storageIndex) ||
                    (definition->second.first != blockId && dominators[blockId].contains(definition->second.first));
        };
        // The closed packing protocol of a dictionary literal (decision 0027, Decision 3, rules 1-6), checked rule by
        // rule; returns the first rule that fails, or nothing, and admits the storage, slots, places and field
        // addresses of a valid literal.
        const auto dictionaryLiteralFailure = [&](const BasicBlock& block, size_t index, const LiteralUse& transfer,
                                                  const std::vector<LiteralUse>& slots, size_t transfers,
                                                  bool foreignUse) -> std::string {
            const auto& storage = block.instructions[index];
            const auto element = static_cast<TypeId>(storage.integerValue);
            const auto& call = *transfer.instruction;
            const auto* factory = functions.at(*call.callee);
            const auto result = types.find(factory->resultType);
            const auto entry = structures.find(element);
            const auto entryType = types.find(element);
            // Rule 2: the element is the entry struct over the dictionary's key and value.
            if (entry == structures.end() || entry->second->role != StructureRole::dictEntry ||
                entryType == types.end() || result == types.end() || entry->second->fields.size() != 2 ||
                entryType->second->arguments != result->second->arguments) {
                return "rule 2: the block's element must be the dictionary's entry struct over its key and value";
            }
            // Rule 6, first: the block is transferred exactly once and otherwise used only by its slots.
            if (transfers != 1 || foreignUse) {
                return "rule 6: the block must be transferred exactly once, to the literal method, and otherwise "
                       "used only by its slots";
            }
            const bool sameBlockTransfer = transfer.block == block.id;
            if (!sameBlockTransfer || transfer.index <= index) {
                return "rule 6: the literal method must follow the packing in its block, on an inert zero receiver";
            }
            // Rule 1: the literal method receives the zero receiver, the block and its count.
            if (call.operands.size() != 3 || call.operands[1] != storage.result->id ||
                call.operands[2] != storage.operands[0]) {
                return "rule 1: the literal method must receive a receiver, the block and its count";
            }
            const auto* count = definingInstruction(storage.operands[0]);
            if (count == nullptr || count->opcode != Opcode::integerConstant || count->integerValue < 0) {
                return "rule 5: the entry count must be a non-negative constant";
            }
            const auto& keyField = entry->second->fields[0];
            const auto& valueField = entry->second->fields[1];
            const bool unitValue = types.contains(valueField.type) &&
                    types.at(valueField.type)->kind == typing::TypeKind::voidType;
            // A stored value is used once in the packing window, so one owned value never fills two slots. Its other
            // uses are only on paths that never reach the packing and that the packing never reaches, such as its
            // cleanup on an early exit of a later entry: a block the packing block neither dominates nor is dominated
            // by. Anything else would use the value before it is stored or after it was transferred.
            const auto usedOnceInWindow = [&](ValueId value) {
                size_t inWindow = 0;
                for (const auto& use : literalUses[value]) {
                    if (use.block == block.id && use.index > index && use.index < transfer.index) {
                        ++inWindow;
                    } else if (use.block == block.id || dominators[use.block].contains(block.id) ||
                               dominators[block.id].contains(use.block)) {
                        return false;
                    }
                }
                return inWindow == 1;
            };
            std::unordered_set<int64_t> keys;
            std::unordered_set<int64_t> storedValues;
            std::vector<ValueId> admittedSlots;
            std::vector<ValueId> admittedPlaces;
            std::vector<ValueId> admittedFields;
            for (const auto& use : slots) {
                // Rule 3: a bounded constant slot of the block, with exactly one entry place.
                const auto& slot = *use.instruction;
                const auto* offset = slot.operands.size() == 2 ? definingInstruction(slot.operands[1]) : nullptr;
                const bool inRange = offset != nullptr && offset->opcode == Opcode::integerConstant &&
                        offset->integerValue >= 0 && offset->integerValue < count->integerValue;
                if (!slot.result.has_value() || slot.integerValue != storage.integerValue || use.block != block.id ||
                    use.index <= index || !inRange || literalUses[slot.result->id].size() != 1) {
                    return "rule 3: each slot must be a bounded constant slot of the block with one entry place";
                }
                const auto address = literalUses[slot.result->id].front();
                const auto& place = *address.instruction;
                if (address.operand != 0 || place.opcode != Opcode::integerAddress || place.operands.size() != 1 ||
                    !place.result.has_value() || place.result->type != element ||
                    place.result->category != ValueCategory::address || address.block != block.id ||
                    address.index <= use.index) {
                    return "rule 3: each slot must be a bounded constant slot of the block with one entry place";
                }
                std::unordered_set<semantic::SymbolId> projected;
                for (const auto& projection : literalUses[place.result->id]) {
                    const auto& field = *projection.instruction;
                    const bool isKey = field.symbol == keyField.symbol;
                    const bool isValue = field.symbol == valueField.symbol;
                    // Rule 3: the place is only projected, once to each of the key and the value, in the window.
                    if (projection.operand != 0 || field.opcode != Opcode::fieldAddress || !field.result.has_value() ||
                        !field.symbol.has_value() || (!isKey && !isValue) || !projected.insert(*field.symbol).second ||
                        projection.block != block.id || projection.index <= address.index ||
                        (sameBlockTransfer && projection.index >= transfer.index)) {
                        return "rule 3: an entry place may only be projected once to each of its key and value "
                               "fields, in the packing window";
                    }
                    // Rule 5: a Void value has no storage, so it is neither projected nor stored.
                    if (isValue && unitValue) return "rule 5: a Void value is neither projected nor stored";
                    // Rule 4: one store, of a value of exactly the field's type evaluated before the storage.
                    const auto fieldType = isKey ? keyField.type : valueField.type;
                    const auto& stores = literalUses[field.result->id];
                    bool stored = stores.size() == 1 && field.result->type == fieldType &&
                            field.result->category == ValueCategory::address;
                    for (const auto& store : stores) {
                        const auto& instruction = *store.instruction;
                        const auto value = instruction.operands.size() == 2 ? values.find(instruction.operands[0])
                                                                            : values.end();
                        stored = stored && store.operand == 1 && instruction.opcode == Opcode::store &&
                                value != values.end() && value->second.type == fieldType &&
                                value->second.category == ValueCategory::value && store.block == block.id &&
                                store.index > projection.index && sameBlockTransfer && store.index < transfer.index &&
                                evaluatedBefore(value->first, block.id, index) &&
                                usedOnceInWindow(value->first);
                    }
                    if (!stored) {
                        return "rule 4: each field address must have exactly one use, a store in its block before "
                               "the transfer of a value of the field's type evaluated before the storage, used once in "
                               "the packing and otherwise only on paths that never reach it";
                    }
                    (isKey ? keys : storedValues).insert(offset->integerValue);
                    admittedFields.push_back(field.result->id);
                }
                admittedSlots.push_back(slot.result->id);
                admittedPlaces.push_back(place.result->id);
            }
            // Rule 5: every entry has its key and, unless the value is Void, its value.
            const auto entries = static_cast<size_t>(count->integerValue);
            if (slots.size() != entries || keys.size() != entries || storedValues.size() != (unitValue ? 0 : entries)) {
                return "rule 5: the key must be stored once for every entry, and the value too unless it is Void";
            }
            // Rule 6: the transfer follows the packing in the block, on an inert zero receiver, and the window holds
            // only packing operations: no load, no escaping address, nothing fallible.
            const auto* receiver = definingInstruction(call.operands[0]);
            const auto* zeroPlace = receiver != nullptr && receiver->opcode == Opcode::load &&
                    receiver->operands.size() == 1 ? definingInstruction(receiver->operands[0]) : nullptr;
            bool inert = sameBlockTransfer && transfer.index > index && zeroPlace != nullptr &&
                    zeroPlace->opcode == Opcode::stackAllocate && zeroPlace->result.has_value() &&
                    zeroPlace->result->type == factory->resultType;
            bool zeroed = false;
            if (inert) {
                for (const auto& use : literalUses[zeroPlace->result->id]) {
                    if (use.instruction->opcode == Opcode::zeroInitialize && use.operand == 0 &&
                        use.block == block.id && use.index < index && !zeroed) {
                        zeroed = true;
                    } else if (use.instruction != receiver || use.operand != 0 || use.block != block.id ||
                               use.index >= index || !zeroed) {
                        inert = false;
                    }
                }
            }
            if (!inert || !zeroed) {
                return "rule 6: the literal method must follow the packing in its block, on an inert zero receiver";
            }
            for (size_t packed = index + 1; packed < transfer.index; ++packed) {
                switch (block.instructions[packed].opcode) {
                    case Opcode::integerConstant:
                    case Opcode::slotAddress:
                    case Opcode::integerAddress:
                    case Opcode::fieldAddress:
                    case Opcode::store:
                        break;
                    default:
                        return "rule 6: the packing window holds only constants, slots, entry places, field "
                               "addresses and stores";
                }
            }
            literalBlocks.emplace(storage.result->id, element);
            for (const auto slot : admittedSlots) literalSlots.emplace(slot, element);
            for (const auto place : admittedPlaces) literalPlaces.emplace(place, element);
            literalEntryFields.insert(admittedFields.begin(), admittedFields.end());
            return {};
        };
        // User functions may only form raw slots through this closed, straight-line initialize-and-transfer
        // protocol. Ordinary source uses of the layout intrinsics still need runtime authorization.
        for (const auto& block : function.blocks) {
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& storage = block.instructions[index];
                if (storage.opcode != Opcode::stackBlock || !storage.result.has_value() ||
                    storage.operands.size() != 1) continue;
                if (function.runtimeContext && std::none_of(literalUses[storage.result->id].begin(),
                        literalUses[storage.result->id].end(), [&](const auto& use) {
                            return use.operand == 1 && use.instruction->opcode == Opcode::call &&
                                    use.instruction->callee.has_value() &&
                                    literalFactories.contains(*use.instruction->callee);
                        })) {
                    continue;
                }
                const auto* count = definingInstruction(storage.operands[0]);
                const auto element = static_cast<TypeId>(storage.integerValue);
                bool valid = count != nullptr && count->opcode == Opcode::integerConstant &&
                        count->integerValue >= 0 && types.contains(element);
                const LiteralUse* transfer = nullptr;
                std::vector<LiteralUse> slots;
                size_t transfers = 0;
                bool foreignUse = false;
                for (const auto& use : literalUses[storage.result->id]) {
                    if (use.instruction->opcode == Opcode::call && use.operand == 1 &&
                        use.instruction->callee.has_value() && literalFactories.contains(*use.instruction->callee)) {
                        if (transfer != nullptr) valid = false;
                        transfer = &use;
                        ++transfers;
                    } else if (use.instruction->opcode == Opcode::slotAddress && use.operand == 0) {
                        slots.push_back(use);
                    } else {
                        valid = false;
                        foreignUse = true;
                    }
                }
                const auto* factory = transfer != nullptr ? functions.at(*transfer->instruction->callee) : nullptr;
                const auto handle = factory != nullptr ? structures.find(factory->resultType) : structures.end();
                if (handle != structures.end() && handle->second->role == StructureRole::dict) {
                    const auto failure = dictionaryLiteralFailure(block, index, *transfer, slots, transfers, foreignUse);
                    if (!failure.empty()) {
                        report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                               "dictionary literal " + failure);
                    }
                    continue;
                }
                valid = valid && transfer != nullptr;
                if (transfer != nullptr) {
                    const auto& call = *transfer->instruction;
                    const auto result = types.find(factory->resultType);
                    valid = valid && transfer->block == block.id && transfer->index > index &&
                            call.operands.size() == 3 && call.operands[2] == storage.operands[0] &&
                            result != types.end() && result->second->arguments.size() == 1 &&
                            result->second->arguments[0] == element;
                    const auto* receiver = call.operands.empty() ? nullptr : definingInstruction(call.operands[0]);
                    const auto* place = receiver != nullptr && receiver->opcode == Opcode::load &&
                            receiver->operands.size() == 1 ? definingInstruction(receiver->operands[0]) : nullptr;
                    valid = valid && place != nullptr && place->opcode == Opcode::stackAllocate &&
                            place->result.has_value() && place->result->type == factory->resultType;
                    bool zeroed = false;
                    if (place != nullptr && place->result.has_value()) {
                        for (const auto& use : literalUses[place->result->id]) {
                            if (use.instruction->opcode == Opcode::zeroInitialize && use.operand == 0 &&
                                use.block == block.id && use.index < index) {
                                if (zeroed) valid = false;
                                zeroed = true;
                            } else if (use.instruction != receiver || use.operand != 0 ||
                                       use.block != block.id || use.index >= index || !zeroed) {
                                valid = false;
                            }
                        }
                    }
                    valid = valid && zeroed;
                    for (size_t packed = index + 1; packed < transfer->index; ++packed) {
                        switch (block.instructions[packed].opcode) {
                            case Opcode::integerConstant:
                            case Opcode::slotAddress:
                            case Opcode::integerAddress:
                            case Opcode::store:
                                break;
                            default:
                                valid = false;
                                break;
                        }
                    }
                }
                std::unordered_set<int64_t> initialized;
                std::vector<ValueId> admittedSlots;
                std::vector<ValueId> admittedPlaces;
                for (const auto& use : slots) {
                    const auto& slot = *use.instruction;
                    const auto* offset = slot.operands.size() == 2 ? definingInstruction(slot.operands[1]) : nullptr;
                    const bool inRange = offset != nullptr && offset->opcode == Opcode::integerConstant &&
                            count != nullptr && offset->integerValue >= 0 && offset->integerValue < count->integerValue;
                    valid = valid && slot.result.has_value() && slot.integerValue == storage.integerValue &&
                            use.block == block.id && use.index > index && inRange;
                    if (!slot.result.has_value()) continue;
                    const auto& addresses = literalUses[slot.result->id];
                    valid = valid && addresses.size() == 1;
                    for (const auto& address : addresses) {
                        const auto& place = *address.instruction;
                        valid = valid && address.operand == 0 && place.opcode == Opcode::integerAddress &&
                                place.operands.size() == 1 && place.result.has_value() &&
                                place.result->type == element && place.result->category == ValueCategory::address &&
                                address.block == block.id && address.index > use.index;
                        if (!place.result.has_value()) continue;
                        const auto& stores = literalUses[place.result->id];
                        valid = valid && stores.size() == 1;
                        for (const auto& store : stores) {
                            const auto& instruction = *store.instruction;
                            const auto value = instruction.operands.size() == 2
                                    ? values.find(instruction.operands[0]) : values.end();
                            const auto definition = value == values.end() ? valueDefinitions.end()
                                    : valueDefinitions.find(value->first);
                            const bool evaluatedFirst = value != values.end() &&
                                    (definition == valueDefinitions.end() ||
                                     (definition->second.first == block.id && definition->second.second < index) ||
                                     (definition->second.first != block.id &&
                                      dominators[block.id].contains(definition->second.first)));
                            valid = valid && store.operand == 1 && instruction.opcode == Opcode::store &&
                                    value != values.end() && value->second.type == element &&
                                    value->second.category == ValueCategory::value &&
                                    store.block == block.id && store.index > address.index &&
                                    transfer != nullptr && store.index < transfer->index && evaluatedFirst &&
                                    inRange && initialized.insert(offset->integerValue).second;
                        }
                        admittedPlaces.push_back(place.result->id);
                    }
                    admittedSlots.push_back(slot.result->id);
                }
                valid = valid && count != nullptr && types.contains(element) &&
                        (types.at(element)->kind == typing::TypeKind::voidType
                                ? slots.empty()
                                : initialized.size() == static_cast<uint64_t>(count->integerValue));
                if (!valid) {
                    report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                           "array literal storage must initialize each bounded slot exactly once, then transfer "
                           "only to its matching literal factory with an inert-zero receiver");
                    continue;
                }
                literalBlocks.emplace(storage.result->id, element);
                for (const auto slot : admittedSlots) literalSlots.emplace(slot, element);
                for (const auto place : admittedPlaces) literalPlaces.emplace(place, element);
            }
        }
        // The escape rule of a library array's element place (decision 0025): an element address is
        // used only in blocks its definition dominates, and only to load, store, take, destroy, project
        // or pass as a call argument, never converted back to an Int, returned, stored as a value or
        // gathered into an aggregate. The rule is transitive through derived addresses.
        {
            std::unordered_set<ValueId> elementAddresses;
            const auto addressorCall = [&](const Instruction& instruction) {
                if (instruction.opcode != Opcode::call || !instruction.callee.has_value()) return false;
                const auto callee = functions.find(*instruction.callee);
                return callee != functions.end() && callee->second->elementAddressor;
            };
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.opcode != Opcode::integerAddress || !instruction.result.has_value() ||
                        instruction.operands.size() != 1) {
                        continue;
                    }
                    const auto definition = valueDefinitions.find(instruction.operands[0]);
                    if (definition == valueDefinitions.end()) continue;
                    const auto definingBlock = blocks.find(definition->second.first);
                    if (definingBlock != blocks.end() &&
                        definition->second.second < definingBlock->second->instructions.size() &&
                        addressorCall(definingBlock->second->instructions[definition->second.second])) {
                        elementAddresses.insert(instruction.result->id);
                    }
                }
            }
            for (bool grew = !elementAddresses.empty(); grew;) {
                grew = false;
                for (const auto& block : function.blocks) {
                    for (const auto& instruction : block.instructions) {
                        if ((instruction.opcode == Opcode::fieldAddress || instruction.opcode == Opcode::payloadAddress ||
                             instruction.opcode == Opcode::subscriptAddress) &&
                            instruction.result.has_value() && !instruction.operands.empty() &&
                            elementAddresses.contains(instruction.operands[0]) &&
                            elementAddresses.insert(instruction.result->id).second) {
                            grew = true;
                        }
                    }
                }
            }
            for (const auto& block : function.blocks) {
                for (size_t index = 0; index < block.instructions.size(); ++index) {
                    const auto& instruction = block.instructions[index];
                    for (size_t position = 0; position < instruction.operands.size(); ++position) {
                        const auto operand = instruction.operands[position];
                        if (!elementAddresses.contains(operand)) continue;
                        bool allowed = false;
                        switch (instruction.opcode) {
                            case Opcode::load:
                            case Opcode::take:
                            case Opcode::destroy:
                            case Opcode::fieldAddress:
                            case Opcode::payloadAddress:
                            case Opcode::subscriptAddress:
                            case Opcode::dictionarySet:
                            case Opcode::dictionaryGet:
                                allowed = position == 0;
                                break;
                            case Opcode::store:
                                allowed = position == 1;
                                break;
                            case Opcode::call:
                                allowed = true;
                                break;
                            case Opcode::callIndirect:
                                allowed = position >= 1;
                                break;
                            default:
                                break;
                        }
                        if (!allowed) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                                   std::string("an element address may not be used by ") +
                                           opcodeName(instruction.opcode) +
                                           ": it is only loaded, stored, taken, destroyed, projected or "
                                           "passed as an argument");
                        }
                        const auto definition = valueDefinitions.find(operand);
                        if (definition != valueDefinitions.end() &&
                            (definition->second.first == block.id
                                     ? definition->second.second >= index
                                     : !dominators[block.id].contains(definition->second.first))) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                                   "an element address is used where its definition does not dominate");
                        }
                    }
                }
            }
        }
        auto verifyDebugBinding = [&](const DebugVariableBinding& binding,
                                      std::optional<BlockId> block,
                                      std::optional<size_t> instruction,
                                      bool entryBinding,
                                      const Instruction* anchor) {
            const auto variable = debugVariables.find(binding.variable);
            const auto address = values.find(binding.address);
            if (variable == debugVariables.end() ||
                variable->second->function != function.id) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        block,
                        instruction,
                        "debug binding references an unknown variable in this function");
                return;
            }
            if (address == values.end() ||
                address->second.category != ValueCategory::address ||
                address->second.type != variable->second->type) {
                report(
                        VerificationErrorId::typeMismatch,
                        functionId,
                        block,
                        instruction,
                        "debug binding for '" + variable->second->name +
                                "' requires address storage of the variable type");
                return;
            }
            if (entryBinding) {
                const auto parameterIndex = variable->second->parameterIndex;
                if (variable->second->kind != DebugVariableKind::parameter ||
                    !parameterIndex.has_value() || *parameterIndex == 0 ||
                    *parameterIndex > function.parameters.size() ||
                    function.parameters[*parameterIndex - 1].value.id != binding.address ||
                    function.parameters[*parameterIndex - 1].value.category !=
                            ValueCategory::address) {
                    report(
                            VerificationErrorId::invalidReference,
                            functionId,
                            block,
                            instruction,
                            "entry debug binding does not match an address parameter");
                    return;
                }
            } else {
                if (anchor == nullptr || !block.has_value() || !instruction.has_value() ||
                    !anchor->debugLocation.has_value() ||
                    anchor->debugLocation->scope != variable->second->scope) {
                    report(
                            VerificationErrorId::invalidReference,
                            functionId,
                            block,
                            instruction,
                            "debug binding for '" + variable->second->name +
                                    "' is not anchored in its lexical scope");
                    return;
                }
                std::optional<ValueId> declaredAddress;
                switch (anchor->opcode) {
                    case Opcode::stackAllocate:
                        if (anchor->result.has_value()) {
                            declaredAddress = anchor->result->id;
                        }
                        break;
                    case Opcode::zeroInitialize:
                        if (!anchor->operands.empty()) declaredAddress = anchor->operands[0];
                        break;
                    case Opcode::store:
                        if (anchor->operands.size() >= 2) declaredAddress = anchor->operands[1];
                        break;
                    default:
                        break;
                }
                if (!declaredAddress.has_value() || *declaredAddress != binding.address) {
                    report(
                            VerificationErrorId::invalidInstruction,
                            functionId,
                            block,
                            instruction,
                            "debug binding for '" + variable->second->name +
                                    "' is not attached to its storage declaration");
                    return;
                }
                if (variable->second->kind == DebugVariableKind::parameter) {
                    const auto parameterIndex = variable->second->parameterIndex;
                    if (!parameterIndex.has_value() || *parameterIndex == 0 ||
                        *parameterIndex > function.parameters.size()) {
                        return;
                    }
                    const auto& parameter = function.parameters[*parameterIndex - 1];
                    if (parameter.value.category == ValueCategory::address ||
                        anchor->opcode != Opcode::store || anchor->operands.size() < 2 ||
                        anchor->operands[0] != parameter.value.id) {
                        report(
                                VerificationErrorId::invalidInstruction,
                                functionId,
                                block,
                                instruction,
                                "value debug parameter '" + variable->second->name +
                                        "' must bind at the store of its incoming parameter");
                        return;
                    }
                }
                const auto isParameterAddress = std::any_of(
                        function.parameters.begin(),
                        function.parameters.end(),
                        [&binding](const auto& parameter) {
                            return parameter.value.id == binding.address &&
                                    parameter.value.category == ValueCategory::address;
                        });
                const auto definition = valueDefinitions.find(binding.address);
                const auto definedEarlier = anchor->opcode == Opcode::stackAllocate ||
                    isParameterAddress ||
                    (definition != valueDefinitions.end() &&
                     ((definition->second.first == *block &&
                       definition->second.second < *instruction) ||
                      (definition->second.first != *block &&
                       dominators[*block].contains(definition->second.first))));
                if (!definedEarlier) {
                    report(
                            VerificationErrorId::invalidReference,
                            functionId,
                            block,
                            instruction,
                            "debug binding for '" + variable->second->name +
                                    "' uses storage not available at the declaration point");
                    return;
                }
            }
            ++debugBindingCounts[binding.variable];
        };
        for (const auto& binding : function.entryDebugVariableBindings) {
            verifyDebugBinding(
                    binding,
                    std::nullopt,
                    std::nullopt,
                    true,
                    nullptr);
        }
        for (const auto& block : function.blocks) {
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                for (const auto& binding : block.instructions[index].debugVariableBindings) {
                    verifyDebugBinding(
                            binding,
                            block.id,
                            index,
                            false,
                            &block.instructions[index]);
                }
            }
        }
        for (const auto& variable : module.debugVariables) {
            if (variable.function != function.id) continue;
            const auto count = debugBindingCounts[variable.id];
            if (count != 1) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "debug variable '" + variable.name + "' has " +
                                std::to_string(count) +
                                " storage bindings; exactly one is required");
            }
        }

        if (!blocks.contains(function.entry)) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function entry references unknown block " +
                            std::to_string(function.entry));
        }

        for (const auto& parameter : function.parameters) {
            if (entryTypes.contains(parameter.value.type)) {
                report(VerificationErrorId::typeMismatch, functionId, std::nullopt, std::nullopt,
                       "a dictionary entry is never a value, so no parameter has its type");
            }
        }
        if (entryTypes.contains(function.resultType)) {
            report(VerificationErrorId::typeMismatch, functionId, std::nullopt, std::nullopt,
                   "a dictionary entry is never a value, so no function returns one");
        }
        for (const auto& block : function.blocks) {
            bool foundTerminator = false;
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                const auto location = std::optional<size_t>(index);
                const auto opcodeSlot = static_cast<uint64_t>(instruction.opcode);
                if ((opcodeSlot >= 35 && opcodeSlot <= 38) ||
                    opcodeSlot > static_cast<uint64_t>(Opcode::stackBlock)) {
                    report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                           "instruction has an unknown or removed opcode");
                }
                if (foundTerminator) {
                    report(
                            VerificationErrorId::instructionAfterTerminator,
                            functionId,
                            block.id,
                            location,
                            "instruction appears after block terminator");
                }
                foundTerminator |= isTerminator(instruction.opcode);
                if (!entryTypes.empty()) {
                    // A dictionary entry is never a value (decision 0027): no instruction yields or takes one, an
                    // entry address is only a place of verified literal storage, and only the layout queries and
                    // that storage name the type.
                    const auto misused = [&](const Value& value) {
                        return entryTypes.contains(value.type) &&
                                (value.category != ValueCategory::address || !literalPlaces.contains(value.id));
                    };
                    bool misuse = instruction.result.has_value() && misused(*instruction.result);
                    for (const auto operand : instruction.operands) {
                        const auto found = values.find(operand);
                        misuse = misuse || (found != values.end() && misused(found->second));
                    }
                    const bool namesEntry = entryTypes.contains(static_cast<TypeId>(instruction.integerValue));
                    switch (instruction.opcode) {
                        case Opcode::destroyRange:
                        case Opcode::cloneAt:
                        case Opcode::cloneCallback:
                        case Opcode::destroyCallback:
                        case Opcode::linearRangeGuard:
                            misuse = misuse || namesEntry;
                            break;
                        case Opcode::stackBlock:
                            misuse = misuse || (namesEntry && (!instruction.result.has_value() ||
                                                               !literalBlocks.contains(instruction.result->id)));
                            break;
                        case Opcode::slotAddress:
                            misuse = misuse || (namesEntry && (!instruction.result.has_value() ||
                                                               !literalSlots.contains(instruction.result->id)));
                            break;
                        default:
                            break;
                    }
                    if (misuse) {
                        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                               "a dictionary entry is never a value: only verified dictionary literal storage "
                               "addresses one");
                    }
                }

                const auto knownCallee = instruction.callee.has_value() &&
                        functions.contains(*instruction.callee);
                const auto expectsResult = instruction.opcode == Opcode::call
                        ? knownCallee && functions.at(*instruction.callee)->returnsValue
                        : instruction.opcode == Opcode::callIndirect
                        // Depends on the callee's function type, checked with the instruction.
                        ? instruction.result.has_value()
                        : producesValue(instruction.opcode);
                if (expectsResult != instruction.result.has_value()) {
                    report(
                            VerificationErrorId::invalidInstruction,
                            functionId,
                            block.id,
                            location,
                            std::string(opcodeName(instruction.opcode)) +
                                    (expectsResult
                                            ? " must define a result"
                                            : " must not define a result"));
                }

                std::vector<const Value*> operands;
                operands.reserve(instruction.operands.size());
                for (const auto operand : instruction.operands) {
                    const auto found = values.find(operand);
                    if (found == values.end()) {
                        report(
                                VerificationErrorId::invalidReference,
                                functionId,
                                block.id,
                                location,
                                "instruction references unknown value " +
                                        std::to_string(operand));
                        operands.push_back(nullptr);
                    } else {
                        operands.push_back(&found->second);
                    }
                }
                for (const auto target : instruction.targets) {
                    if (!blocks.contains(target)) {
                        report(
                                VerificationErrorId::invalidReference,
                                functionId,
                                block.id,
                                location,
                                "instruction references unknown block " +
                                        std::to_string(target));
                    }
                }

                for (const auto& switchCase : instruction.switchCases) {
                    if (!blocks.contains(switchCase.target)) {
                        report(
                                VerificationErrorId::invalidReference,
                                functionId,
                                block.id,
                                location,
                                "pattern switch references unknown block " +
                                        std::to_string(switchCase.target));
                    }
                }
                auto requireShape = [&](size_t operandCount, size_t targetCount) {
                    if (instruction.operands.size() == operandCount &&
                        instruction.targets.size() == targetCount) {
                        return true;
                    }
                    report(
                            VerificationErrorId::invalidInstruction,
                            functionId,
                            block.id,
                            location,
                            std::string(opcodeName(instruction.opcode)) +
                                    " expects " + std::to_string(operandCount) +
                                    " operand(s) and " + std::to_string(targetCount) +
                                    " target(s)");
                    return false;
                };

                switch (instruction.opcode) {
                    case Opcode::unitConstant: {
                        requireShape(0, 0);
                        const auto* unitType = instruction.result.has_value() &&
                                types.contains(instruction.result->type)
                                ? types.at(instruction.result->type) : nullptr;
                        if (unitType == nullptr ||
                            unitType->kind != typing::TypeKind::voidType ||
                            instruction.result->category != ValueCategory::value) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "unit constant must produce a Void value");
                        }
                        break;
                    }
                    case Opcode::typeSize:
                    case Opcode::typeAlignment: {
                        requireShape(0, 0);
                        const auto queried = static_cast<TypeId>(instruction.integerValue);
                        const auto* result = instruction.result.has_value() && types.contains(instruction.result->type)
                                ? types.at(instruction.result->type) : nullptr;
                        if (!types.contains(queried) || result == nullptr ||
                            result->kind != typing::TypeKind::integer ||
                            instruction.result->category != ValueCategory::value) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId, block.id, location,
                                    "a size or alignment query names a known type and yields an Int");
                        } else if (nonConcreteTypes.contains(queried) ||
                                   types.at(queried)->kind == typing::TypeKind::never) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId, block.id, location,
                                    "a size or alignment query needs a concrete type with values, not '" +
                                            types.at(queried)->name + "'");
                        }
                        if (!function.runtimeContext) {
                            report(
                                    VerificationErrorId::callMismatch,
                                    functionId, block.id, location,
                                    "a layout query requires runtime authorization");
                        }
                        break;
                    }
                    case Opcode::destroyRange:
                    case Opcode::cloneAt:
                    case Opcode::linearRangeGuard:
                    case Opcode::slotAddress:
                    case Opcode::stackBlock:
                    case Opcode::cloneCallback:
                    case Opcode::destroyCallback: {
                        const bool noOperands = instruction.opcode == Opcode::cloneCallback ||
                                instruction.opcode == Opcode::destroyCallback;
                        const bool yieldsInt = noOperands || instruction.opcode == Opcode::slotAddress ||
                                instruction.opcode == Opcode::stackBlock;
                        const bool guard = instruction.opcode == Opcode::linearRangeGuard;
                        const bool stackBlockOp = instruction.opcode == Opcode::stackBlock;
                        const size_t expected = noOperands ? 0 : (guard || stackBlockOp) ? 1 : 2;
                        if (requireShape(expected, 0)) {
                            for (const auto* operand : operands) {
                                const auto* operandType = operand != nullptr && types.contains(operand->type)
                                        ? types.at(operand->type) : nullptr;
                                if (operandType == nullptr || operandType->kind != typing::TypeKind::integer ||
                                    operand->category != ValueCategory::value) {
                                    report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                           std::string(opcodeName(instruction.opcode)) + " takes Int operands");
                                }
                            }
                        }
                        if (yieldsInt) {
                            const auto* result = instruction.result.has_value() && types.contains(instruction.result->type)
                                    ? types.at(instruction.result->type) : nullptr;
                            if (result == nullptr || result->kind != typing::TypeKind::integer ||
                                instruction.result->category != ValueCategory::value) {
                                report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                       std::string(opcodeName(instruction.opcode)) + " yields an Int");
                            }
                        } else if (instruction.result.has_value()) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                   std::string(opcodeName(instruction.opcode)) + " yields no value");
                        }
                        if (stackBlockOp && instruction.operands.size() == 1 && operands[0] != nullptr) {
                            // The count is a constant, so the backend sizes the storage at compile time.
                            const auto definition = valueDefinitions.find(operands[0]->id);
                            const auto definingBlock = definition == valueDefinitions.end() ? blocks.end()
                                    : blocks.find(definition->second.first);
                            const auto* defining = definingBlock != blocks.end() &&
                                    definition->second.second < definingBlock->second->instructions.size()
                                    ? &definingBlock->second->instructions[definition->second.second] : nullptr;
                            if (defining == nullptr || defining->opcode != Opcode::integerConstant ||
                                defining->integerValue < 0) {
                                report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                       "stack_block takes a non-negative constant count");
                            }
                        }
                        if (!guard) {
                            const auto queried = static_cast<TypeId>(instruction.integerValue);
                            const auto* count = stackBlockOp && instruction.operands.size() == 1
                                    ? definingInstruction(instruction.operands[0]) : nullptr;
                            const bool emptyNeverLiteral = count != nullptr &&
                                    count->opcode == Opcode::integerConstant && count->integerValue == 0 &&
                                    instruction.result.has_value() &&
                                    literalBlocks.contains(instruction.result->id);
                            if (!types.contains(queried) || nonConcreteTypes.contains(queried) ||
                                (types.at(queried)->kind == typing::TypeKind::never && !emptyNeverLiteral)) {
                                report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                       std::string(opcodeName(instruction.opcode)) +
                                               " needs a concrete type with values");
                            } else if ((instruction.opcode == Opcode::cloneAt ||
                                        instruction.opcode == Opcode::cloneCallback) &&
                                       noncopyableTypes.contains(queried)) {
                                report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                       std::string(opcodeName(instruction.opcode)) +
                                               " needs a copyable type, but '" + types.at(queried)->name +
                                               "' is noncopyable");
                            }
                        }
                        const bool literalStorage = instruction.result.has_value() &&
                                ((instruction.opcode == Opcode::stackBlock &&
                                  literalBlocks.contains(instruction.result->id)) ||
                                 (instruction.opcode == Opcode::slotAddress &&
                                  literalSlots.contains(instruction.result->id)));
                        if (!function.runtimeContext && !literalStorage) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "a collection capability requires runtime authorization");
                        }
                        break;
                    }
                    case Opcode::integerAddress: {
                        const auto* source = requireShape(1, 0) && operands[0] != nullptr && types.contains(operands[0]->type)
                                ? types.at(operands[0]->type) : nullptr;
                        if (source == nullptr || source->kind != typing::TypeKind::integer ||
                            operands[0]->category != ValueCategory::value ||
                            !instruction.result.has_value() ||
                            instruction.result->category != ValueCategory::address ||
                            !types.contains(instruction.result->type)) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId, block.id, location,
                                    "an integer address takes an Int and yields the address of a known type");
                        } else if (types.at(instruction.result->type)->kind == typing::TypeKind::never) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId, block.id, location,
                                    "an integer address cannot address a slot of 'Never'");
                        }
                        // A library array's element place is the one integer address user code may form: its
                        // operand is the result of a call of an element addressor (decision 0025).
                        bool fromAddressor = false;
                        std::optional<TypeId> addressedElement;
                        if (operands.size() == 1 && operands[0] != nullptr) {
                            const auto definition = valueDefinitions.find(operands[0]->id);
                            const auto definingBlock = definition == valueDefinitions.end() ? blocks.end()
                                    : blocks.find(definition->second.first);
                            const auto* defining = definingBlock != blocks.end() &&
                                    definition->second.second < definingBlock->second->instructions.size()
                                    ? &definingBlock->second->instructions[definition->second.second] : nullptr;
                            const auto callee = defining != nullptr && defining->opcode == Opcode::call &&
                                    defining->callee.has_value() ? functions.find(*defining->callee) : functions.end();
                            fromAddressor = callee != functions.end() && callee->second->elementAddressor;
                            if (fromAddressor && !callee->second->parameters.empty() &&
                                types.contains(callee->second->parameters[0].value.type)) {
                                const auto& receiver = *types.at(callee->second->parameters[0].value.type);
                                const auto collection = structures.find(receiver.id);
                                const auto element = collection != structures.end()
                                        ? collectionElementArgument(collection->second->role) : 0;
                                if (element < receiver.arguments.size()) {
                                    addressedElement = receiver.arguments[element];
                                }
                            }
                        }
                        if (fromAddressor && instruction.result.has_value() &&
                            instruction.result->type != addressedElement.value_or(instruction.result->type)) {
                            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                   "an element address must have the element type of the array it indexes");
                        }
                        const bool fromLiteral = instruction.result.has_value() &&
                                literalPlaces.contains(instruction.result->id);
                        if (!function.runtimeContext && !fromAddressor && !fromLiteral) {
                            report(
                                    VerificationErrorId::callMismatch,
                                    functionId, block.id, location,
                                    "an integer address requires runtime authorization");
                        }
                        break;
                    }
                    case Opcode::pointerToInteger: {
                        const auto* source = requireShape(1, 0) && operands[0] != nullptr &&
                                types.contains(operands[0]->type) ? types.at(operands[0]->type) : nullptr;
                        const auto* result = instruction.result.has_value() && types.contains(instruction.result->type)
                                ? types.at(instruction.result->type) : nullptr;
                        if (source == nullptr || result == nullptr || result->kind != typing::TypeKind::integer ||
                            instruction.result->category != ValueCategory::value ||
                            (operands[0]->category != ValueCategory::address &&
                             source->kind != typing::TypeKind::function)) {
                            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                   "a pointer conversion takes a typed slot or function address and yields an Int");
                        }
                        if (!function.runtimeContext) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "a pointer conversion requires runtime authorization");
                        }
                        if (source != nullptr && operands[0]->category == ValueCategory::value &&
                            source->kind == typing::TypeKind::function) {
                            const auto definition = valueDefinitions.find(operands[0]->id);
                            const auto definingBlock = definition == valueDefinitions.end() ? blocks.end()
                                    : blocks.find(definition->second.first);
                            const auto* address = definingBlock != blocks.end() &&
                                    definition->second.second < definingBlock->second->instructions.size()
                                    ? &definingBlock->second->instructions[definition->second.second] : nullptr;
                            const auto callee = address != nullptr && address->opcode == Opcode::functionAddress &&
                                    address->callee.has_value() ? functions.find(*address->callee) : functions.end();
                            const bool matches = source->arguments.size() == 2 &&
                                    source->effects == std::vector<syntax::AccessEffect> {syntax::AccessEffect::consuming} &&
                                    types.contains(source->arguments[0]) &&
                                    types.at(source->arguments[0])->kind == typing::TypeKind::integer &&
                                    types.contains(source->arguments[1]) &&
                                    types.at(source->arguments[1])->kind == typing::TypeKind::voidType &&
                                    callee != functions.end() && callee->second->runtimeContext &&
                                    !callee->second->nativeBinding.has_value() && !callee->second->isExternal &&
                                    !callee->second->returnsValue;
                            if (!matches) {
                                report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                       "a runtime callback address requires a named authorized Joyeer "
                                       "function with one consuming Int parameter and a Void result");
                            }
                        }
                        break;
                    }
                    case Opcode::integerFits:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            instruction.result.has_value()) {
                            const auto* source = types.contains(operands[0]->type)
                                    ? types.at(operands[0]->type) : nullptr;
                            const auto target = static_cast<TypeId>(instruction.integerValue);
                            const auto* targetType = types.contains(target) ? types.at(target) : nullptr;
                            const auto* result = types.contains(instruction.result->type)
                                    ? types.at(instruction.result->type) : nullptr;
                            if (operands[0]->category != ValueCategory::value ||
                                instruction.result->category != ValueCategory::value ||
                                source == nullptr || targetType == nullptr || result == nullptr ||
                                !typing::isInteger(source->kind) || !typing::isInteger(targetType->kind) ||
                                result->kind != typing::TypeKind::boolean) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId, block.id, location,
                                        "integer range test requires integer operand and target types and a Boolean result");
                            }
                        }
                        break;
                    case Opcode::constantValue: {
                        requireShape(0, 0);
                        const auto index = static_cast<uint64_t>(instruction.integerValue);
                        if (!instruction.result.has_value() ||
                            instruction.result->category != ValueCategory::value ||
                            index >= module.constants.size() ||
                            module.constants[index].type != instruction.result->type ||
                            module.constants[index].data.empty()) {
                            report(
                                    VerificationErrorId::invalidInstruction,
                                    functionId, block.id, location,
                                    "constant reference must name a module constant of the result type");
                        }
                        break;
                    }
                    case Opcode::integerConstant:
                    case Opcode::floatingConstant:
                    case Opcode::booleanConstant:
                    case Opcode::stringConstant:
                    case Opcode::byteConstant: {
                        requireShape(0, 0);
                        if (instruction.result.has_value() &&
                            instruction.result->category != ValueCategory::value) {
                            report(
                                    VerificationErrorId::invalidInstruction,
                                    functionId,
                                    block.id,
                                    location,
                                    "constant result must be an object value");
                        }
                        if (instruction.result.has_value() && types.contains(instruction.result->type)) {
                            const auto kind = types.at(instruction.result->type)->kind;
                            bool valid = false;
                            switch (instruction.opcode) {
                                case Opcode::integerConstant:
                                    if (typing::isInteger(kind)) {
                                        const auto bits = typing::scalarBitWidth(kind);
                                        valid = bits == 64 || (typing::isSignedInteger(kind)
                                                ? instruction.integerValue >= -(int64_t{1} << (bits - 1)) &&
                                                  instruction.integerValue < (int64_t{1} << (bits - 1))
                                                : static_cast<uint64_t>(instruction.integerValue) < (uint64_t{1} << bits));
                                    }
                                    break;
                                case Opcode::floatingConstant:
                                    valid = validFloatingLiteral(kind, instruction.text);
                                    break;
                                case Opcode::booleanConstant:
                                    valid = kind == typing::TypeKind::boolean &&
                                            (instruction.integerValue == 0 || instruction.integerValue == 1);
                                    break;
                                case Opcode::byteConstant:
                                    valid = kind == typing::TypeKind::uint8 &&
                                            instruction.integerValue >= 0 && instruction.integerValue <= 255;
                                    break;
                                case Opcode::stringConstant: valid = kind == typing::TypeKind::string; break;
                                default: break;
                            }
                            if (!valid) {
                                report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                        "constant kind or value does not match its result type");
                            }
                        }
                        break;
                    }
                    case Opcode::stackAllocate:
                        requireShape(0, 0);
                        if (instruction.result.has_value() &&
                            instruction.result->category != ValueCategory::address) {
                            report(
                                    VerificationErrorId::invalidInstruction,
                                    functionId,
                                    block.id,
                                    location,
                                    "stack allocation result must be an address");
                        }
                        break;
                    case Opcode::zeroInitialize:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            operands[0]->category != ValueCategory::address) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "zero initialization requires addressable storage");
                        }
                        break;
                    case Opcode::load:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            instruction.result.has_value() &&
                            (operands[0]->category != ValueCategory::address ||
                             instruction.result->category != ValueCategory::value ||
                             operands[0]->type != instruction.result->type)) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "load address and result types must match");
                        }
                        break;
                    case Opcode::store:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr &&
                            (operands[0]->category != ValueCategory::value ||
                             operands[1]->category != ValueCategory::address ||
                             operands[0]->type != operands[1]->type)) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "stored value and destination address types must match");
                        }
                        break;
                    case Opcode::copyValue:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            instruction.result.has_value() &&
                            (operands[0]->category != ValueCategory::value ||
                             instruction.result->category != ValueCategory::value ||
                             operands[0]->type != instruction.result->type)) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "copied value and result types must match");
                        }
                        if (!operands.empty() && operands[0] != nullptr &&
                            noncopyableTypes.contains(operands[0]->type)) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                   "copy cannot duplicate a noncopyable deinit owner or a value containing one");
                        }
                        break;
                    case Opcode::take:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            instruction.result.has_value() &&
                            (operands[0]->category != ValueCategory::address ||
                             instruction.result->category != ValueCategory::value ||
                             operands[0]->type != instruction.result->type)) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "taken storage and result types must match");
                        }
                        break;
                    case Opcode::destroy:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            (operands[0]->category != ValueCategory::address ||
                             !destruction.requiresDestruction(operands[0]->type))) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "destroy requires addressable nontrivial storage");
                        }
                        break;
                    case Opcode::divide:
                    case Opcode::remainder:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr && instruction.result.has_value()) {
                            const auto* type = types.contains(operands[0]->type)
                                    ? types.at(operands[0]->type) : nullptr;
                            if (operands[0]->category != ValueCategory::value ||
                                operands[1]->category != ValueCategory::value ||
                                instruction.result->category != ValueCategory::value ||
                                operands[0]->type != operands[1]->type ||
                                instruction.result->type != operands[0]->type ||
                                type == nullptr || !typing::isNumeric(type->kind) ||
                                (instruction.opcode == Opcode::remainder && !typing::isInteger(type->kind))) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId, block.id, location,
                                        "division and remainder require matching numeric values and result");
                            }
                        }
                        break;
                    case Opcode::bitAnd:
                    case Opcode::bitOr:
                    case Opcode::bitXor:
                    case Opcode::shiftLeft:
                    case Opcode::shiftRight:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr && instruction.result.has_value()) {
                            const auto* type = types.contains(operands[0]->type)
                                    ? types.at(operands[0]->type) : nullptr;
                            const auto* count = types.contains(operands[1]->type)
                                    ? types.at(operands[1]->type) : nullptr;
                            const auto isShift = instruction.opcode == Opcode::shiftLeft ||
                                    instruction.opcode == Opcode::shiftRight;
                            if (operands[0]->category != ValueCategory::value ||
                                operands[1]->category != ValueCategory::value ||
                                instruction.result->category != ValueCategory::value ||
                                instruction.result->type != operands[0]->type ||
                                (!isShift && operands[0]->type != operands[1]->type) ||
                                type == nullptr || !typing::isInteger(type->kind) ||
                                (isShift && (count == nullptr || !typing::isInteger(count->kind)))) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId, block.id, location,
                                        "bitwise operators require integer operands and a matching result");
                            }
                        }
                        break;
                    case Opcode::convertChecked:
                    case Opcode::convertWrapping:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            instruction.result.has_value()) {
                            const auto* source = types.contains(operands[0]->type)
                                    ? types.at(operands[0]->type) : nullptr;
                            const auto* target = types.contains(instruction.result->type)
                                    ? types.at(instruction.result->type) : nullptr;
                            if (operands[0]->category != ValueCategory::value ||
                                instruction.result->category != ValueCategory::value ||
                                source == nullptr || target == nullptr ||
                                !typing::isInteger(source->kind) || !typing::isInteger(target->kind)) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId, block.id, location,
                                        "integer conversion requires integer operand and result values");
                            }
                        }
                        break;
                    case Opcode::add:
                    case Opcode::subtract:
                    case Opcode::multiply:
                    case Opcode::less:
                    case Opcode::lessEqual:
                    case Opcode::greater:
                    case Opcode::greaterEqual:
                    case Opcode::equal:
                    case Opcode::notEqual:
                    case Opcode::logicalAnd:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr && instruction.result.has_value()) {
                            const auto categoriesAreValues =
                                    operands[0]->category == ValueCategory::value &&
                                    operands[1]->category == ValueCategory::value &&
                                    instruction.result->category == ValueCategory::value;
                            const auto operandTypesMatch = operands[0]->type == operands[1]->type;
                            const auto isArithmetic = instruction.opcode == Opcode::add ||
                                    instruction.opcode == Opcode::subtract ||
                                    instruction.opcode == Opcode::multiply;
                            const auto resultMatches = !isArithmetic ||
                                    instruction.result->type == operands[0]->type;
                            if (!categoriesAreValues || !operandTypesMatch || !resultMatches) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "operator operand/result categories or types do not match");
                            }
                        }
                        break;
                    case Opcode::call: {
                        if (!instruction.targets.empty()) {
                            report(
                                    VerificationErrorId::invalidInstruction,
                                    functionId,
                                    block.id,
                                    location,
                                    "call must not contain block targets");
                        }
                        if (!knownCallee) {
                            report(
                                    VerificationErrorId::invalidReference,
                                    functionId,
                                    block.id,
                                    location,
                                    "call references an unknown function");
                            break;
                        }
                        const auto& callee = *functions.at(*instruction.callee);
                        if (!function.runtimeContext && callee.literalFactory &&
                            (instruction.operands.size() != 3 ||
                             !literalBlocks.contains(instruction.operands[1]))) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "a literal factory requires verified literal storage");
                        }
                        if (callee.nativeBinding.has_value() && !function.runtimeContext) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "direct call to a C binding requires runtime authorization");
                        }
                        bool matches = instruction.operands.size() == callee.parameters.size();
                        const auto count = std::min(
                                instruction.operands.size(),
                                callee.parameters.size());
                        for (size_t argument = 0; argument < count; ++argument) {
                                matches &= operands[argument] != nullptr &&
                                    operands[argument]->category ==
                                        callee.parameters[argument].value.category &&
                                (callee.parameters[argument].acceptsAnyType ||
                                 operands[argument]->type ==
                                     callee.parameters[argument].value.type);
                        }
                        matches &= callee.returnsValue == instruction.result.has_value();
                        if (callee.returnsValue && instruction.result.has_value()) {
                            matches &= instruction.result->category == ValueCategory::value &&
                                    instruction.result->type == callee.resultType;
                        }
                        if (!matches) {
                            report(
                                    VerificationErrorId::callMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "call arguments or result do not match callee signature");
                        }
                        // A call that cannot return ends its block (decision 0020).
                        const auto* calleeResult = types.contains(callee.resultType) ? types.at(callee.resultType) : nullptr;
                        if (calleeResult != nullptr && calleeResult->kind == typing::TypeKind::never &&
                            (index + 1 >= block.instructions.size() ||
                             block.instructions[index + 1].opcode != Opcode::unreachable)) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                   "call of a function returning Never must be followed by unreachable");
                        }
                        break;
                    }
                        case Opcode::constructStruct: {
                        const auto* structure = instruction.result.has_value()
                            ? (structures.contains(instruction.result->type)
                                ? structures.at(instruction.result->type)
                                : nullptr)
                            : nullptr;
                        bool matches = structure != nullptr && instruction.symbol.has_value() &&
                            *instruction.symbol == structure->symbol &&
                            instruction.result->category == ValueCategory::value &&
                            instruction.operands.size() == structure->fields.size() &&
                            instruction.targets.empty();
                        const auto count = structure == nullptr
                            ? 0
                            : std::min(instruction.operands.size(), structure->fields.size());
                        for (size_t field = 0; field < count; ++field) {
                            matches &= operands[field] != nullptr &&
                                operands[field]->category == ValueCategory::value &&
                                operands[field]->type == structure->fields[field].type;
                        }
                        if (!matches) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "struct construction does not match its definition");
                        }
                        break;
                        }
                        case Opcode::constructDictionary: {
                        const auto* resultType = instruction.result.has_value() &&
                            types.contains(instruction.result->type)
                            ? types.at(instruction.result->type)
                            : nullptr;
                        bool matches = resultType != nullptr &&
                            resultType->kind == typing::TypeKind::dictionary &&
                            resultType->arguments.size() == 2 &&
                            instruction.result->category == ValueCategory::value &&
                            instruction.operands.size() % 2 == 0 &&
                            instruction.targets.empty();
                        if (resultType != nullptr && resultType->arguments.size() == 2) {
                            for (size_t index = 0; index < operands.size(); ++index) {
                                matches &= operands[index] != nullptr &&
                                        operands[index]->category == ValueCategory::value &&
                                        operands[index]->type ==
                                                resultType->arguments[index % 2];
                            }
                        }
                        if (!matches) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "dictionary construction entries do not match key/value types");
                        }
                        break;
                        }
                        case Opcode::dictionarySet:
                        if (requireShape(3, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr && operands[2] != nullptr) {
                            const auto found = types.find(operands[0]->type);
                            const auto* dictionaryType = found == types.end()
                                    ? nullptr
                                    : found->second;
                            if (operands[0]->category != ValueCategory::address ||
                                operands[1]->category != ValueCategory::value ||
                                operands[2]->category != ValueCategory::value ||
                                dictionaryType == nullptr ||
                                dictionaryType->kind != typing::TypeKind::dictionary ||
                                dictionaryType->arguments.size() != 2 ||
                                dictionaryType->arguments[0] != operands[1]->type ||
                                dictionaryType->arguments[1] != operands[2]->type) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "dictionary set requires a dictionary address and matching key/value");
                            }
                        }
                        break;
                    case Opcode::functionAddress: {
                        if (!requireShape(0, 0)) break;
                        if (!knownCallee) {
                            report(VerificationErrorId::invalidReference, functionId, block.id, location,
                                   "function_address references an unknown function");
                            break;
                        }
                        const auto& callee = *functions.at(*instruction.callee);
                        const auto* functionType = instruction.result.has_value() &&
                                        types.contains(instruction.result->type)
                                ? types.at(instruction.result->type)
                                : nullptr;
                        if (callee.literalFactory) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "literal factories cannot be formed as function values");
                        }
                        // Only a Joyeer function has the Joyeer convention; a C import, a data symbol
                        // and a builtin accepting any type have no address of a function type.
                        if (callee.nativeBinding.has_value() && !function.runtimeContext) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "function_address of a C binding requires runtime authorization");
                        }
                        // The root `main` is not a value (decision 0020).
                        bool matches = !callee.isExternal && !(callee.isRootModule && callee.name == "main") &&
                                (!callee.nativeBinding.has_value() ||
                                 (callee.nativeBinding->kind == NativeBindingKind::cExport &&
                                  !callee.nativeBinding->dataSymbol)) &&
                                functionType != nullptr &&
                                instruction.result->category == ValueCategory::value &&
                                functionType->kind == typing::TypeKind::function &&
                                functionType->effects.size() == callee.parameters.size() &&
                                functionType->arguments.size() == callee.parameters.size() + 1;
                        for (size_t parameter = 0; matches && parameter < callee.parameters.size(); ++parameter) {
                            const auto& declared = callee.parameters[parameter];
                            const auto effect = functionType->effects[parameter];
                            matches &= !declared.acceptsAnyType &&
                                    declared.value.type == functionType->arguments[parameter] &&
                                    declared.value.category == effectCategory(effect) &&
                                    declared.isConsuming == (effect == syntax::AccessEffect::consuming) &&
                                    declared.isInitializing == (effect == syntax::AccessEffect::initializing);
                        }
                        if (matches) matches = callee.resultType == functionType->arguments.back();
                        if (!matches) {
                            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                                   "function_address requires a Joyeer function and a result of exactly its function type");
                        }
                        break;
                    }
                    case Opcode::callIndirect: {
                        if (!instruction.targets.empty() || instruction.callee.has_value() ||
                            instruction.operands.empty() || operands[0] == nullptr) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                   "call_indirect needs a function value operand and no callee or targets");
                            break;
                        }
                        const auto* functionType = types.contains(operands[0]->type)
                                ? types.at(operands[0]->type)
                                : nullptr;
                        if (operands[0]->category != ValueCategory::value || functionType == nullptr ||
                            functionType->kind != typing::TypeKind::function ||
                            functionType->arguments.size() != functionType->effects.size() + 1) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "call_indirect operand 0 must be a value of function type");
                            break;
                        }
                        const auto parameterCount = functionType->effects.size();
                        bool matches = instruction.operands.size() == parameterCount + 1;
                        for (size_t parameter = 0; matches && parameter < parameterCount; ++parameter) {
                            const auto* operand = operands[parameter + 1];
                            matches &= operand != nullptr &&
                                    operand->category == effectCategory(functionType->effects[parameter]) &&
                                    operand->type == functionType->arguments[parameter];
                        }
                        const auto resultType = functionType->arguments.back();
                        const auto* result = types.contains(resultType) ? types.at(resultType) : nullptr;
                        const bool isNever = result != nullptr && result->kind == typing::TypeKind::never;
                        const bool returnsValue = result != nullptr && !isNever &&
                                result->kind != typing::TypeKind::voidType;
                        matches &= returnsValue == instruction.result.has_value();
                        if (returnsValue && instruction.result.has_value()) {
                            matches &= instruction.result->category == ValueCategory::value &&
                                    instruction.result->type == resultType;
                        }
                        if (!matches) {
                            report(VerificationErrorId::callMismatch, functionId, block.id, location,
                                   "call_indirect arguments or result do not match the function type");
                        }
                        // A call that cannot return ends its block.
                        if (isNever && (index + 1 >= block.instructions.size() ||
                                        block.instructions[index + 1].opcode != Opcode::unreachable)) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                   "call_indirect of a function returning Never must be followed by unreachable");
                        }
                        break;
                    }
                    case Opcode::dictionaryGet:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr && instruction.result.has_value()) {
                            const auto* dictionaryType = types.contains(operands[0]->type)
                                    ? types.at(operands[0]->type)
                                    : nullptr;
                            const auto* resultType = types.contains(instruction.result->type)
                                    ? types.at(instruction.result->type)
                                    : nullptr;
                            const auto matches =
                                    operands[0]->category == ValueCategory::value &&
                                    operands[1]->category == ValueCategory::value &&
                                    instruction.result->category == ValueCategory::value &&
                                    dictionaryType != nullptr &&
                                    dictionaryType->kind == typing::TypeKind::dictionary &&
                                    dictionaryType->arguments.size() == 2 &&
                                    dictionaryType->arguments[0] == operands[1]->type &&
                                    resultType != nullptr &&
                                    optionalPayload(resultType->id) == std::optional<TypeId>(dictionaryType->arguments[1]);
                            if (!matches) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "dictionary get requires a dictionary value and matching key, producing Optional<V>");
                            }
                        }
                        break;
                        case Opcode::fieldAddress:
                        case Opcode::extractField: {
                        const auto shapeMatches = requireShape(1, 0);
                        const auto* structure = shapeMatches && operands[0] != nullptr &&
                            structures.contains(operands[0]->type)
                            ? structures.at(operands[0]->type)
                            : nullptr;
                        const auto field = structure == nullptr || !instruction.symbol.has_value()
                            ? static_cast<const FieldDefinition*>(nullptr)
                            : [&]() -> const FieldDefinition* {
                                const auto found = std::find_if(
                                    structure->fields.begin(),
                                    structure->fields.end(),
                                    [&instruction](const auto& candidate) {
                                    return candidate.symbol != semantic::invalidSymbolId &&
                                        candidate.symbol == *instruction.symbol;
                                    });
                                return found == structure->fields.end() ? nullptr : &*found;
                            }();
                        const auto expectedCategory = instruction.opcode == Opcode::fieldAddress
                            ? ValueCategory::address
                            : ValueCategory::value;
                        bool matches = field != nullptr && instruction.result.has_value() &&
                            operands[0]->category == expectedCategory &&
                            instruction.result->category == expectedCategory &&
                            instruction.result->type == field->type;
                        if (!matches) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "field projection does not match its structure definition");
                        }
                        // A dictionary entry is never a value (decision 0027): its fields are addressed only by
                        // the packing of a verified dictionary literal.
                        if (structure != nullptr && structure->role == StructureRole::dictEntry &&
                            (instruction.opcode != Opcode::fieldAddress || !instruction.result.has_value() ||
                             !literalEntryFields.contains(instruction.result->id))) {
                            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                                   "a dictionary entry field is addressed only by verified dictionary literal "
                                   "storage");
                        }
                        break;
                        }
                        case Opcode::constructEnum: {
                        const auto* enumCase = instruction.result.has_value() &&
                            instruction.symbol.has_value()
                            ? enumCaseFor(instruction.result->type, *instruction.symbol)
                            : nullptr;
                        bool matches = enumCase != nullptr && instruction.targets.empty() &&
                            instruction.result->category == ValueCategory::value &&
                            instruction.operands.size() == enumCase->payloadTypes.size();
                        const auto count = enumCase == nullptr
                            ? 0
                            : std::min(
                                instruction.operands.size(),
                                enumCase->payloadTypes.size());
                        for (size_t payload = 0; payload < count; ++payload) {
                            matches &= operands[payload] != nullptr &&
                                operands[payload]->category == ValueCategory::value &&
                                operands[payload]->type == enumCase->payloadTypes[payload];
                        }
                        if (!matches) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "enum construction does not match its case definition");
                        }
                        break;
                        }
                        case Opcode::extractPayload: {
                        const auto shapeMatches = requireShape(1, 0);
                        const auto* enumCase = shapeMatches && operands[0] != nullptr &&
                            instruction.symbol.has_value()
                            ? enumCaseFor(operands[0]->type, *instruction.symbol)
                            : nullptr;
                        const auto index = instruction.integerValue;
                        const auto indexIsValid = enumCase != nullptr && index >= 0 &&
                            static_cast<size_t>(index) < enumCase->payloadTypes.size();
                        if (!indexIsValid || !instruction.result.has_value() ||
                            operands[0]->category != ValueCategory::value ||
                            instruction.result->category != ValueCategory::value ||
                            instruction.result->type !=
                                enumCase->payloadTypes[static_cast<size_t>(index)]) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "enum payload extraction does not match its case definition");
                        }
                        break;
                        }
                        case Opcode::payloadAddress: {
                        const auto shapeMatches = requireShape(1, 0);
                        const auto* enumCase = shapeMatches && operands[0] != nullptr &&
                            instruction.symbol.has_value()
                            ? enumCaseFor(operands[0]->type, *instruction.symbol)
                            : nullptr;
                        const auto index = instruction.integerValue;
                        const auto indexIsValid = enumCase != nullptr && index >= 0 &&
                            static_cast<size_t>(index) < enumCase->payloadTypes.size();
                        if (!indexIsValid || !instruction.result.has_value() ||
                            operands[0]->category != ValueCategory::address ||
                            instruction.result->category != ValueCategory::address ||
                            instruction.result->type !=
                                enumCase->payloadTypes[static_cast<size_t>(index)]) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "enum payload address does not match its case definition");
                        }
                        break;
                        }
                        case Opcode::stringUtf8:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            instruction.result.has_value()) {
                            const auto* sourceType = types.contains(operands[0]->type)
                                ? types.at(operands[0]->type)
                                : nullptr;
                            const auto* resultType = types.contains(instruction.result->type)
                                ? types.at(instruction.result->type)
                                : nullptr;
                            const auto* elementType = resultType != nullptr &&
                                structures.contains(resultType->id) &&
                                structures.at(resultType->id)->role == StructureRole::array &&
                                resultType->arguments.size() == 1 &&
                                types.contains(resultType->arguments[0])
                                ? types.at(resultType->arguments[0])
                                : nullptr;
                            const auto matches = sourceType != nullptr &&
                                sourceType->kind == typing::TypeKind::string &&
                                operands[0]->category == ValueCategory::value &&
                                instruction.result->category == ValueCategory::value &&
                                elementType != nullptr &&
                                elementType->kind == typing::TypeKind::uint8;
                            if (!matches) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "string_utf8 requires String and produces [UInt8]");
                            }
                        }
                        break;
                        case Opcode::count:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                                instruction.result.has_value()) {
                                const auto* baseType = types.contains(operands[0]->type)
                                        ? types.at(operands[0]->type)
                                        : nullptr;
                                const auto* resultType = types.contains(instruction.result->type)
                                        ? types.at(instruction.result->type)
                                        : nullptr;
                                const auto matches = baseType != nullptr && resultType != nullptr &&
                                        operands[0]->category == ValueCategory::value &&
                                        instruction.result->category == ValueCategory::value &&
                                        (baseType->kind == typing::TypeKind::string ||
                                         baseType->kind == typing::TypeKind::dictionary) &&
                                        resultType->kind == typing::TypeKind::integer;
                                if (!matches) {
                                    report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "count requires String/Dict and produces Int");
                                }
                        }
                        break;
                        case Opcode::subscript:
                        case Opcode::subscriptAddress:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                                operands[1] != nullptr && instruction.result.has_value()) {
                                const auto* baseType = types.contains(operands[0]->type)
                                        ? types.at(operands[0]->type)
                                        : nullptr;
                                const auto* indexType = types.contains(operands[1]->type)
                                        ? types.at(operands[1]->type)
                                        : nullptr;
                                std::optional<TypeId> expectedIndex;
                                std::optional<TypeId> expectedResult;
                                if (baseType != nullptr) {
                                    if (baseType->kind == typing::TypeKind::string) {
                                        const auto intType = std::find_if(
                                                module.types.begin(),
                                                module.types.end(),
                                                [](const auto& type) {
                                                    return type.kind == typing::TypeKind::integer;
                                                });
                                        const auto byteType = std::find_if(
                                                module.types.begin(),
                                                module.types.end(),
                                                [](const auto& type) {
                                                    return type.kind == typing::TypeKind::uint8;
                                                });
                                        if (intType != module.types.end()) expectedIndex = intType->id;
                                        if (byteType != module.types.end()) expectedResult = byteType->id;
                                    } else if (baseType->kind == typing::TypeKind::dictionary &&
                                               baseType->arguments.size() == 2) {
                                        expectedIndex = baseType->arguments[0];
                                        expectedResult = baseType->arguments[1];
                                    }
                                }
                                const auto expectedCategory = instruction.opcode == Opcode::subscript
                                        ? ValueCategory::value
                                        : ValueCategory::address;
                                const auto matches = indexType != nullptr &&
                                        expectedIndex.has_value() && expectedResult.has_value() &&
                                        operands[0]->category == expectedCategory &&
                                        operands[1]->category == ValueCategory::value &&
                                        operands[1]->type == *expectedIndex &&
                                        instruction.result->category == expectedCategory &&
                                        instruction.result->type == *expectedResult;
                                if (!matches) {
                                    report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "subscript index/result types do not match its base type");
                                }
                        }
                        break;
                    case Opcode::branch:
                        requireShape(0, 1);
                        break;
                    case Opcode::conditionalBranch:
                        if (requireShape(1, 2) && operands[0] != nullptr &&
                            operands[0]->category != ValueCategory::value) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "conditional branch condition must be an object value");
                        }
                        break;
                    case Opcode::switchPattern: {
                        const auto shapeMatches = requireShape(1, 0);
                        bool matches = shapeMatches && operands[0] != nullptr &&
                                operands[0]->category == ValueCategory::value &&
                                !instruction.switchCases.empty();
                        std::function<bool(const Pattern&, TypeId)> verifyPattern;
                        verifyPattern = [&](const Pattern& pattern, TypeId expectedType) {
                            if (pattern.type != expectedType || !types.contains(pattern.type)) {
                                return false;
                            }
                            if (pattern.kind != PatternKind::enumCase) {
                                return pattern.payloads.empty() && !pattern.symbol.has_value() &&
                                        (pattern.kind != PatternKind::floatingLiteral ||
                                         validFloatingLiteral(types.at(pattern.type)->kind, pattern.text));
                            }
                            if (!pattern.symbol.has_value()) return false;
                            const auto* enumCase = enumCaseFor(expectedType, *pattern.symbol);
                            if (enumCase == nullptr ||
                                pattern.payloads.size() != enumCase->payloadTypes.size()) {
                                return false;
                            }
                            for (size_t payload = 0; payload < pattern.payloads.size(); ++payload) {
                                if (!verifyPattern(
                                            pattern.payloads[payload],
                                            enumCase->payloadTypes[payload])) {
                                    return false;
                                }
                            }
                            return true;
                        };
                        if (matches) {
                            for (const auto& switchCase : instruction.switchCases) {
                                matches &= verifyPattern(
                                        switchCase.pattern,
                                        operands[0]->type);
                            }
                        }
                        if (!matches) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "pattern switch cases do not match the scrutinee type");
                        }
                        break;
                    }
                    case Opcode::returnValue:
                        if (requireShape(1, 0) && operands[0] != nullptr &&
                            (!function.returnsValue ||
                             operands[0]->category != ValueCategory::value ||
                             operands[0]->type != function.resultType)) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "return value does not match function result type");
                        }
                        break;
                    case Opcode::returnVoid:
                        requireShape(0, 0);
                        if (function.returnsValue) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "value-returning function cannot return void");
                        }
                        break;
                    case Opcode::unreachable:
                        requireShape(0, 0);
                        break;
                }
            }

            if (block.instructions.empty() ||
                !isTerminator(block.instructions.back().opcode)) {
                report(
                        VerificationErrorId::missingTerminator,
                        functionId,
                        block.id,
                        std::nullopt,
                        "block does not end in a terminator");
            }
        }
    }

    return result;
}

bool isTerminator(Opcode opcode) {
    switch (opcode) {
        case Opcode::branch:
        case Opcode::conditionalBranch:
        case Opcode::switchPattern:
        case Opcode::returnValue:
        case Opcode::returnVoid:
        case Opcode::unreachable:
            return true;
        default:
            return false;
    }
}

const char* opcodeName(Opcode opcode) {
    switch (opcode) {
        case Opcode::integerConstant: return "integer";
        case Opcode::floatingConstant: return "floating";
        case Opcode::booleanConstant: return "boolean";
        case Opcode::stringConstant: return "string";
        case Opcode::byteConstant: return "byte";
        case Opcode::unitConstant: return "unit";
        case Opcode::stackAllocate: return "alloc_stack";
        case Opcode::zeroInitialize: return "zero_init";
        case Opcode::load: return "load";
        case Opcode::store: return "store";
        case Opcode::copyValue: return "copy";
        case Opcode::take: return "take";
        case Opcode::destroy: return "destroy";
        case Opcode::add: return "add";
        case Opcode::subtract: return "sub";
        case Opcode::multiply: return "mul";
        case Opcode::divide: return "div";
        case Opcode::remainder: return "rem";
        case Opcode::less: return "lt";
        case Opcode::lessEqual: return "le";
        case Opcode::greater: return "gt";
        case Opcode::greaterEqual: return "ge";
        case Opcode::equal: return "eq";
        case Opcode::notEqual: return "ne";
        case Opcode::logicalAnd: return "and";
        case Opcode::bitAnd: return "bit_and";
        case Opcode::bitOr: return "bit_or";
        case Opcode::bitXor: return "bit_xor";
        case Opcode::shiftLeft: return "shl";
        case Opcode::shiftRight: return "shr";
        case Opcode::convertChecked: return "convert";
        case Opcode::convertWrapping: return "convert_wrapping";
        case Opcode::constantValue: return "constant";
        case Opcode::integerFits: return "fits";
        case Opcode::typeSize: return "type_size";
        case Opcode::typeAlignment: return "type_alignment";
        case Opcode::integerAddress: return "integer_address";
        case Opcode::pointerToInteger: return "pointer_to_integer";
        case Opcode::destroyRange: return "destroy_range";
        case Opcode::cloneAt: return "clone_at";
        case Opcode::cloneCallback: return "clone_callback";
        case Opcode::destroyCallback: return "destroy_callback";
        case Opcode::linearRangeGuard: return "linear_range_guard";
        case Opcode::slotAddress: return "slot_address";
        case Opcode::stackBlock: return "stack_block";
        case Opcode::call: return "call";
        case Opcode::functionAddress: return "function_address";
        case Opcode::callIndirect: return "call_indirect";
        case Opcode::constructStruct: return "construct_struct";
        case Opcode::constructDictionary: return "construct_dictionary";
        case Opcode::dictionarySet: return "dictionary_set";
        case Opcode::dictionaryGet: return "dictionary_get";
        case Opcode::fieldAddress: return "field_addr";
        case Opcode::extractField: return "extract_field";
        case Opcode::constructEnum: return "construct_enum";
        case Opcode::extractPayload: return "extract_payload";
        case Opcode::payloadAddress: return "payload_address";
        case Opcode::stringUtf8: return "string_utf8";
        case Opcode::count: return "count";
        case Opcode::subscript: return "subscript";
        case Opcode::subscriptAddress: return "subscript_addr";
        case Opcode::branch: return "br";
        case Opcode::conditionalBranch: return "cond_br";
        case Opcode::switchPattern: return "switch_pattern";
        case Opcode::returnValue: return "ret";
        case Opcode::returnVoid: return "ret_void";
        case Opcode::unreachable: return "unreachable";
    }
    return "unknown";
}

const char* verificationErrorName(VerificationErrorId id) {
    switch (id) {
        case VerificationErrorId::duplicateId: return "ir.duplicate-id";
        case VerificationErrorId::invalidReference: return "ir.invalid-reference";
        case VerificationErrorId::invalidInstruction: return "ir.invalid-instruction";
        case VerificationErrorId::missingTerminator: return "ir.missing-terminator";
        case VerificationErrorId::instructionAfterTerminator:
            return "ir.instruction-after-terminator";
        case VerificationErrorId::typeMismatch: return "ir.type-mismatch";
        case VerificationErrorId::callMismatch: return "ir.call-mismatch";
        case VerificationErrorId::invalidSourceLocation: return "ir.invalid-source-location";
    }
    return "ir.unknown";
}

std::string dump(const Module& module) {
    std::unordered_map<TypeId, const TypeName*> types;
    for (const auto& type : module.types) types.emplace(type.id, &type);

    std::vector<const TypeName*> sortedTypes;
    for (const auto& type : module.types) sortedTypes.push_back(&type);
    std::sort(sortedTypes.begin(), sortedTypes.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });

    std::vector<const Function*> sortedFunctions;
    for (const auto& function : module.functions) sortedFunctions.push_back(&function);
    std::sort(sortedFunctions.begin(), sortedFunctions.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });

    std::vector<const DebugScope*> sortedDebugScopes;
    for (const auto& scope : module.debugScopes) sortedDebugScopes.push_back(&scope);
    std::sort(
            sortedDebugScopes.begin(),
            sortedDebugScopes.end(),
            [](const auto* left, const auto* right) { return left->id < right->id; });

    std::vector<const DebugVariable*> sortedDebugVariables;
    for (const auto& variable : module.debugVariables) {
        sortedDebugVariables.push_back(&variable);
    }
    std::sort(
            sortedDebugVariables.begin(),
            sortedDebugVariables.end(),
            [](const auto* left, const auto* right) { return left->id < right->id; });

    std::vector<const StructureDefinition*> sortedStructures;
    for (const auto& structure : module.structures) sortedStructures.push_back(&structure);
    std::sort(
            sortedStructures.begin(),
            sortedStructures.end(),
            [](const auto* left, const auto* right) { return left->type < right->type; });

    std::vector<const EnumerationDefinition*> sortedEnumerations;
    for (const auto& enumeration : module.enumerations) {
        sortedEnumerations.push_back(&enumeration);
    }
    std::sort(
            sortedEnumerations.begin(),
            sortedEnumerations.end(),
            [](const auto* left, const auto* right) { return left->type < right->type; });

    std::ostringstream out;
    out << "module \"" << escape(module.sourceName) << "\" {\n";
    for (size_t index = 0; index < module.sourceFiles.size(); ++index) {
        const auto& source = module.sourceFiles[index];
        out << "  source #" << index << " \"" << escape(source.fileName)
            << "\" directory=\"" << escape(source.directory) << "\"\n";
    }
    for (const auto* type : sortedTypes) {
        out << "  type !" << type->id << " = \"" << escape(type->name) << "\"\n";
    }
    if (!sortedTypes.empty() &&
        (!sortedStructures.empty() || !sortedEnumerations.empty() ||
         !sortedFunctions.empty())) {
        out << '\n';
    }

    for (const auto* structure : sortedStructures) {
        out << "  struct !" << structure->type << " \"" << escape(structure->name)
            << "\" symbol#" << structure->symbol << " {";
        for (size_t index = 0; index < structure->fields.size(); ++index) {
            const auto& field = structure->fields[index];
            out << (index == 0 ? " " : ", ") << "\"" << escape(field.name) << "\": "
                << typeName(types, field.type) << " symbol#" << field.symbol;
            if (field.isMutable) out << " var";
        }
        out << (structure->fields.empty() ? "}" : " }");
        if (structure->deinitializer.has_value()) out << " deinit=@" << *structure->deinitializer;
        if (structure->role == StructureRole::array) out << " role=array";
        if (structure->role == StructureRole::dict) out << " role=dict";
        if (structure->role == StructureRole::dictEntry) out << " role=dict_entry";
        if (structure->copier.has_value()) out << " copier=@" << *structure->copier;
        out << '\n';
    }
    for (const auto* enumeration : sortedEnumerations) {
        out << "  enum !" << enumeration->type << " \"" << escape(enumeration->name)
            << "\" symbol#" << enumeration->symbol << " {";
        for (size_t caseIndex = 0; caseIndex < enumeration->cases.size(); ++caseIndex) {
            const auto& enumCase = enumeration->cases[caseIndex];
            out << (caseIndex == 0 ? " " : ", ") << "\"" << escape(enumCase.name)
                << "\" symbol#" << enumCase.symbol;
            if (!enumCase.payloadTypes.empty()) {
                out << '(';
                for (size_t payload = 0; payload < enumCase.payloadTypes.size(); ++payload) {
                    if (payload != 0) out << ", ";
                    out << typeName(types, enumCase.payloadTypes[payload]);
                }
                out << ')';
            }
        }
        out << (enumeration->cases.empty() ? "}" : " }");
        if (enumeration->role != EnumerationRole::none) {
            out << " role=" << (enumeration->role == EnumerationRole::optional ? "optional"
                                : enumeration->role == EnumerationRole::result ? "result" : "unknown") << '('
                << enumeration->firstRoleCase << ',' << enumeration->secondRoleCase << ')';
        }
        out << '\n';
    }
    for (const auto* scope : sortedDebugScopes) {
        out << "  debug_scope #" << scope->id << ' '
            << (scope->kind == DebugScopeKind::function ? "function" : "block")
            << " function=@" << scope->function;
        if (scope->parent.has_value()) out << " parent=#" << *scope->parent;
        if (scope->semanticScope.has_value()) {
            out << " semantic-scope#" << *scope->semanticScope;
        }
        out << " @" << scope->span.offset << ':' << scope->span.length;
        if (scope->span.sourceId != 0) out << " source#" << scope->span.sourceId;
        out << '\n';
    }
    for (const auto* variable : sortedDebugVariables) {
        out << "  debug_var #" << variable->id << ' ';
        switch (variable->kind) {
            case DebugVariableKind::parameter: out << "parameter"; break;
            case DebugVariableKind::local: out << "local"; break;
            case DebugVariableKind::patternBinding: out << "pattern"; break;
        }
        out << " \"" << escape(variable->name) << "\" function=@"
            << variable->function << " scope=#" << variable->scope
            << " type=" << typeName(types, variable->type);
        if (variable->parameterIndex.has_value()) {
            out << " arg=" << *variable->parameterIndex;
        }
        if (variable->isMutable) out << " var";
        if (variable->symbol.has_value()) out << " symbol#" << *variable->symbol;
        out << " @" << variable->span.offset << ':' << variable->span.length;
        if (variable->span.sourceId != 0) out << " source#" << variable->span.sourceId;
        out << '\n';
    }
    if ((!sortedStructures.empty() || !sortedEnumerations.empty()) &&
        (!sortedDebugScopes.empty() || !sortedDebugVariables.empty() ||
         !sortedFunctions.empty())) {
        out << '\n';
    } else if ((!sortedDebugScopes.empty() || !sortedDebugVariables.empty()) &&
               !sortedFunctions.empty()) {
        out << '\n';
    }

    for (size_t functionIndex = 0; functionIndex < sortedFunctions.size(); ++functionIndex) {
        const auto& function = *sortedFunctions[functionIndex];
        out << "  " << (function.isExternal ? "extern " : "")
            << "func @" << function.id << " \"" << escape(function.name) << "\"(";
        for (size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0) out << ", ";
            const auto& parameter = function.parameters[index];
            out << valueName(parameter.value.id) << ": "
                << typeName(types, parameter.value.type)
                << " \"" << escape(parameter.name) << "\"";
            if (parameter.isInitializing) out << " initializing";
            else if (parameter.isMutable) out << " inout";
            if (parameter.isConsuming) out << " consuming";
            if (parameter.acceptsAnyType) out << " accepts-any";
        }
        out << ") -> " << typeName(types, function.resultType);
        if (function.nativeBinding.has_value()) {
            out << (function.nativeBinding->kind == NativeBindingKind::cImport
                            ? " c_import \"" : " c_export \"")
                << escape(function.nativeBinding->symbol) << '"';
            for (const auto pointer : function.nativeBinding->pointerParameters) out << " pointer#" << pointer;
        }
        if (function.isExternal) {
            out << "\n";
            continue;
        }
        if (function.debugScope.has_value()) out << " debug_scope#" << *function.debugScope;
        if (!function.isRootModule) out << " dependency";
        if (function.elementAddressor) out << " element_addressor";
        if (function.literalFactory) out << " literal_factory";
        if (function.alwaysInline) out << " always_inline";
        out << " {\n";
        for (const auto& binding : function.entryDebugVariableBindings) {
            out << "    debug_bind #" << binding.variable << " -> "
                << valueName(binding.address) << " entry\n";
        }

        std::vector<const BasicBlock*> sortedBlocks;
        for (const auto& block : function.blocks) sortedBlocks.push_back(&block);
        std::sort(sortedBlocks.begin(), sortedBlocks.end(), [](const auto* left, const auto* right) {
            return left->id < right->id;
        });
        for (const auto* block : sortedBlocks) {
            out << "    ^" << block->id << " \"" << escape(block->name) << "\":\n";
            for (const auto& instruction : block->instructions) {
                out << "      ";
                if (instruction.result.has_value()) {
                    out << valueName(instruction.result->id) << ": ";
                    if (instruction.result->category == ValueCategory::address) out << '&';
                    out << typeName(types, instruction.result->type) << " = ";
                }
                out << opcodeName(instruction.opcode);
                switch (instruction.opcode) {
                    case Opcode::integerConstant:
                        if (instruction.result.has_value() && types.contains(instruction.result->type) &&
                                typing::isUnsignedInteger(types.at(instruction.result->type)->kind)) {
                            out << ' ' << static_cast<uint64_t>(instruction.integerValue);
                        } else {
                            out << ' ' << instruction.integerValue;
                        }
                        break;
                    case Opcode::booleanConstant:
                    case Opcode::byteConstant:
                        out << ' ' << instruction.integerValue;
                        break;
                    case Opcode::floatingConstant:
                        out << ' ' << instruction.text;
                        break;
                    case Opcode::constantValue:
                        out << " #" << instruction.integerValue;
                        break;
                    case Opcode::integerFits:
                        out << ' ' << valueName(instruction.operands[0]) << " in type "
                            << instruction.integerValue;
                        break;
                    case Opcode::typeSize:
                    case Opcode::typeAlignment:
                    case Opcode::cloneCallback:
                    case Opcode::destroyCallback:
                        out << " of type " << instruction.integerValue;
                        break;
                    case Opcode::destroyRange:
                    case Opcode::cloneAt:
                    case Opcode::slotAddress:
                    case Opcode::stackBlock:
                        out << " of type " << instruction.integerValue;
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            out << (index == 0 ? " " : ", ") << valueName(instruction.operands[index]);
                        }
                        break;
                    case Opcode::linearRangeGuard:
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            out << (index == 0 ? " " : ", ") << valueName(instruction.operands[index]);
                        }
                        break;
                    case Opcode::integerAddress:
                    case Opcode::pointerToInteger:
                        out << ' ' << valueName(instruction.operands[0]);
                        break;
                    case Opcode::stringConstant:
                        out << " \"" << escape(instruction.text) << "\"";
                        break;
                    case Opcode::stackAllocate:
                    case Opcode::returnVoid:
                    case Opcode::unitConstant:
                    case Opcode::unreachable:
                        break;
                    case Opcode::zeroInitialize:
                    case Opcode::load:
                    case Opcode::copyValue:
                    case Opcode::take:
                    case Opcode::destroy:
                    case Opcode::convertChecked:
                    case Opcode::convertWrapping:
                    case Opcode::returnValue:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        break;
                    case Opcode::store:
                    case Opcode::dictionarySet:
                    case Opcode::dictionaryGet:
                    case Opcode::add:
                    case Opcode::subtract:
                    case Opcode::multiply:
                    case Opcode::divide:
                    case Opcode::remainder:
                    case Opcode::less:
                    case Opcode::lessEqual:
                    case Opcode::greater:
                    case Opcode::greaterEqual:
                    case Opcode::equal:
                    case Opcode::notEqual:
                    case Opcode::logicalAnd:
                    case Opcode::bitAnd:
                    case Opcode::bitOr:
                    case Opcode::bitXor:
                    case Opcode::shiftLeft:
                    case Opcode::shiftRight:
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            out << (index == 0 ? " " : ", ")
                                << valueName(instruction.operands[index]);
                        }
                        break;
                    case Opcode::functionAddress:
                        out << " @" << instruction.callee.value_or(invalidFunctionId);
                        break;
                    case Opcode::callIndirect:
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            out << (index == 0 ? " " : index == 1 ? "(" : ", ") << valueName(instruction.operands[index]);
                        }
                        out << (instruction.operands.size() <= 1 ? "()" : ")");
                        break;
                    case Opcode::call:
                        out << " @" << instruction.callee.value_or(invalidFunctionId) << '(';
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            if (index != 0) out << ", ";
                            out << valueName(instruction.operands[index]);
                        }
                        out << ')';
                        break;
                    case Opcode::constructStruct:
                    case Opcode::constructDictionary:
                    case Opcode::constructEnum:
                        out << '(';
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            if (index != 0) out << ", ";
                            out << valueName(instruction.operands[index]);
                        }
                        out << ')';
                        break;
                    case Opcode::fieldAddress:
                    case Opcode::extractField:
                    case Opcode::stringUtf8:
                    case Opcode::count:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        break;
                    case Opcode::payloadAddress:
                    case Opcode::extractPayload:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        out << ", " << instruction.integerValue;
                        break;
                    case Opcode::subscript:
                    case Opcode::subscriptAddress:
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            out << (index == 0 ? " " : ", ")
                                << valueName(instruction.operands[index]);
                        }
                        break;
                    case Opcode::branch:
                        if (!instruction.targets.empty()) out << " ^" << instruction.targets[0];
                        break;
                    case Opcode::conditionalBranch:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        for (const auto target : instruction.targets) out << ", ^" << target;
                        break;
                    case Opcode::switchPattern:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        out << " [";
                        for (size_t index = 0; index < instruction.switchCases.size(); ++index) {
                            if (index != 0) out << ", ";
                            out << patternText(instruction.switchCases[index].pattern, types)
                                << " -> ^" << instruction.switchCases[index].target;
                        }
                        out << ']';
                        break;
                }
                if (instruction.symbol.has_value()) out << " symbol#" << *instruction.symbol;
                if (instruction.debugLocation.has_value() &&
                    instruction.debugLocation->scope.has_value()) {
                    out << " scope#" << *instruction.debugLocation->scope;
                }
                out << " @" << instruction.span.offset << ':' << instruction.span.length;
                if (instruction.span.sourceId != 0) {
                    out << " source#" << instruction.span.sourceId;
                }
                out << '\n';
                for (const auto& binding : instruction.debugVariableBindings) {
                    out << "        debug_bind #" << binding.variable << " -> "
                        << valueName(binding.address) << '\n';
                }
            }
        }
        out << "  }\n";
        if (functionIndex + 1 < sortedFunctions.size()) out << '\n';
    }
    out << "}\n";
    return out.str();
}

std::string dump(const VerificationResult& result) {
    std::ostringstream out;
    for (const auto& error : result.errors) {
        out << verificationErrorName(error.id);
        if (error.function.has_value()) out << " function=" << *error.function;
        if (error.block.has_value()) out << " block=" << *error.block;
        if (error.instruction.has_value()) out << " instruction=" << *error.instruction;
        out << ": " << error.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::ir
