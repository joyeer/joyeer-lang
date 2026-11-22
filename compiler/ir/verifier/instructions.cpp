// Instruction checks. verifyInstruction() runs the checks every instruction shares, then
// dispatches on the opcode to one check method. This file holds the dispatcher and the checks
// for constants, storage, operators and terminators; aggregates.cpp, calls.cpp and
// capabilities.cpp hold the rest. Each check method reports and returns; none stops verification.

#include "internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joyeer::ir::verifier {

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

// The type checker's pattern matrix (`patternMatrixIsExhaustive` in compiler/frontend/typechecking.cpp)
// over IR patterns of verified types: whether the rows cover every tuple of values of the column types.
// A wildcard covers its column; `true` and `false` together cover `Bool`; one case pattern per case
// covers an enum when the rows of each case cover its payloads. Other literals cover nothing alone, an
// uninhabited payload still needs a pattern, and only a wildcard covers an enum without cases.
class SwitchCoverage {
public:
    using Row = std::vector<const Pattern*>;  // null is a wildcard that specialization put in for a payload

    SwitchCoverage(const std::unordered_map<TypeId, const TypeName*>& types,
                   const std::unordered_map<TypeId, const EnumerationDefinition*>& enumerations):
            types(types), enumerations(enumerations) {}

    bool covers(const std::vector<TypeId>& columns, const std::vector<Row>& rows, size_t depth = 0) {
        if (rows.empty()) return false;
        if (columns.empty() || std::any_of(rows.begin(), rows.end(), [](const Row& row) {
                return std::all_of(row.begin(), row.end(), isWildcard);
            })) {
            return true;
        }
        // Past the budget or the depth the answer is "covered", so a switch is rejected only for a value that
        // no case matches. The type checker runs this algorithm on the same patterns (`nil` is `.None`, cases
        // in declaration order) with a smaller budget, so every lowered `match` is settled before this point;
        // the budget only guards other producers.
        if (remaining == 0 || depth == 256) return true;
        --remaining;

        // The rows whose first pattern is a wildcard or one that `admits`, with the first column replaced by
        // one column per payload: a wildcard row gets a wildcard per payload, an admitted row its payloads.
        const auto specialize = [&](const auto& admits, const std::vector<TypeId>& payloadTypes) {
            auto specializedColumns = payloadTypes;
            specializedColumns.insert(specializedColumns.end(), columns.begin() + 1, columns.end());
            std::vector<Row> specializedRows;
            for (const auto& row : rows) {
                Row specialized;
                if (isWildcard(row.front())) {
                    specialized.assign(payloadTypes.size(), nullptr);
                } else if (admits(*row.front()) && row.front()->payloads.size() == payloadTypes.size()) {
                    for (const auto& payload : row.front()->payloads) specialized.push_back(&payload);
                } else {
                    continue;
                }
                specialized.insert(specialized.end(), row.begin() + 1, row.end());
                specializedRows.push_back(std::move(specialized));
            }
            return covers(specializedColumns, specializedRows, depth + 1);
        };

        const auto none = [](const Pattern&) { return false; };
        const auto startsWithWildcard = [](const Row& row) { return isWildcard(row.front()); };
        if (std::any_of(rows.begin(), rows.end(), startsWithWildcard) && specialize(none, {})) return true;
        if (std::all_of(rows.begin(), rows.end(), startsWithWildcard)) return false;

        const auto type = types.find(columns.front());
        if (type != types.end() && type->second->kind == typing::TypeKind::boolean) {
            const auto literal = [](int64_t value) {
                return [value](const Pattern& pattern) {
                    return pattern.kind == PatternKind::booleanLiteral && pattern.integerValue == value;
                };
            };
            return specialize(literal(1), {}) && specialize(literal(0), {});
        }
        const auto enumeration = enumerations.find(columns.front());
        if (enumeration == enumerations.end() || enumeration->second->cases.empty()) return false;
        for (const auto& enumCase : enumeration->second->cases) {
            const auto ofCase = [&enumCase](const Pattern& pattern) {
                return pattern.kind == PatternKind::enumCase && pattern.symbol == enumCase.symbol;
            };
            if (!specialize(ofCase, enumCase.payloadTypes)) return false;
        }
        return true;
    }

private:
    static bool isWildcard(const Pattern* pattern) {
        return pattern == nullptr || pattern->kind == PatternKind::wildcard;
    }

