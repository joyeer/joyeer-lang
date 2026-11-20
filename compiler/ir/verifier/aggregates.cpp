// Checks of struct and enum instructions against the module's definitions.

#include "internal.h"

#include <algorithm>

namespace joyeer::ir::verifier {

// A struct value built from one value per field, in declaration order.
void FunctionVerifier::verifyConstructStruct(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
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
}

// `fieldAddress` and `extractField`: the field exists, with the matching access category.
void FunctionVerifier::verifyFieldProjection(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto shapeMatches = site.shapeMatches;
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
}

// An enum value of one case, built from one value per payload.
void FunctionVerifier::verifyConstructEnum(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto* enumCase = instruction.result.has_value() &&
            instruction.symbol.has_value()
            ? context.enumCaseFor(instruction.result->type, *instruction.symbol)
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
}

// Payload `integerValue` of an enum value of the case `symbol`, as a value.
void FunctionVerifier::verifyExtractPayload(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto shapeMatches = site.shapeMatches;
    const auto* enumCase = shapeMatches && operands[0] != nullptr &&
            instruction.symbol.has_value()
            ? context.enumCaseFor(operands[0]->type, *instruction.symbol)
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
}

// The address-category twin of verifyExtractPayload: payload storage inside enum storage.
void FunctionVerifier::verifyPayloadAddress(const InstructionSite& site) {
    const auto& block = site.block;
    const auto& instruction = site.instruction;
    const auto location = site.location;
    const auto& operands = site.operands;
    const auto shapeMatches = site.shapeMatches;
    const auto* enumCase = shapeMatches && operands[0] != nullptr &&
            instruction.symbol.has_value()
            ? context.enumCaseFor(operands[0]->type, *instruction.symbol)
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
}

} // namespace joyeer::ir::verifier
