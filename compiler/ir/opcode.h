#ifndef __joyeer_ir_opcode_h__
#define __joyeer_ir_opcode_h__

// The IR instruction set. Opcode values are dense and follow the descriptor table below, one row per opcode
// (decision 0028); they are wire values shared with the backend (compiler/backend/ir_wire.h). Adding, removing or
// reordering an opcode changes the values, so it bumps `wire::version`.

#include <cstdint>
#include <span>

namespace joyeer::ir {

enum class Opcode {
    // Constants. `literal` is an integer, byte, Boolean or unit value from `integerValue`, read in the result type;
    // `constantValue` names a module constant.
    literal,
    floatingConstant,
    stringConstant,
    constantValue,

    // Storage and ownership.
    stackAllocate,
    zeroInitialize,
    load,
    store,
    copyValue,
    take,
    destroy,

    // Scalar operations. `compare` takes a `ComparePredicate` modifier and `convert` a `ConversionMode`: checked
    // (traps out of range) or wrapping.
    add,
    subtract,
    multiply,
    divide,
    remainder,
    compare,
    bitAnd,
    bitOr,
    bitXor,
    shiftLeft,
    shiftRight,
    convert,
    integerFits,

    // Aggregates and Strings. `count` is the byte length of a String; `subscript` is the byte of a String at an Int
    // offset and traps out of range.
    constructStruct,
    fieldAddress,
    extractField,
    constructEnum,
    extractPayload,
    payloadAddress,
    stringUtf8,
    count,
    subscript,

    // Calls and function values (decision 0020). `functionAddress` yields `callee` as a value of its function type;
    // `callIndirect` calls operand 0, a function value, with the remaining operands passed by the type's parameter
    // effects.
    call,
    functionAddress,
    callIndirect,

    // Control flow. `ret` takes one operand in a value-returning function and none otherwise.
    branch,
    conditionalBranch,
    switchPattern,
    ret,
    unreachable,

    // Runtime-only memory operations.
    // Layout intrinsics (spec 20.12.5). `typeSize` and `typeAlignment` yield an `Int` for the type
    // `integerValue` names; `integerAddress` turns an `Int` operand into the address of a slot of the
    // result's type.
    typeSize,
    typeAlignment,
    integerAddress,
    // Conversion of a typed slot or function address to the scalar native ABI.
    pointerToInteger,
    // Collection capabilities (decision 0025), all for the type `integerValue` names. `destroyRange` destroys the
    // `count` live elements at an address (operands: address, count); `cloneAt` copies the value at the second
    // operand into the uninitialized slot at the first; `typeHelper` yields the address of the type's clone or
    // destroy helper (the `TypeHelperKind` modifier) as an `Int`, or 0 when it has none.
    destroyRange,
    cloneAt,
    typeHelper,
    // Traps with the message in `text` when its Bool operand is true.
    trapIf,
    // The address of element `index` of a run of slots of the type `integerValue` names, starting at `base`
    // (operands: base, index; an `Int` result): `base + index * size`, with no overflow or bounds check. Library
    // code checks the bounds first and keeps size computations for allocation checked (decision 0026).
    slotAddress,
    // Contiguous stack storage for a constant number of slots of the type `integerValue` names (operand: the
    // constant count; an `Int` address result, 0 for no storage). Allocated once per invocation and never
    // destroyed (decision 0026).
    stackBlock,
    // Keep `stackBlock` last: the table's static checks (opcode.cpp) bound the values by it. Add opcodes in their
    // group instead.
};

// The `compare` predicates. Floating-point `notEqual` is unordered (true for NaN); the others are ordered.
enum class ComparePredicate : uint8_t {
    less,
    lessEqual,
    greater,
    greaterEqual,
    equal,
    notEqual,
};

// The `convert` modes.
enum class ConversionMode : uint8_t {
    checked,
    wrapping,
};

// The helpers `typeHelper` yields.
enum class TypeHelperKind : uint8_t {
    clone,
    destroy,
};

// When an instruction defines a result.
enum class OpcodeResult : uint8_t {
    none,
    always,
    // When the direct callee returns a value (`call`).
    directCallee,
    // When the function type of operand 0 returns a value (`callIndirect`); its own check decides.
    functionType,
};

// What `Instruction::integerValue` or `Instruction::text` holds for an opcode. The `callee`, `symbol` and
// `switchCases` fields are not immediates: they are named by the instruction's own check.
enum class OpcodeImmediate : uint8_t {
    none,
    // `integerValue` is the literal, read in the result type (signed or unsigned).
    integerLiteral,
    // `text` is the decimal spelling of a Float32 or Float64 literal.
    floatingLiteral,
    // `text` is the String literal's bytes.
    stringLiteral,
    // `integerValue` is a `TypeId`.
    type,
    // `integerValue` indexes `Module::constants`.
    constantIndex,
    // `integerValue` indexes the payloads of the enum case `symbol`.
    payloadIndex,
    // `text` is a trap message.
    message,
};

// What `Instruction::modifier` holds for an opcode. It is 0 for an opcode without one.
enum class OpcodeModifier : uint8_t {
    none,
    comparePredicate,
    conversionMode,
    typeHelperKind,
};

// `OpcodeDescriptor::operands` of an opcode whose operand count its own check decides.
inline constexpr int variadicOperands = -1;

// The shape of one opcode. Before it runs the opcode's own check, the verifier checks the operand and target
// counts of a fixed-arity row and the result rule, and marks a runtime-only opcode outside a runtime function;
// a variadic opcode's check matches its own operands and targets, and each runtime-only check reports the mark.
struct OpcodeDescriptor {
    Opcode opcode;
    // The spelling in IR dumps and diagnostics.
    const char* name;
    // The operand count, or `variadicOperands`.
    int operands;
    // The block target count. Pattern switch targets live in `switchCases`, not here.
    int targets;
    OpcodeResult result;
    OpcodeImmediate immediate;
    OpcodeModifier modifier;
    // Whether the instruction ends its block.
    bool terminator;
    // Whether only runtime functions may use the opcode. The opcode's own check may admit verified literal
    // storage or a collection's element place outside them (verifier/capabilities.cpp).
    bool runtimeOnly;
};

// Every opcode, one row each, in declaration order.
[[nodiscard]] std::span<const OpcodeDescriptor> opcodeDescriptors();
// The row of `opcode`, or null for a value outside the table.
[[nodiscard]] const OpcodeDescriptor* findOpcodeDescriptor(Opcode opcode);

// Whether an instruction with `opcode` ends its block.
[[nodiscard]] bool isTerminator(Opcode opcode);
// Whether an instruction with `opcode` always defines a result. False for `call` and `callIndirect`, whose
// result depends on the callee.
[[nodiscard]] bool producesValue(Opcode opcode);
// The opcode's spelling in IR dumps and diagnostics, or "unknown" for a value outside the table.
[[nodiscard]] const char* opcodeName(Opcode opcode);
// The number of valid modifier values of `kind`; 0 for `none`.
[[nodiscard]] unsigned modifierCount(OpcodeModifier kind);
// The spelling of `modifier` in IR dumps, or null when it is not valid for `kind`.
[[nodiscard]] const char* modifierName(OpcodeModifier kind, uint8_t modifier);

} // namespace joyeer::ir

#endif
