#ifndef __joyeer_ir_definitions_h__
#define __joyeer_ir_definitions_h__

// Module-level definitions that instructions refer to: the type table, struct and enum
// layouts with their language roles (decisions 0015, 0024-0027), and read-only constants.

#include "value.h"

#include "compiler/frontend/semantic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::ir {

struct TypeName {
    TypeId id = typing::invalidTypeId;
    std::string name;
    typing::TypeKind kind = typing::TypeKind::error;
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::vector<TypeId> arguments;
    // A function type's parameter effects, one per parameter; `arguments` is the parameters, then the
    // result.
    std::vector<syntax::AccessEffect> effects;
    std::optional<SourceSpan> declarationSpan;
};

struct FieldDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::string name;
    TypeId type = typing::invalidTypeId;
    bool isMutable = false;
};

enum class StructureRole : uint8_t {
    none,
    array,
    // The library dictionary handle (decision 0027): inert-zero like the array, owning a key and a value.
    dict,
    // The entry a dictionary literal packs: exactly `key: K` then `value: V`, never a value (decision 0027).
    dictEntry,
};

// The type argument of a role-bound collection that is the element an addressor returns (decisions 0026, 0027):
// the only argument of an array, the value of a dictionary.
inline size_t collectionElementArgument(StructureRole role) {
    switch (role) {
        case StructureRole::array: return 0;
        case StructureRole::dict: return 1;
        case StructureRole::none:
        case StructureRole::dictEntry: break;
    }
    return 0;
}

// How many type arguments a role-bound collection owns, each of which its copy depends on: the element of an
// array, the key and the value of a dictionary; 0 for any other structure.
inline size_t collectionOwnedArguments(StructureRole role) {
    switch (role) {
        case StructureRole::array: return 1;
        case StructureRole::dict: return 2;
        case StructureRole::none:
        case StructureRole::dictEntry: break;
    }
    return 0;
}

// A role-bound collection handle: no hidden live flag, because its all-zero value is destroyable (decisions 0025,
// 0027).
inline bool isInertZero(StructureRole role) {
    return role == StructureRole::array || role == StructureRole::dict;
}

struct StructureDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    TypeId type = typing::invalidTypeId;
    std::string name;
    std::vector<FieldDefinition> fields;
    // The struct's `deinit` (decision 0015): a function taking the value's address and
    // returning nothing, run once when a live value is destroyed, before its fields. A
    // struct with one ends with a hidden Bool field (`liveFieldName`, no source symbol) that
    // construction sets and `take` clears with the rest of the value, so destroying moved-from
    // or uninitialized storage never runs the `deinit`.
    std::optional<FunctionId> deinitializer;
    // A struct bound to the library array role (decision 0025): destruction runs `deinitializer` with
    // no hidden live flag, because the all-zero value is destroyable and the destroy helper zero-fills.
    StructureRole role = StructureRole::none;
    // The method that copies such a struct (a function borrowing its value and returning the struct).
    // Present exactly when the instance is copyable; without it the struct is noncopyable.
    std::optional<FunctionId> copier;
};

inline constexpr const char* liveFieldName = "$live";

struct EnumCaseDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::string name;
    std::vector<TypeId> payloadTypes;
};

// The language role of a library enum (decision 0024). The backend and verifier read the bound
// case indices from the definition and never compare case names.
enum class EnumerationRole : uint8_t {
    none,
    optional,
    result,
};

struct EnumerationDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    TypeId type = typing::invalidTypeId;
    std::string name;
    std::vector<EnumCaseDefinition> cases;
    EnumerationRole role = EnumerationRole::none;
    // Indices into `cases`: for `optional` the present then the absent case, for `result` the success
    // then the failure case. Meaningless when `role` is none.
    uint32_t firstRoleCase = 0;
    uint32_t secondRoleCase = 0;
};

// Read-only array data shared by every use of one top-level constant. `data`
// uses the compiler's constant encoding: little-endian scalars, and strings and
// arrays prefixed by a 64-bit length, nested by the element type.
struct Constant {
    uint32_t id = 0;
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    TypeId type = typing::invalidTypeId;
    std::string name;
    std::string data;
};

} // namespace joyeer::ir

#endif
