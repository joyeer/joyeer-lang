#include "joyeer/backend/llvm.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace joyeer::llvmbackend {

namespace {

std::string escapeQuoted(std::string_view text) {
    std::ostringstream out;
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 0x20 && byte <= 0x7e && character != '"' && character != '\\') {
            out << character;
        } else {
            out << '\\' << std::uppercase << std::hex << std::setw(2)
                << std::setfill('0') << static_cast<unsigned int>(byte) << std::dec;
        }
    }
    return out.str();
}

std::string valueName(ir::ValueId value) {
    return "%v" + std::to_string(value);
}

std::string blockName(ir::BlockId block) {
    return "b" + std::to_string(block);
}

std::string functionName(const ir::Function& function) {
    return "@joyeer_fn_" + std::to_string(function.id);
}

class Builder {
public:
    Result build(const ir::Module& source) {
        module = &source;
        const auto verification = ir::Verifier().verify(source);
        if (!verification.succeeded()) {
            for (const auto& error : verification.errors) {
                report(
                        DiagnosticId::invalidModule,
                        {},
                        error.function,
                        error.block,
                        std::string(ir::verificationErrorName(error.id)) +
                                ": " + error.message);
            }
            return Result { {}, std::move(diagnostics) };
        }

        for (const auto& type : source.types) types.emplace(type.id, &type);
        for (const auto& function : source.functions) {
            functions.emplace(function.id, &function);
        }
        for (const auto& function : source.functions) {
            if (!function.isExternal) emitFunction(function);
        }
        if (!diagnostics.empty()) return Result { {}, std::move(diagnostics) };

        std::ostringstream out;
        out << "; Joyeer LLVM backend\n"
            << "source_filename = \"" << escapeQuoted(source.sourceName) << "\"\n";
        if (usesString) out << "%joyeer.string = type { ptr, i64 }\n";
        if (usesString || !stringGlobals.empty() || !runtimeDeclarations.empty()) {
            out << '\n';
        }
        for (const auto& global : stringGlobals) out << global << '\n';
        if (!stringGlobals.empty() && !runtimeDeclarations.empty()) out << '\n';

        std::vector<std::string> declarations(
                runtimeDeclarations.begin(),
                runtimeDeclarations.end());
        std::sort(declarations.begin(), declarations.end());
        for (const auto& declaration : declarations) out << declaration << '\n';
        if (!declarations.empty() && !functionBodies.empty()) out << '\n';
        for (size_t index = 0; index < functionBodies.size(); ++index) {
            out << functionBodies[index];
            if (index + 1 < functionBodies.size()) out << '\n';
        }
        return Result { out.str(), {} };
    }

private:
    const ir::Module* module = nullptr;
    const ir::Function* currentFunction = nullptr;
    const ir::BasicBlock* currentBlock = nullptr;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<ir::TypeId, const ir::TypeName*> types;
    std::unordered_map<ir::FunctionId, const ir::Function*> functions;
    std::unordered_map<ir::ValueId, ir::Value> values;
    std::unordered_map<ir::ValueId, std::string> operands;
    std::unordered_set<std::string> runtimeDeclarations;
    std::vector<std::string> stringGlobals;
    std::vector<std::string> functionBodies;
    size_t nextString = 0;
    size_t nextTemporary = 0;
    bool usesString = false;

    void report(
            DiagnosticId id,
            SourceSpan span,
            std::optional<ir::FunctionId> function,
            std::optional<ir::BlockId> block,
            std::string message) {
        diagnostics.push_back(Diagnostic {
            id,
            span,
            function,
            block,
            std::move(message),
        });
    }

    void reportHere(DiagnosticId id, SourceSpan span, std::string message) {
        report(
                id,
                span,
                currentFunction == nullptr
                        ? std::optional<ir::FunctionId>()
                        : std::optional<ir::FunctionId>(currentFunction->id),
                currentBlock == nullptr
                        ? std::optional<ir::BlockId>()
                        : std::optional<ir::BlockId>(currentBlock->id),
                std::move(message));
    }

    std::optional<std::string> llvmType(ir::TypeId id, SourceSpan span = {}) {
        const auto found = types.find(id);
        if (found == types.end()) {
            reportHere(
                    DiagnosticId::unsupportedType,
                    span,
                    "unknown Joyeer IR type " + std::to_string(id));
            return std::nullopt;
        }
        switch (found->second->kind) {
            case typing::TypeKind::voidType:
            case typing::TypeKind::never:
                return "void";
            case typing::TypeKind::integer:
                return "i64";
            case typing::TypeKind::boolean:
                return "i1";
            case typing::TypeKind::uint8:
                return "i8";
            case typing::TypeKind::string:
                usesString = true;
                return "%joyeer.string";
            default:
                reportHere(
                        DiagnosticId::unsupportedType,
                        span,
                        "LLVM lowering is not implemented for type '" +
                                found->second->name + "'");
                return std::nullopt;
        }
    }

