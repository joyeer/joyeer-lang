#ifndef __joyeer_ir_value_h__
#define __joyeer_ir_value_h__

// Identifiers of IR entities and the typed SSA value every instruction defines or uses.

#include "compiler/frontend/typechecking.h"

#include <cstdint>
#include <limits>

namespace joyeer::ir {

using TypeId = typing::TypeId;
using ValueId = uint32_t;
using BlockId = uint32_t;
using FunctionId = uint32_t;
using DebugScopeId = uint32_t;
using DebugVariableId = uint32_t;

inline constexpr ValueId invalidValueId = std::numeric_limits<ValueId>::max();
inline constexpr BlockId invalidBlockId = std::numeric_limits<BlockId>::max();
inline constexpr FunctionId invalidFunctionId = std::numeric_limits<FunctionId>::max();
inline constexpr DebugScopeId invalidDebugScopeId = std::numeric_limits<DebugScopeId>::max();
inline constexpr DebugVariableId invalidDebugVariableId =
    std::numeric_limits<DebugVariableId>::max();

enum class ValueCategory {
    value,
    address,
};

struct Value {
    ValueId id = invalidValueId;
    TypeId type = typing::invalidTypeId;
    ValueCategory category = ValueCategory::value;

    bool operator==(const Value& other) const = default;
};

} // namespace joyeer::ir

#endif
