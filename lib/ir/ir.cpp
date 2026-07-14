#include "joyeer/ir/ir.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

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

} // namespace

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
            if (parameter.value.category != ValueCategory::value) {
                report(
                        VerificationErrorId::invalidInstruction,
                        functionId,
                        std::nullopt,
                        std::nullopt,
                        "parameter '" + parameter.name + "' must be an object value");
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
                                    operands[argument]->category == ValueCategory::value &&
                                    operands[argument]->type ==
                                            callee.parameters[argument].value.type;
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
        case Opcode::branch: return "br";
        case Opcode::conditionalBranch: return "cond_br";
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

    std::ostringstream out;
    out << "module \"" << escape(module.sourceName) << "\" {\n";
    for (const auto* type : sortedTypes) {
        out << "  type !" << type->id << " = \"" << escape(type->name) << "\"\n";
    }
    if (!sortedTypes.empty() && !sortedFunctions.empty()) out << '\n';

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
                    case Opcode::branch:
                        if (!instruction.targets.empty()) out << " ^" << instruction.targets[0];
                        break;
                    case Opcode::conditionalBranch:
                        if (!instruction.operands.empty()) {
                            out << ' ' << valueName(instruction.operands[0]);
                        }
                        for (const auto target : instruction.targets) out << ", ^" << target;
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