    const std::unordered_map<TypeId, const TypeName*>& types;
    const std::unordered_map<TypeId, const EnumerationDefinition*>& enumerations;
    size_t remaining = size_t { 1 } << 16;
};

} // namespace

// The checks every instruction shares: a known opcode, nothing after the terminator, no dictionary
// entry value, a result exactly when the opcode defines one, known operands and targets, and the
// opcode's fixed operand and target counts. Then the opcode's own check. The opcode's descriptor
// (opcode.h) states its counts, result rule and runtime authorization.
void FunctionVerifier::verifyInstruction(const BasicBlock& block, size_t index, bool& foundTerminator) {
    const auto& instruction = block.instructions[index];
    const auto location = std::optional<size_t>(index);
    const auto* descriptor = findOpcodeDescriptor(instruction.opcode);
    if (descriptor == nullptr) {
        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
               "instruction has an unknown opcode");
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
            case Opcode::typeHelper:
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
    const auto resultRule = descriptor == nullptr ? OpcodeResult::none : descriptor->result;
    const auto expectsResult = resultRule == OpcodeResult::directCallee
            ? knownCallee && functions.at(*instruction.callee)->returnsValue
            : resultRule == OpcodeResult::functionType
            // Depends on the callee's function type, checked with the instruction.
            ? instruction.result.has_value()
            : resultRule == OpcodeResult::always;
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

    // A variadic opcode's own check matches its operands against the callee, function type or definition.
    bool shapeMatches = true;
    if (descriptor != nullptr && descriptor->operands != variadicOperands) {
        shapeMatches = instruction.operands.size() == static_cast<size_t>(descriptor->operands) &&
                instruction.targets.size() == static_cast<size_t>(descriptor->targets);
        if (!shapeMatches) {
            report(
                    VerificationErrorId::invalidInstruction,
                    functionId,
                    block.id,
                    location,
                    std::string(descriptor->name) +
                            " expects " + std::to_string(descriptor->operands) +
                            " operand(s) and " + std::to_string(descriptor->targets) +
                            " target(s)");
        }
    }
    if (descriptor != nullptr) {
        const auto count = modifierCount(descriptor->modifier);
        if (count == 0 ? instruction.modifier != 0 : instruction.modifier >= count) {
            report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
                   std::string(descriptor->name) + " has an invalid modifier " +
                           std::to_string(instruction.modifier));
        }
    }
    const bool unauthorized = descriptor != nullptr && descriptor->runtimeOnly && !function.runtimeContext;

    const InstructionSite site { block, index, instruction, location, std::move(operands), knownCallee,
                                 shapeMatches, unauthorized };
    switch (instruction.opcode) {
        // Constants (this file).
        case Opcode::literal: verifyLiteral(site); break;
        case Opcode::floatingConstant:
        case Opcode::stringConstant: verifyScalarConstant(site); break;
        case Opcode::constantValue: verifyConstantValue(site); break;

        // Storage and ownership (this file).
        case Opcode::stackAllocate: verifyStackAllocate(site); break;
        case Opcode::zeroInitialize: verifyZeroInitialize(site); break;
        case Opcode::load: verifyLoad(site); break;
        case Opcode::store: verifyStore(site); break;
        case Opcode::copyValue: verifyCopy(site); break;
        case Opcode::take: verifyTake(site); break;
        case Opcode::destroy: verifyDestroy(site); break;

        // Operators (this file).
        case Opcode::add:
        case Opcode::subtract:
        case Opcode::multiply:
        case Opcode::compare: verifyBinaryOperator(site); break;
        case Opcode::divide:
        case Opcode::remainder: verifyDivision(site); break;
        case Opcode::bitAnd:
        case Opcode::bitOr:
        case Opcode::bitXor:
        case Opcode::shiftLeft:
        case Opcode::shiftRight: verifyBitwiseOperator(site); break;
        case Opcode::convert: verifyConversion(site); break;
        case Opcode::integerFits: verifyIntegerFits(site); break;

        // Calls and function values (calls.cpp).
        case Opcode::call: verifyCall(site); break;
        case Opcode::functionAddress: verifyFunctionAddress(site); break;
        case Opcode::callIndirect: verifyIndirectCall(site); break;

        // Structs and enums (aggregates.cpp).
        case Opcode::constructStruct: verifyConstructStruct(site); break;
        case Opcode::fieldAddress:
        case Opcode::extractField: verifyFieldProjection(site); break;
        case Opcode::constructEnum: verifyConstructEnum(site); break;
        case Opcode::extractPayload: verifyExtractPayload(site); break;
        case Opcode::payloadAddress: verifyPayloadAddress(site); break;

        // Runtime-only layout queries, collection capabilities and raw addresses (capabilities.cpp).
        case Opcode::typeSize:
        case Opcode::typeAlignment: verifyLayoutQuery(site); break;
        case Opcode::destroyRange:
        case Opcode::cloneAt:
        case Opcode::slotAddress:
        case Opcode::stackBlock:
        case Opcode::typeHelper: verifyCollectionCapability(site); break;
        case Opcode::trapIf: verifyTrapIf(site); break;
        case Opcode::integerAddress: verifyIntegerAddress(site); break;
        case Opcode::pointerToInteger: verifyPointerToInteger(site); break;

        // Terminators (this file).
        // The table's shape check is the whole check of `branch` and `unreachable`.
        case Opcode::branch: break;
        case Opcode::conditionalBranch: verifyConditionalBranch(site); break;
        case Opcode::switchPattern: verifySwitchPattern(site); break;
        case Opcode::ret: verifyReturn(site); break;
        case Opcode::unreachable: break;
    }
}

