#ifndef __joyeer_ir_opcode_h__
#define __joyeer_ir_opcode_h__

// The IR instruction set. Opcode values are wire slots shared with the backend
// (compiler/backend/ir_wire.h): never renumber them, and keep removed slots unused.
// Each live opcode has one row in the descriptor table below (decision 0028).

#include <cstdint>
#include <span>

namespace joyeer::ir {

enum class Opcode {
    // An integer, byte, Boolean or unit value from `integerValue`, read in the result type (decision 0028). Wire
    // slots 1, 3 and 4 belonged to the merged Boolean, byte and unit constants.
    literal = 0,
    stringConstant = 2,

    stackAllocate = 5,
    zeroInitialize,
    load,
    store,
    copyValue,
    take,
    destroy,

    add,
    subtract,
    multiply,
    divide,
    remainder,
    // A comparison by the `ComparePredicate` modifier. Wire slots 18-22 belonged to the merged predicates.
    compare,
    // Wire slot 23 belonged to `logicalAnd`, which lowering never emitted: `&&` and `||` lower to branches.
    bitAnd = 24,
    bitOr,
    bitXor,
    shiftLeft,
    shiftRight,
    // An integer conversion by the `ConversionMode` modifier: checked (traps out of range) or wrapping. Wire slot
    // 30 belonged to the merged wrapping conversion.
    convert,
    constantValue = 31,
    integerFits,

    call,
    constructStruct,
    // Wire slots 35-38 belonged to the removed builtin array operations, and 39-41 to the removed builtin
    // dictionary operations (decision 0027); the verifier rejects them.
    fieldAddress = 42,
    extractField,
    constructEnum,
    extractPayload,
    payloadAddress,
    stringUtf8,
    // The byte length of a String.
    count,
    // The byte of a String at an Int offset; traps out of range.
    subscript,
    // Wire slot 50 belonged to the removed `subscript_addr`, the builtin dictionary's element address.

    branch = 51,
    conditionalBranch,
    switchPattern,
    // A return: one operand in a value-returning function, none otherwise. Wire slot 55 belonged to the merged
    // `ret_void`.
    ret,
    unreachable = 56,
    floatingConstant,
    // Function values (decision 0020). `functionAddress` yields `callee` as a value of its function type;
    // `callIndirect` calls operand 0, a function value, with the remaining operands passed by the
    // type's parameter effects.
    functionAddress,
    callIndirect,
    // Layout intrinsics (spec 20.12.5). `typeSize` and `typeAlignment` yield an `Int` for the type
    // `integerValue` names; `integerAddress` turns an `Int` operand into the address of a slot of the
    // result's type.
    typeSize,
    typeAlignment,
    integerAddress,
    // Runtime-only conversion of a typed slot or function address to the scalar native ABI.
    pointerToInteger,
    // Collection capabilities (decision 0025), all runtime-only and all for the type `integerValue`
    // names. `destroyRange` destroys the `count` live elements at an address (operands: address, count);
    // `cloneAt` copies the value at the second operand into the uninitialized slot at the first;
    // `typeHelper` yields the address of the type's clone or destroy helper (the `TypeHelperKind` modifier) as an
    // `Int`, or 0 when it has none. Wire slot 67 belonged to the merged destroy callback.
    destroyRange,
    cloneAt,
    typeHelper,
    // Runtime-only: traps with the message in `text` when its Bool operand is true.
    trapIf = 68,
    // The address of element `index` of a run of slots of the type `integerValue` names, starting at `base`
    // (operands: base, index; an `Int` result): `base + index * size`, with no overflow or bounds check. Library
    // code checks the bounds first and keeps size computations for allocation checked (decision 0026).
    slotAddress,
    // Contiguous stack storage for a constant number of slots of the type `integerValue` names (operand: the
    // constant count; an `Int` address result, 0 for no storage). Allocated once per invocation and never
    // destroyed; runtime-only (decision 0026).
    stackBlock,
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

// Every live opcode, one row each, in declaration order.
[[nodiscard]] std::span<const OpcodeDescriptor> opcodeDescriptors();
// The row of `opcode`, or null for a removed or unknown wire slot.
[[nodiscard]] const OpcodeDescriptor* findOpcodeDescriptor(Opcode opcode);

// Whether an instruction with `opcode` ends its block.
[[nodiscard]] bool isTerminator(Opcode opcode);
// Whether an instruction with `opcode` always defines a result. False for `call` and `callIndirect`, whose
// result depends on the callee.
[[nodiscard]] bool producesValue(Opcode opcode);
// The opcode's spelling in IR dumps and diagnostics, or "unknown" for a removed or unknown slot.
[[nodiscard]] const char* opcodeName(Opcode opcode);
// The number of valid modifier values of `kind`; 0 for `none`.
[[nodiscard]] unsigned modifierCount(OpcodeModifier kind);
// The spelling of `modifier` in IR dumps, or null when it is not valid for `kind`.
[[nodiscard]] const char* modifierName(OpcodeModifier kind, uint8_t modifier);

} // namespace joyeer::ir

#endif
