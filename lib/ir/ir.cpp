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

    for (const auto& function : module.functions) {
        const auto functionId = std::optional<FunctionId>(function.id);
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
            if (!values.emplace(parameter.value.id, parameter.value).second) {
                report(
                        VerificationErrorId::duplicateId,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "duplicate value id " + std::to_string(parameter.value.id));
            }
        }

        if (function.isExternal) {
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
                                         baseType->kind == typing::TypeKind::array) &&
                                        resultType->kind == typing::TypeKind::integer;
                                if (!matches) {
                                    report(
                                        VerificationErrorId::typeMismatch,
                                        functionId,
                                        block.id,
                                        location,
                                        "count requires String/Array and produces Int");
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
        case Opcode::constructDictionary: return "construct_dictionary";
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
    if ((!sortedStructures.empty() || !sortedEnumerations.empty()) &&
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
            if (parameter.isMutable) out << " inout";
            if (parameter.acceptsAnyType) out << " accepts-any";
        }
        out << ") -> " << typeName(types, function.resultType);
        if (function.isExternal) {
            out << "\n";
            continue;
        }
        out << " {\n";

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
                out << " @" << instruction.span.offset << ':' << instruction.span.length << '\n';
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