// Constants.

// An integer, byte or Boolean value that fits its result type, or the unit value 0 of `Void`.
void FunctionVerifier::verifyLiteral(const InstructionSite& site) {
    const auto& instruction = site.instruction;
    // A missing result is the result rule's report; an unknown type is the type table's.
    if (!instruction.result.has_value() || !types.contains(instruction.result->type)) return;
    const auto* type = types.at(instruction.result->type);
    bool valid = instruction.result->category == ValueCategory::value;
    if (valid) {
        const auto kind = type->kind;
        const auto value = instruction.integerValue;
        if (typing::isInteger(kind)) {
            const auto bits = typing::scalarBitWidth(kind);
            valid = bits == 64 || (typing::isSignedInteger(kind)
                    ? value >= -(int64_t{1} << (bits - 1)) && value < (int64_t{1} << (bits - 1))
                    : static_cast<uint64_t>(value) < (uint64_t{1} << bits));
        } else if (kind == typing::TypeKind::boolean) {
            valid = value == 0 || value == 1;
        } else {
            valid = kind == typing::TypeKind::voidType && value == 0;
        }
    }
    if (!valid) {
        report(VerificationErrorId::typeMismatch, functionId, site.block.id, site.location,
               "literal kind or value does not match its result type");
    }
}

// A floating or String constant fits its result type.
void FunctionVerifier::verifyScalarConstant(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
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
            case Opcode::floatingConstant:
                valid = validFloatingLiteral(kind, instruction.text);
                break;
            case Opcode::stringConstant: valid = kind == typing::TypeKind::string; break;
            default: break;
        }
        if (!valid) {
            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                    "constant kind or value does not match its result type");
        }
    }
}

// A reference to a read-only module constant.
void FunctionVerifier::verifyConstantValue(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
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
}

// Storage and ownership.

void FunctionVerifier::verifyStackAllocate(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    if (instruction.result.has_value() &&
        instruction.result->category != ValueCategory::address) {
        report(
                VerificationErrorId::invalidInstruction,
                functionId,
                block.id,
                location,
                "stack allocation result must be an address");
    }
}

void FunctionVerifier::verifyZeroInitialize(const InstructionSite& site) {
    const auto& block = site.block;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
        operands[0]->category != ValueCategory::address) {
        report(
                VerificationErrorId::typeMismatch,
                functionId,
                block.id,
                location,
                "zero initialization requires addressable storage");
    }
}

