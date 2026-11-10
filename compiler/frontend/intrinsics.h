#ifndef JOYEER_COMPILER_INTRINSICS_H
#define JOYEER_COMPILER_INTRINSICS_H

#include "typechecking.h"

#include <array>
#include <string_view>
#include <vector>

// Runtime-only memory operations (spec §19.4.5). They are ordinary `@c_extern`
// imports whose symbol is reserved: the backend lowers each to inline LLVM
// instead of a call, so authorization, signature checking, and IR transport
// are those of a native binding. An address is an `Int`.
namespace joyeer::intrinsic {

inline constexpr std::string_view prefix = "joyeer_intrinsic_";

// Slots in the table behind joyeer_intrinsic_static_slot, and the symbol every
// object defines (as a merged comdat) so that all units share one table.
inline constexpr int64_t staticSlotCount = 64;
inline constexpr std::string_view staticSlotSymbol = "joyeer_static_slots";

enum class Operation {
    addressOf,
    load8,
    load16,
    load32,
    load64,
    store8,
    store16,
    store32,
    store64,
    atomicAdd,
    atomicLoad,
    atomicExchange,
    atomicCompareExchange,
    callVoid1,
    callVoid2,
    literalAddress,
    symbolAddress,
    staticSlot,
    memoryCopy,
    memoryMove,
    memoryFill,
    doubleBits,
    floatBits,
    systemCall,
    targetArchitecture,
};

struct Descriptor {
    std::string_view symbol;
    Operation operation;
    typing::TypeKind result;
    std::array<typing::TypeKind, 7> parameters;
    size_t count;
    // The first parameter is a `[UInt8]` passed as a bare data pointer.
    bool bytePointer;
};

using typing::TypeKind;

inline constexpr std::array<Descriptor, 25> table {{
    { "joyeer_intrinsic_address_of", Operation::addressOf, TypeKind::integer,
      { TypeKind::array, TypeKind::error }, 1, true },
    { "joyeer_intrinsic_load_u8", Operation::load8, TypeKind::uint8,
      { TypeKind::integer, TypeKind::error }, 1, false },
    { "joyeer_intrinsic_load_u16", Operation::load16, TypeKind::uint16,
      { TypeKind::integer, TypeKind::error }, 1, false },
    { "joyeer_intrinsic_load_u32", Operation::load32, TypeKind::uint32,
      { TypeKind::integer, TypeKind::error }, 1, false },
    { "joyeer_intrinsic_load_u64", Operation::load64, TypeKind::uint64,
      { TypeKind::integer, TypeKind::error }, 1, false },
    { "joyeer_intrinsic_store_u8", Operation::store8, TypeKind::voidType,
      { TypeKind::integer, TypeKind::uint8 }, 2, false },
    { "joyeer_intrinsic_store_u16", Operation::store16, TypeKind::voidType,
      { TypeKind::integer, TypeKind::uint16 }, 2, false },
    { "joyeer_intrinsic_store_u32", Operation::store32, TypeKind::voidType,
      { TypeKind::integer, TypeKind::uint32 }, 2, false },
    { "joyeer_intrinsic_store_u64", Operation::store64, TypeKind::voidType,
      { TypeKind::integer, TypeKind::uint64 }, 2, false },
    { "joyeer_intrinsic_atomic_add", Operation::atomicAdd, TypeKind::integer,
      { TypeKind::integer, TypeKind::integer }, 2, false },
    // The other sequentially consistent atomics (spec §19.4.5) a runtime lock needs: a load, an
    // exchange that returns the previous value, and a compare-and-swap that returns the value it
    // found. The address must be 8-byte aligned.
    { "joyeer_intrinsic_atomic_load", Operation::atomicLoad, TypeKind::integer,
      { TypeKind::integer, TypeKind::error }, 1, false },
    { "joyeer_intrinsic_atomic_exchange", Operation::atomicExchange, TypeKind::integer,
      { TypeKind::integer, TypeKind::integer }, 2, false },
    { "joyeer_intrinsic_atomic_cmpxchg", Operation::atomicCompareExchange, TypeKind::integer,
      { TypeKind::integer, TypeKind::integer, TypeKind::integer }, 3, false },
    // Indirect calls (spec §19.4.6): the first argument is the address of a C
    // function returning void that takes one or two address arguments. The
    // runtime uses them for the generated element clone and destroy functions.
    { "joyeer_intrinsic_call_void1", Operation::callVoid1, TypeKind::voidType,
      { TypeKind::integer, TypeKind::integer, TypeKind::error }, 2, false },
    { "joyeer_intrinsic_call_void2", Operation::callVoid2, TypeKind::voidType,
      { TypeKind::integer, TypeKind::integer, TypeKind::integer }, 3, false },
    // The address of the NUL-terminated constant data of a string literal given
    // as the argument (spec §19.4.6), for messages a core unit cannot build as a
    // String. The argument must be a literal; no String value is created.
    { "joyeer_intrinsic_literal_address", Operation::literalAddress, TypeKind::integer,
      { TypeKind::string, TypeKind::error, TypeKind::error }, 1, false },
    // The address of the function or data symbol named by the string literal given
    // as the argument (spec §19.4.6): a runtime function stored as a callback. The
    // symbol is resolved at link time; nothing checks that it exists or its type.
    { "joyeer_intrinsic_symbol_address", Operation::symbolAddress, TypeKind::integer,
      { TypeKind::string, TypeKind::error, TypeKind::error }, 1, false },
    // The address of zero-initialized 8-byte slot `index` of a table of
    // staticSlotCount slots shared by every unit in the program (spec §19.4.6).
    { "joyeer_intrinsic_static_slot", Operation::staticSlot, TypeKind::integer,
      { TypeKind::integer, TypeKind::error, TypeKind::error }, 1, false },
    // Bulk memory (spec §19.4.5). `count` is a byte count taken as unsigned; the
    // ranges are not checked. memcpy requires the ranges not to overlap.
    { "joyeer_intrinsic_memcpy", Operation::memoryCopy, TypeKind::voidType,
      { TypeKind::integer, TypeKind::integer, TypeKind::integer }, 3, false },
    { "joyeer_intrinsic_memmove", Operation::memoryMove, TypeKind::voidType,
      { TypeKind::integer, TypeKind::integer, TypeKind::integer }, 3, false },
    { "joyeer_intrinsic_memset", Operation::memoryFill, TypeKind::voidType,
      { TypeKind::integer, TypeKind::uint8, TypeKind::integer }, 3, false },
    // The IEEE 754 bit pattern of a Double or a Float, for formatting without a C library.
    { "joyeer_intrinsic_double_bits", Operation::doubleBits, TypeKind::uint64,
      { TypeKind::float64, TypeKind::error, TypeKind::error }, 1, false },
    { "joyeer_intrinsic_float_bits", Operation::floatBits, TypeKind::uint32,
      { TypeKind::float32, TypeKind::error, TypeKind::error }, 1, false },
    // A Linux system call (spec §19.4.6): the number, then six arguments, all addresses or
    // integers. The result is the kernel's: -4095 to -1 is a negated errno. Accepted only when
    // the target is Linux on x86-64 or AArch64; the runtime owns the call numbers.
    { "joyeer_intrinsic_syscall", Operation::systemCall, TypeKind::integer,
      { TypeKind::integer, TypeKind::integer, TypeKind::integer, TypeKind::integer,
        TypeKind::integer, TypeKind::integer, TypeKind::integer }, 7, false },
    // 1 for an x86-64 target, 2 for AArch64, 0 for any other: a constant, so the runtime
    // can pick the system call numbers of the architecture it was built for.
    { "joyeer_intrinsic_target_architecture", Operation::targetArchitecture, TypeKind::integer,
      { TypeKind::error, TypeKind::error, TypeKind::error }, 0, false },
}};

[[nodiscard]] inline bool reserved(std::string_view symbol) {
    return symbol.substr(0, prefix.size()) == prefix;
}

// Whether the intrinsic reads its string argument at compile time, so the argument
// must be a literal and never becomes a String value.
[[nodiscard]] inline bool readsLiteral(std::string_view symbol) {
    return symbol == "joyeer_intrinsic_literal_address" || symbol == "joyeer_intrinsic_symbol_address";
}

[[nodiscard]] inline const Descriptor* find(std::string_view symbol) {
    for (const auto& descriptor : table) {
        if (descriptor.symbol == symbol) return &descriptor;
    }
    return nullptr;
}

// Whether a declaration's parameter and result kinds, which parameters are bare
// byte pointers, and which are `inout`, are exactly what the intrinsic requires.
// The byte pointer must be `inout`: a borrowed array may live in read-only
// memory (a constant), and a store through its address would then be undefined.
[[nodiscard]] inline bool matches(
        const Descriptor& descriptor,
        TypeKind result,
        const std::vector<TypeKind>& parameters,
        const std::vector<bool>& pointers,
        const std::vector<bool>& mutables) {
    if (result != descriptor.result || parameters.size() != descriptor.count ||
        pointers.size() != parameters.size() || mutables.size() != parameters.size()) {
        return false;
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (parameters[index] != descriptor.parameters[index]) return false;
        if (pointers[index] != (index == 0 && descriptor.bytePointer)) return false;
        if (pointers[index] && !mutables[index]) return false;
    }
    return true;
}

} // namespace joyeer::intrinsic

#endif
