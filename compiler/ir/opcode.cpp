#include "opcode.h"

#include <array>
#include <iterator>
#include <cstddef>
#include <type_traits>

namespace joyeer::ir {

namespace {

using R = OpcodeResult;
using I = OpcodeImmediate;
using M = OpcodeModifier;
constexpr int variadic = variadicOperands;

// An opcode that always defines a result.
constexpr OpcodeDescriptor value(Opcode opcode, const char* name, int operands, I immediate = I::none,
                                 M modifier = M::none) {
    return { opcode, name, operands, 0, R::always, immediate, modifier, false, false };
}

// An opcode that never defines a result.
constexpr OpcodeDescriptor effect(Opcode opcode, const char* name, int operands, I immediate = I::none) {
    return { opcode, name, operands, 0, R::none, immediate, M::none, false, false };
}

// A call whose result `result` decides.
constexpr OpcodeDescriptor call(Opcode opcode, const char* name, R result) {
    return { opcode, name, variadic, 0, result, I::none, M::none, false, false };
}

constexpr OpcodeDescriptor terminator(Opcode opcode, const char* name, int operands, int targets) {
    return { opcode, name, operands, targets, R::none, I::none, M::none, true, false };
}

constexpr OpcodeDescriptor runtimeOnly(OpcodeDescriptor descriptor) {
    descriptor.runtimeOnly = true;
    return descriptor;
}

// One row per live opcode, in declaration (and wire slot) order.
constexpr std::array descriptors {
    // Constants.
    value(Opcode::literal, "literal", 0, I::integerLiteral),
    value(Opcode::stringConstant, "string", 0, I::stringLiteral),

    // Storage and ownership.
    value(Opcode::stackAllocate, "alloc_stack", 0),
    effect(Opcode::zeroInitialize, "zero_init", 1),
    value(Opcode::load, "load", 1),
    effect(Opcode::store, "store", 2),
    value(Opcode::copyValue, "copy", 1),
    value(Opcode::take, "take", 1),
    effect(Opcode::destroy, "destroy", 1),

    // Scalar operations.
    value(Opcode::add, "add", 2),
    value(Opcode::subtract, "sub", 2),
    value(Opcode::multiply, "mul", 2),
    value(Opcode::divide, "div", 2),
    value(Opcode::remainder, "rem", 2),
    value(Opcode::compare, "compare", 2, I::none, M::comparePredicate),
    value(Opcode::bitAnd, "bit_and", 2),
    value(Opcode::bitOr, "bit_or", 2),
    value(Opcode::bitXor, "bit_xor", 2),
    value(Opcode::shiftLeft, "shl", 2),
    value(Opcode::shiftRight, "shr", 2),
    value(Opcode::convert, "convert", 1, I::none, M::conversionMode),
    value(Opcode::constantValue, "constant", 0, I::constantIndex),
    value(Opcode::integerFits, "fits", 1, I::type),

    // Calls and aggregates.
    call(Opcode::call, "call", R::directCallee),
    value(Opcode::constructStruct, "construct_struct", variadic),
    value(Opcode::fieldAddress, "field_addr", 1),
    value(Opcode::extractField, "extract_field", 1),
    value(Opcode::constructEnum, "construct_enum", variadic),
    value(Opcode::extractPayload, "extract_payload", 1, I::payloadIndex),
    value(Opcode::payloadAddress, "payload_address", 1, I::payloadIndex),
    value(Opcode::stringUtf8, "string_utf8", 1),
    value(Opcode::count, "count", 1),
    value(Opcode::subscript, "subscript", 2),

    // Control flow.
    terminator(Opcode::branch, "br", 0, 1),
    terminator(Opcode::conditionalBranch, "cond_br", 1, 2),
    terminator(Opcode::switchPattern, "switch_pattern", 1, 0),
    terminator(Opcode::ret, "ret", variadic, 0),
    terminator(Opcode::unreachable, "unreachable", 0, 0),

    value(Opcode::floatingConstant, "floating", 0, I::floatingLiteral),

    // Function values.
    value(Opcode::functionAddress, "function_address", 0),
    call(Opcode::callIndirect, "call_indirect", R::functionType),

    // Runtime-only layout queries, raw addresses, collection capabilities and traps.
    runtimeOnly(value(Opcode::typeSize, "type_size", 0, I::type)),
    runtimeOnly(value(Opcode::typeAlignment, "type_alignment", 0, I::type)),
    runtimeOnly(value(Opcode::integerAddress, "integer_address", 1)),
    runtimeOnly(value(Opcode::pointerToInteger, "pointer_to_integer", 1)),
    runtimeOnly(effect(Opcode::destroyRange, "destroy_range", 2, I::type)),
    runtimeOnly(effect(Opcode::cloneAt, "clone_at", 2, I::type)),
    runtimeOnly(value(Opcode::typeHelper, "type_helper", 0, I::type, M::typeHelperKind)),
    runtimeOnly(effect(Opcode::trapIf, "trap_if", 1, I::message)),
    runtimeOnly(value(Opcode::slotAddress, "slot_address", 2, I::type)),
    runtimeOnly(value(Opcode::stackBlock, "stack_block", 1, I::type)),
};

constexpr const char* predicateNames[] = { "lt", "le", "gt", "ge", "eq", "ne" };
constexpr const char* conversionNames[] = { "checked", "wrapping" };
constexpr const char* helperNames[] = { "clone", "destroy" };

using Slot = std::underlying_type_t<Opcode>;

constexpr Slot slot(Opcode opcode) {
    return static_cast<Slot>(opcode);
}

// Rows in strictly increasing slot order: each opcode has at most one row, and the last row bounds the slots.
constexpr bool rowsAreOrdered() {
    for (size_t index = 1; index < descriptors.size(); ++index) {
        if (slot(descriptors[index - 1].opcode) >= slot(descriptors[index].opcode)) return false;
    }
    return slot(descriptors.front().opcode) >= 0;
}
static_assert(rowsAreOrdered(), "opcode rows must follow wire slot order, one row per opcode");

constexpr size_t slotCount = static_cast<size_t>(slot(descriptors.back().opcode)) + 1;

// The row index of each wire slot, or -1 for a removed slot.
constexpr auto rowBySlot = [] {
    std::array<int, slotCount> rows {};
    rows.fill(-1);
    for (size_t index = 0; index < descriptors.size(); ++index) {
        rows[static_cast<size_t>(slot(descriptors[index].opcode))] = static_cast<int>(index);
    }
    return rows;
}();

} // namespace

std::span<const OpcodeDescriptor> opcodeDescriptors() {
    return descriptors;
}

const OpcodeDescriptor* findOpcodeDescriptor(Opcode opcode) {
    const auto value = slot(opcode);
    if (value < 0 || static_cast<size_t>(value) >= slotCount) return nullptr;
    const auto row = rowBySlot[static_cast<size_t>(value)];
    return row < 0 ? nullptr : &descriptors[static_cast<size_t>(row)];
}

bool producesValue(Opcode opcode) {
    const auto* descriptor = findOpcodeDescriptor(opcode);
    return descriptor != nullptr && descriptor->result == R::always;
}

bool isTerminator(Opcode opcode) {
    const auto* descriptor = findOpcodeDescriptor(opcode);
    return descriptor != nullptr && descriptor->terminator;
}

const char* opcodeName(Opcode opcode) {
    const auto* descriptor = findOpcodeDescriptor(opcode);
    return descriptor == nullptr ? "unknown" : descriptor->name;
}

unsigned modifierCount(OpcodeModifier kind) {
    switch (kind) {
        case M::none: return 0;
        case M::comparePredicate: return static_cast<unsigned>(std::size(predicateNames));
        case M::conversionMode: return static_cast<unsigned>(std::size(conversionNames));
        case M::typeHelperKind: return static_cast<unsigned>(std::size(helperNames));
    }
    return 0;
}

const char* modifierName(OpcodeModifier kind, uint8_t modifier) {
    if (modifier >= modifierCount(kind)) return nullptr;
    switch (kind) {
        case M::none: return nullptr;
        case M::comparePredicate: return predicateNames[modifier];
        case M::conversionMode: return conversionNames[modifier];
        case M::typeHelperKind: return helperNames[modifier];
    }
    return nullptr;
}

} // namespace joyeer::ir
