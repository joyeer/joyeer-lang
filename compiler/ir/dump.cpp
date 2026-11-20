#include "dump.h"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joyeer::ir {

namespace {

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
                const auto* descriptor = findOpcodeDescriptor(instruction.opcode);
                const auto immediate = descriptor == nullptr ? OpcodeImmediate::none : descriptor->immediate;
                if (descriptor != nullptr && descriptor->modifier != OpcodeModifier::none) {
                    const auto* modifier = modifierName(descriptor->modifier, instruction.modifier);
                    if (modifier != nullptr) {
                        out << ' ' << modifier;
                    } else {
                        out << " modifier#" << static_cast<unsigned>(instruction.modifier);
                    }
                } else if (instruction.modifier != 0) {
                    // A stray modifier the verifier rejects; shown so the dump explains the rejection.
                    out << " modifier#" << static_cast<unsigned>(instruction.modifier);
                }
                const auto printOperands = [&] {
                    for (size_t index = 0; index < instruction.operands.size(); ++index) {
                        out << (index == 0 ? " " : ", ") << valueName(instruction.operands[index]);
                    }
                };
                switch (instruction.opcode) {
                    // Opcodes with a callee, a block target, cases, or a type after the operand.
                    case Opcode::integerFits:
                        printOperands();
                        out << " in type " << instruction.integerValue;
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
                    case Opcode::constructEnum:
                        out << '(';
                        for (size_t index = 0; index < instruction.operands.size(); ++index) {
                            if (index != 0) out << ", ";
                            out << valueName(instruction.operands[index]);
                        }
                        out << ')';
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
                    // Every other opcode: its immediate by kind, then its operands, then a payload index.
                    default:
                        switch (immediate) {
                            case OpcodeImmediate::integerLiteral:
                                if (instruction.result.has_value() && types.contains(instruction.result->type) &&
                                        typing::isUnsignedInteger(types.at(instruction.result->type)->kind)) {
                                    out << ' ' << static_cast<uint64_t>(instruction.integerValue);
                                } else {
                                    out << ' ' << instruction.integerValue;
                                }
                                break;
                            case OpcodeImmediate::floatingLiteral:
                                out << ' ' << instruction.text;
                                break;
                            case OpcodeImmediate::stringLiteral:
                                out << " \"" << escape(instruction.text) << "\"";
                                break;
                            case OpcodeImmediate::constantIndex:
                                out << " #" << instruction.integerValue;
                                break;
                            case OpcodeImmediate::type:
                                out << " of type " << instruction.integerValue;
                                break;
                            case OpcodeImmediate::none:
                            case OpcodeImmediate::payloadIndex:
                            case OpcodeImmediate::message:
                                break;
                        }
                        printOperands();
                        if (immediate == OpcodeImmediate::payloadIndex) {
                            out << ", " << instruction.integerValue;
                        } else if (immediate == OpcodeImmediate::message) {
                            out << ", \"" << escape(instruction.text) << '"';
                        }
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