void FunctionVerifier::verifyLoad(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

void FunctionVerifier::verifyStore(const InstructionSite& site) {
    const auto& block = site.block;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

// A copy of a copyable value (decision 0015).
void FunctionVerifier::verifyCopy(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

// A move out of storage, which leaves it destroyable.
void FunctionVerifier::verifyTake(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

// A destroy of storage whose type needs one (destruction.h).
void FunctionVerifier::verifyDestroy(const InstructionSite& site) {
    const auto& block = site.block;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
        (operands[0]->category != ValueCategory::address ||
         !destruction.requiresDestruction(operands[0]->type))) {
        report(
                VerificationErrorId::typeMismatch,
                functionId,
                block.id,
                location,
                "destroy requires addressable nontrivial storage");
    }
}

// Operators.

void FunctionVerifier::verifyDivision(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

void FunctionVerifier::verifyBitwiseOperator(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

void FunctionVerifier::verifyConversion(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

void FunctionVerifier::verifyIntegerFits(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
}

// Arithmetic and comparison: matching value operands.
void FunctionVerifier::verifyBinaryOperator(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
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
        // Only scalars (decision 0028 stage 3): String concatenation and comparison are prelude calls, so these
        // opcodes never allocate or produce an owned value.
        const auto* operand = types.contains(operands[0]->type) ? types.at(operands[0]->type) : nullptr;
        if (isArithmetic && categoriesAreValues && operandTypesMatch &&
            (operand == nullptr || (!typing::isInteger(operand->kind) && !typing::isFloating(operand->kind)))) {
            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                   std::string(opcodeName(instruction.opcode)) + " takes integer or floating operands");
        }
        if (instruction.opcode == Opcode::compare && categoriesAreValues && operandTypesMatch) {
            const auto* result = types.contains(instruction.result->type)
                    ? types.at(instruction.result->type) : nullptr;
            const bool comparable = operand != nullptr &&
                    (typing::isInteger(operand->kind) || typing::isFloating(operand->kind) ||
                     operand->kind == typing::TypeKind::boolean);
            if (!comparable || result == nullptr || result->kind != typing::TypeKind::boolean) {
                report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                       "compare takes scalar operands and yields a Bool");
            }
        }
    }
}

// Terminators.

void FunctionVerifier::verifyConditionalBranch(const InstructionSite& site) {
    const auto& block = site.block;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (site.shapeMatches && operands[0] != nullptr &&
        operands[0]->category != ValueCategory::value) {
        report(
                VerificationErrorId::typeMismatch,
                functionId,
                block.id,
                location,
                "conditional branch condition must be an object value");
    }
}

// Every case pattern matches the scrutinee's type, recursively through enum payloads; a top-level
// wildcard is the last case; and the cases cover every value of the scrutinee (docs/impl/ir.md section 4).
void FunctionVerifier::verifySwitchPattern(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto shapeMatches = site.shapeMatches;
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
        const auto* enumCase = context.enumCaseFor(expectedType, *pattern.symbol);
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
        return;
    }

    // The backend tests the cases in order, branches to the first wildcard without testing the rest, and
    // ends the tests with `unreachable`.
    const auto& cases = instruction.switchCases;
    const auto wildcard = std::find_if(cases.begin(), cases.end(), [](const SwitchCase& switchCase) {
        return switchCase.pattern.kind == PatternKind::wildcard;
    });
    if (wildcard != cases.end() && std::next(wildcard) != cases.end()) {
        report(VerificationErrorId::unreachableSwitchCase, functionId, block.id, location,
               "pattern switch case " + std::to_string(std::distance(cases.begin(), wildcard) + 1) +
                       " is a wildcard, so the cases after it can never be chosen");
    }
    std::vector<SwitchCoverage::Row> rows;
    for (const auto& switchCase : cases) rows.push_back({ &switchCase.pattern });
    if (!SwitchCoverage(types, context.enumerations).covers({ operands[0]->type }, rows)) {
        report(VerificationErrorId::nonExhaustiveSwitch, functionId, block.id, location,
               "pattern switch cases do not cover every value of the scrutinee; end them with a wildcard case");
    }
}

// One operand of the function's result type in a value-returning function, none otherwise.
void FunctionVerifier::verifyReturn(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    if (operands.size() > 1 || !instruction.targets.empty()) {
        report(VerificationErrorId::invalidInstruction, functionId, block.id, location,
               "ret takes one operand in a value-returning function and none otherwise");
    } else if (operands.empty()) {
        if (function.returnsValue) {
            report(VerificationErrorId::typeMismatch, functionId, block.id, location,
                   "value-returning function cannot return void");
        }
    } else if (operands[0] != nullptr &&
               (!function.returnsValue || operands[0]->category != ValueCategory::value ||
                operands[0]->type != function.resultType)) {
        report(VerificationErrorId::typeMismatch, functionId, block.id, location,
               "return value does not match function result type");
    }
}

} // namespace joyeer::ir::verifier
