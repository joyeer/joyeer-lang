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
};

struct Descriptor {
    std::string_view symbol;
    Operation operation;
    typing::TypeKind result;
    std::array<typing::TypeKind, 2> parameters;
    size_t count;
    // The first parameter is a `[UInt8]` passed as a bare data pointer.
    bool bytePointer;
};

using typing::TypeKind;

inline constexpr std::array<Descriptor, 10> table {{
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
}};

[[nodiscard]] inline bool reserved(std::string_view symbol) {
    return symbol.substr(0, prefix.size()) == prefix;
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
