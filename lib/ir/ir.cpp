#include "joyeer/ir/ir.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace joyeer::ir {

namespace {

bool producesValue(Opcode opcode) {
    switch (opcode) {
        case Opcode::integerConstant:
        case Opcode::booleanConstant:
        case Opcode::stringConstant:
        case Opcode::byteConstant:
        case Opcode::stackAllocate:
        case Opcode::load:
        case Opcode::copyValue:
        case Opcode::take:
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
        case Opcode::constructStruct:
        case Opcode::constructArray:
        case Opcode::constructDictionary:
        case Opcode::fieldAddress:
        case Opcode::extractField:
        case Opcode::constructEnum:
        case Opcode::extractPayload:
        case Opcode::count:
        case Opcode::subscript:
        case Opcode::subscriptAddress:
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
            out << pattern.integerValue;
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

bool requiresDestruction(const Module& module, TypeId requestedType) {
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

    std::unordered_map<TypeId, bool> memo;
    std::unordered_set<TypeId> visiting;
    std::function<bool(TypeId)> visit = [&](TypeId id) {
        const auto cached = memo.find(id);
        if (cached != memo.end()) return cached->second;
        const auto found = types.find(id);
        if (found == types.end()) return false;
        if (!visiting.insert(id).second) return true;

        bool result = false;
        switch (found->second->kind) {
            case typing::TypeKind::string:
            case typing::TypeKind::array:
            case typing::TypeKind::dictionary:
                result = true;
                break;
            case typing::TypeKind::structure: {
                const auto structure = structures.find(id);
                if (structure != structures.end()) {
                    result = std::any_of(
                            structure->second->fields.begin(),
                            structure->second->fields.end(),
                            [&visit](const auto& field) { return visit(field.type); });
                }
                break;
            }
            case typing::TypeKind::enumeration:
            case typing::TypeKind::optional:
            case typing::TypeKind::result: {
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
        memo.emplace(id, result);
        return result;
    };
    return visit(requestedType);
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

    if (module.sourceInfo.has_value()) {
        const auto& source = *module.sourceInfo;
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
        if (!module.sourceInfo.has_value()) {
            return;
        }
        const auto& source = *module.sourceInfo;
        const auto end = static_cast<uint64_t>(location.span.offset) +
                static_cast<uint64_t>(location.span.length);
        if (location.span.offset > source.byteLength || end > source.byteLength) {
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
    }

    std::unordered_map<TypeId, const StructureDefinition*> structures;
    std::unordered_map<semantic::SymbolId, const FieldDefinition*> fields;
    for (const auto& structure : module.structures) {
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
        for (const auto& field : structure.fields) {
            if (!types.contains(field.type)) {
                report(
                        VerificationErrorId::invalidReference,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        "field '" + field.name + "' references an unknown type");
            }
            if (!fields.emplace(field.symbol, &field).second) {
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

    for (const auto& function : module.functions) {
        const auto functionId = std::optional<FunctionId>(function.id);
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

        for (const auto& block : function.blocks) {
            bool foundTerminator = false;
            for (size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                const auto location = std::optional<size_t>(index);
                if (foundTerminator) {
                    report(
                            VerificationErrorId::instructionAfterTerminator,
                            functionId,
                            block.id,
                            location,
                            "instruction appears after block terminator");
                }
                foundTerminator |= isTerminator(instruction.opcode);

                const auto knownCallee = instruction.callee.has_value() &&
                        functions.contains(*instruction.callee);
                const auto expectsResult = instruction.opcode == Opcode::call
                        ? knownCallee && functions.at(*instruction.callee)->returnsValue
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
                    case Opcode::integerConstant:
                    case Opcode::booleanConstant:
                    case Opcode::stringConstant:
                    case Opcode::byteConstant:
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
                        break;
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
                             !requiresDestruction(module, operands[0]->type))) {
                            report(
                                    VerificationErrorId::typeMismatch,
                                    functionId,
                                    block.id,
                                    location,
                                    "destroy requires addressable nontrivial storage");
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
                        case Opcode::constructArray: {
                        const auto* resultType = instruction.result.has_value() &&
                            types.contains(instruction.result->type)
                            ? types.at(instruction.result->type)
                            : nullptr;
                        bool matches = resultType != nullptr &&
                            resultType->kind == typing::TypeKind::array &&
                            resultType->arguments.size() == 1 &&
                            instruction.result->category == ValueCategory::value &&
                            instruction.targets.empty();
                        if (resultType != nullptr && resultType->arguments.size() == 1) {
                            for (const auto* operand : operands) {
                                matches &= operand != nullptr &&
                                        operand->category == ValueCategory::value &&
                                        operand->type == resultType->arguments[0];
                            }
                        }
                        if (!matches) {
                            report(
                                VerificationErrorId::typeMismatch,
                                functionId,
                                block.id,
                                location,
                                "array construction elements do not match its element type");
                        }
                        break;
                        }
                    case Opcode::arrayAppend:
                        if (requireShape(2, 0) && operands[0] != nullptr &&
                            operands[1] != nullptr) {
                            const auto found = types.find(operands[0]->type);
                            const auto* arrayType = found == types.end()
                                    ? nullptr
                                    : found->second;
                            if (operands[0]->category != ValueCategory::address ||
                                operands[1]->category != ValueCategory::value ||
                                arrayType == nullptr ||
                                arrayType->kind != typing::TypeKind::array ||
                                arrayType->arguments.size() != 1 ||
                                arrayType->arguments[0] != operands[1]->type) {
                                report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "array append requires an array address and matching element value");
                            }
                        }
                        break;
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
                                    return candidate.symbol == *instruction.symbol;
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
                                         baseType->kind == typing::TypeKind::array ||
                                         baseType->kind == typing::TypeKind::dictionary) &&
                                        resultType->kind == typing::TypeKind::integer;
                                if (!matches) {
                                    report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "count requires String/Array/Dict and produces Int");
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
                                    } else if (baseType->kind == typing::TypeKind::array &&
                                               baseType->arguments.size() == 1) {
                                        const auto intType = std::find_if(
                                                module.types.begin(),
                                                module.types.end(),
                                                [](const auto& type) {
                                                    return type.kind == typing::TypeKind::integer;
                                                });
                                        if (intType != module.types.end()) expectedIndex = intType->id;
                                        expectedResult = baseType->arguments[0];
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
                                return pattern.payloads.empty() && !pattern.symbol.has_value();
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
        case Opcode::booleanConstant: return "boolean";
        case Opcode::stringConstant: return "string";
        case Opcode::byteConstant: return "byte";
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
        case Opcode::less: return "lt";
        case Opcode::lessEqual: return "le";
        case Opcode::greater: return "gt";
        case Opcode::greaterEqual: return "ge";
        case Opcode::equal: return "eq";
        case Opcode::notEqual: return "ne";
        case Opcode::logicalAnd: return "and";
        case Opcode::call: return "call";
        case Opcode::constructStruct: return "construct_struct";
        case Opcode::constructArray: return "construct_array";
        case Opcode::arrayAppend: return "array_append";
        case Opcode::constructDictionary: return "construct_dictionary";
        case Opcode::dictionarySet: return "dictionary_set";
        case Opcode::fieldAddress: return "field_addr";
        case Opcode::extractField: return "extract_field";
        case Opcode::constructEnum: return "construct_enum";
        case Opcode::extractPayload: return "extract_payload";
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
        out << (structure->fields.empty() ? "}\n" : " }\n");
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
        out << (enumeration->cases.empty() ? "}\n" : " }\n");
    }
    for (const auto* scope : sortedDebugScopes) {
        out << "  debug_scope #" << scope->id << ' '
            << (scope->kind == DebugScopeKind::function ? "function" : "block")
            << " function=@" << scope->function;
        if (scope->parent.has_value()) out << " parent=#" << *scope->parent;
        if (scope->semanticScope.has_value()) {
            out << " semantic-scope#" << *scope->semanticScope;
        }
        out << " @" << scope->span.offset << ':' << scope->span.length << '\n';
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
        out << " @" << variable->span.offset << ':' << variable->span.length << '\n';
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
        if (function.isExternal) {
            out << "\n";
            continue;
        }
        if (function.debugScope.has_value()) out << " debug_scope#" << *function.debugScope;
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
                    case Opcode::booleanConstant:
                    case Opcode::byteConstant:
                        out << ' ' << instruction.integerValue;
                        break;
                    case Opcode::stringConstant:
                        out << " \"" << escape(instruction.text) << "\"";
                        break;
                    case Opcode::stackAllocate:
                    case Opcode::returnVoid:
                    case Opcode::unreachable:
                        break;
                    case Opcode::zeroInitialize:
                    case Opcode::load:
                    case Opcode::copyValue:
                    case Opcode::take:
                    case Opcode::destroy:
                    case Opcode::returnValue:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        break;
                    case Opcode::store:
                    case Opcode::arrayAppend:
                    case Opcode::dictionarySet:
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
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            out << (index == 0 ? " " : ", ")
                                << valueName(instruction.operands[index]);
                        }
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
                    case Opcode::constructArray:
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
                    case Opcode::count:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        break;
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
                out << " @" << instruction.span.offset << ':' << instruction.span.length << '\n';
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