    const ir::TypeName* type(ir::TypeId id) const {
        const auto found = types.find(id);
        return found == types.end() ? nullptr : found->second;
    }

    const ir::Value* value(ir::ValueId id) const {
        const auto found = values.find(id);
        return found == values.end() ? nullptr : &found->second;
    }

    std::optional<std::string> operand(ir::ValueId id) const {
        const auto found = operands.find(id);
        if (found == operands.end()) return std::nullopt;
        return found->second;
    }

    void emitFunction(const ir::Function& function) {
        currentFunction = &function;
        currentBlock = nullptr;
        values.clear();
        operands.clear();

        const auto resultType = llvmType(function.resultType);
        if (!resultType.has_value()) return;
        std::vector<std::string> parameterTypes;
        for (const auto& parameter : function.parameters) {
            const auto typeText = parameter.value.category == ir::ValueCategory::address
                    ? std::optional<std::string>("ptr")
                    : llvmType(parameter.value.type, parameter.span);
            if (!typeText.has_value()) return;
            parameterTypes.push_back(*typeText);
            values.emplace(parameter.value.id, parameter.value);
            operands.emplace(parameter.value.id, valueName(parameter.value.id));
        }
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!instruction.result.has_value()) continue;
                values.emplace(instruction.result->id, *instruction.result);
                operands.emplace(
                        instruction.result->id,
                        valueName(instruction.result->id));
            }
        }

        std::ostringstream out;
        out << "define " << *resultType << ' ' << functionName(function) << '(';
        for (size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0) out << ", ";
            out << parameterTypes[index] << ' '
                << valueName(function.parameters[index].value.id);
        }
        out << ") {\n";
        for (const auto& block : function.blocks) {
            currentBlock = &block;
            out << blockName(block.id) << ":\n";
            for (const auto& instruction : block.instructions) {
                if (!emitInstruction(out, instruction)) return;
            }
        }
        out << "}\n";
        functionBodies.push_back(out.str());
    }

    bool emitInstruction(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto unsupported = [&]() {
            reportHere(
                    DiagnosticId::unsupportedInstruction,
                    instruction.span,
                    std::string("LLVM lowering is not implemented for '") +
                            ir::opcodeName(instruction.opcode) + "'");
            return false;
        };
        const auto requiredOperand = [&](size_t index) -> std::optional<std::string> {
            if (index >= instruction.operands.size()) return std::nullopt;
            return operand(instruction.operands[index]);
        };

        switch (instruction.opcode) {
            case ir::Opcode::integerConstant:
            case ir::Opcode::byteConstant:
                operands[instruction.result->id] = std::to_string(instruction.integerValue);
                return true;
            case ir::Opcode::booleanConstant:
                operands[instruction.result->id] =
                        instruction.integerValue == 0 ? "false" : "true";
                return true;
            case ir::Opcode::stringConstant:
                return emitStringConstant(instruction);
            case ir::Opcode::stackAllocate: {
                const auto typeText = llvmType(instruction.result->type, instruction.span);
                if (!typeText.has_value()) return false;
                out << "  " << valueName(instruction.result->id)
                    << " = alloca " << *typeText << "\n";
                return true;
            }
            case ir::Opcode::load: {
                const auto address = requiredOperand(0);
                const auto typeText = llvmType(instruction.result->type, instruction.span);
                if (!address.has_value() || !typeText.has_value()) return false;
                out << "  " << valueName(instruction.result->id)
                    << " = load " << *typeText << ", ptr " << *address << "\n";
                return true;
            }
            case ir::Opcode::store:
                return emitStore(out, instruction);
            case ir::Opcode::add:
            case ir::Opcode::subtract:
            case ir::Opcode::multiply:
                return emitArithmetic(out, instruction);
            case ir::Opcode::less:
            case ir::Opcode::lessEqual:
            case ir::Opcode::greater:
            case ir::Opcode::greaterEqual:
            case ir::Opcode::equal:
            case ir::Opcode::notEqual:
                return emitComparison(out, instruction);
            case ir::Opcode::logicalAnd: {
                const auto left = requiredOperand(0);
                const auto right = requiredOperand(1);
                if (!left.has_value() || !right.has_value()) return false;
                out << "  " << valueName(instruction.result->id)
                    << " = and i1 " << *left << ", " << *right << "\n";
                return true;
            }
            case ir::Opcode::call:
                return emitCall(out, instruction);
            case ir::Opcode::branch:
                out << "  br label %" << blockName(instruction.targets[0]) << "\n";
                return true;
            case ir::Opcode::conditionalBranch: {
                const auto condition = requiredOperand(0);
                if (!condition.has_value()) return false;
                out << "  br i1 " << *condition << ", label %"
                    << blockName(instruction.targets[0]) << ", label %"
                    << blockName(instruction.targets[1]) << "\n";
                return true;
            }
            case ir::Opcode::returnValue:
                return emitReturn(out, instruction);
            case ir::Opcode::returnVoid:
                out << "  ret void\n";
                return true;
            case ir::Opcode::unreachable:
                out << "  unreachable\n";
                return true;
            default:
                return unsupported();
        }
    }

    bool emitStringConstant(const ir::Instruction& instruction) {
        const auto globalName = "@.joyeer.string." + std::to_string(nextString++);
        const auto length = instruction.text.size();
        std::ostringstream global;
        global << globalName << " = private unnamed_addr constant [" << length
               << " x i8] c\"" << escapeQuoted(instruction.text) << "\", align 1";
        stringGlobals.push_back(global.str());
        usesString = true;
        operands[instruction.result->id] =
                "%joyeer.string { ptr " + globalName + ", i64 " +
                std::to_string(length) + " }";
        return true;
    }

    bool emitStore(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto stored = operand(instruction.operands[0]);
        const auto address = operand(instruction.operands[1]);
        const auto* storedValue = value(instruction.operands[0]);
        if (!stored.has_value() || !address.has_value() || storedValue == nullptr) {
            return false;
        }
        const auto typeText = llvmType(storedValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  store " << *typeText << ' ' << *stored
            << ", ptr " << *address << "\n";
        return true;
    }

    bool emitArithmetic(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto left = operand(instruction.operands[0]);
        const auto right = operand(instruction.operands[1]);
        const auto* leftValue = value(instruction.operands[0]);
        if (!left.has_value() || !right.has_value() || leftValue == nullptr) return false;
        const auto* operandType = type(leftValue->type);
        const auto result = valueName(instruction.result->id);

        if (operandType != nullptr && operandType->kind == typing::TypeKind::integer) {
            std::string helper;
            switch (instruction.opcode) {
                case ir::Opcode::add: helper = "joyeer_checked_add_int"; break;
                case ir::Opcode::subtract: helper = "joyeer_checked_sub_int"; break;
                case ir::Opcode::multiply: helper = "joyeer_checked_mul_int"; break;
                default: return false;
            }
            runtimeDeclarations.insert("declare i64 @" + helper + "(i64, i64)");
            out << "  " << result << " = call i64 @" << helper
                << "(i64 " << *left << ", i64 " << *right << ")\n";
        } else if (operandType != nullptr &&
                   operandType->kind == typing::TypeKind::string &&
                   instruction.opcode == ir::Opcode::add) {
            usesString = true;
            runtimeDeclarations.insert(
                    "declare %joyeer.string @joyeer_string_concat(%joyeer.string, %joyeer.string)");
            out << "  " << result
                << " = call %joyeer.string @joyeer_string_concat(%joyeer.string "
                << *left << ", %joyeer.string " << *right << ")\n";
        } else {
            reportHere(
                    DiagnosticId::unsupportedInstruction,
                    instruction.span,
                    "unsupported arithmetic operand type");
            return false;
        }
        return true;
    }

    bool emitComparison(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto left = operand(instruction.operands[0]);
        const auto right = operand(instruction.operands[1]);
        const auto* leftValue = value(instruction.operands[0]);
        if (!left.has_value() || !right.has_value() || leftValue == nullptr) return false;
        const auto* operandType = type(leftValue->type);
        const auto result = valueName(instruction.result->id);

        if (operandType != nullptr && operandType->kind == typing::TypeKind::string) {
            return emitStringComparison(out, instruction, *left, *right);
        }
        const auto typeText = llvmType(leftValue->type, instruction.span);
        if (!typeText.has_value() || operandType == nullptr) return false;
        out << "  " << result << " = icmp "
            << comparisonPredicate(
                    instruction.opcode,
                    operandType->kind == typing::TypeKind::integer)
            << ' ' << *typeText << ' ' << *left << ", " << *right << "\n";
        return true;
    }

    bool emitStringComparison(
            std::ostringstream& out,
            const ir::Instruction& instruction,
            const std::string& left,
            const std::string& right) {
        usesString = true;
        const auto result = valueName(instruction.result->id);
        if (instruction.opcode == ir::Opcode::equal ||
            instruction.opcode == ir::Opcode::notEqual) {
            runtimeDeclarations.insert(
                    "declare i1 @joyeer_string_equal(%joyeer.string, %joyeer.string)");
            const auto equal = instruction.opcode == ir::Opcode::equal
                    ? result
                    : "%tmp" + std::to_string(nextTemporary++);
            out << "  " << equal
                << " = call i1 @joyeer_string_equal(%joyeer.string " << left
                << ", %joyeer.string " << right << ")\n";
            if (instruction.opcode == ir::Opcode::notEqual) {
                out << "  " << result << " = xor i1 " << equal << ", true\n";
            }
            return true;
        }

        runtimeDeclarations.insert(
                "declare i64 @joyeer_string_compare(%joyeer.string, %joyeer.string)");
        const auto compared = "%tmp" + std::to_string(nextTemporary++);
        out << "  " << compared
            << " = call i64 @joyeer_string_compare(%joyeer.string " << left
            << ", %joyeer.string " << right << ")\n";
        out << "  " << result << " = icmp "
            << comparisonPredicate(instruction.opcode, true)
            << " i64 " << compared << ", 0\n";
        return true;
    }

    std::string comparisonPredicate(ir::Opcode opcode, bool isSigned) const {
        switch (opcode) {
            case ir::Opcode::less: return isSigned ? "slt" : "ult";
            case ir::Opcode::lessEqual: return isSigned ? "sle" : "ule";
            case ir::Opcode::greater: return isSigned ? "sgt" : "ugt";
            case ir::Opcode::greaterEqual: return isSigned ? "sge" : "uge";
            case ir::Opcode::equal: return "eq";
            case ir::Opcode::notEqual: return "ne";
            default: return "eq";
        }
    }

    bool emitCall(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto calleeFound = functions.find(*instruction.callee);
        if (calleeFound == functions.end()) return false;
        const auto& callee = *calleeFound->second;
        if (callee.isExternal) {
            if (callee.name == "print" && instruction.operands.size() == 1) {
                return emitPrint(out, instruction);
            }
            reportHere(
                    DiagnosticId::unsupportedExternal,
                    instruction.span,
                    "unsupported external function '" + callee.name + "'");
            return false;
        }

        const auto returnType = llvmType(callee.resultType, instruction.span);
        if (!returnType.has_value()) return false;
        out << "  ";
        if (instruction.result.has_value()) {
            out << valueName(instruction.result->id) << " = ";
        }
        out << "call " << *returnType << ' ' << functionName(callee) << '(';
        for (size_t index = 0; index < instruction.operands.size(); ++index) {
            if (index != 0) out << ", ";
            const auto argument = operand(instruction.operands[index]);
            const auto* argumentValue = value(instruction.operands[index]);
            if (!argument.has_value() || argumentValue == nullptr) return false;
            const auto argumentType = argumentValue->category == ir::ValueCategory::address
                    ? std::optional<std::string>("ptr")
                    : llvmType(argumentValue->type, instruction.span);
            if (!argumentType.has_value()) return false;
            out << *argumentType << ' ' << *argument;
        }
        out << ")\n";
        return true;
    }

    bool emitPrint(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto argument = operand(instruction.operands[0]);
        const auto* argumentValue = value(instruction.operands[0]);
        if (!argument.has_value() || argumentValue == nullptr) return false;
        const auto* argumentType = type(argumentValue->type);
        if (argumentType == nullptr) return false;

        std::string runtimeName;
        switch (argumentType->kind) {
            case typing::TypeKind::integer: runtimeName = "joyeer_print_int"; break;
            case typing::TypeKind::boolean: runtimeName = "joyeer_print_bool"; break;
            case typing::TypeKind::uint8: runtimeName = "joyeer_print_byte"; break;
            case typing::TypeKind::string: runtimeName = "joyeer_print_string"; break;
            default:
                reportHere(
                        DiagnosticId::unsupportedExternal,
                        instruction.span,
                        "print is not implemented for type '" + argumentType->name + "'");
                return false;
        }
        const auto typeText = llvmType(argumentValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        runtimeDeclarations.insert(
                "declare void @" + runtimeName + "(" + *typeText + ")");
        out << "  call void @" << runtimeName << '(' << *typeText << ' '
            << *argument << ")\n";
        return true;
    }

    bool emitReturn(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto returned = operand(instruction.operands[0]);
        const auto* returnedValue = value(instruction.operands[0]);
        if (!returned.has_value() || returnedValue == nullptr) return false;
        const auto typeText = llvmType(returnedValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  ret " << *typeText << ' ' << *returned << "\n";
        return true;
    }
};

} // namespace

Result Emitter::emit(const ir::Module& module) const {
    return Builder().build(module);
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::invalidModule: return "llvm.invalid-module";
        case DiagnosticId::unsupportedType: return "llvm.unsupported-type";
        case DiagnosticId::unsupportedInstruction: return "llvm.unsupported-instruction";
        case DiagnosticId::unsupportedExternal: return "llvm.unsupported-external";
    }
    return "llvm.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length;
        if (diagnostic.function.has_value()) out << " function=" << *diagnostic.function;
        if (diagnostic.block.has_value()) out << " block=" << *diagnostic.block;
        out << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::llvmbackend