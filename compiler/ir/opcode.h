#ifndef __joyeer_ir_opcode_h__
#define __joyeer_ir_opcode_h__

// The IR instruction set. Opcode values are wire slots shared with the backend
// (compiler/backend/ir_wire.h): never renumber them, and keep removed slots unused.

namespace joyeer::ir {

enum class Opcode {
    integerConstant,
    booleanConstant,
    stringConstant,
    byteConstant,
    unitConstant,

    stackAllocate,
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
    less,
    lessEqual,
    greater,
    greaterEqual,
    equal,
    notEqual,
    logicalAnd,
    bitAnd,
    bitOr,
    bitXor,
    shiftLeft,
    shiftRight,
    convertChecked,
    convertWrapping,
    constantValue,
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
    returnValue,
    returnVoid,
    unreachable,
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
    // `cloneCallback` and `destroyCallback` yield the address of the type's clone or destroy helper as an
    // `Int`, or 0 when it has none; `linearRangeGuard` traps when its `count` operand is not zero.
    destroyRange,
    cloneAt,
    cloneCallback,
    destroyCallback,
    linearRangeGuard,
    // The address of element `index` of a run of slots of the type `integerValue` names, starting at `base`
    // (operands: base, index; an `Int` result): `base + index * size`, with no overflow or bounds check. Library
    // code checks the bounds first and keeps size computations for allocation checked (decision 0026).
    slotAddress,
    // Contiguous stack storage for a constant number of slots of the type `integerValue` names (operand: the
    // constant count; an `Int` address result, 0 for no storage). Allocated once per invocation and never
    // destroyed; runtime-only (decision 0026).
    stackBlock,
};

// Whether an instruction with `opcode` ends its block.
[[nodiscard]] bool isTerminator(Opcode opcode);
// Whether an instruction with `opcode` defines a result. `call` and `callIndirect` are not listed:
// whether they define one depends on the callee.
[[nodiscard]] bool producesValue(Opcode opcode);
// The opcode's spelling in IR dumps and diagnostics.
[[nodiscard]] const char* opcodeName(Opcode opcode);

} // namespace joyeer::ir

#endif
