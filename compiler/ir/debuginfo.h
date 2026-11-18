#ifndef __joyeer_ir_debuginfo_h__
#define __joyeer_ir_debuginfo_h__

// Source tables and debug metadata. Lowering attaches these to functions and instructions;
// the verifier checks them (verifier/debuginfo.cpp) and the backend emits DWARF or CodeView from them.

#include "value.h"

#include "compiler/frontend/semantic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::ir {

struct DebugLocation {
    SourceSpan span;
    bool implicitCode = false;
    std::optional<DebugScopeId> scope;
};

struct SourceInfo {
    std::string fileName;
    std::string directory;
    uint64_t byteLength = 0;
    std::vector<uint32_t> lineStarts;
};

enum class DebugScopeKind {
    function,
    lexicalBlock,
};

enum class DebugVariableKind {
    parameter,
    local,
    patternBinding,
};

struct DebugScope {
    DebugScopeId id = invalidDebugScopeId;
    DebugScopeKind kind = DebugScopeKind::lexicalBlock;
    FunctionId function = invalidFunctionId;
    std::optional<DebugScopeId> parent;
    std::optional<semantic::ScopeId> semanticScope;
    SourceSpan span;
};

struct DebugVariable {
    DebugVariableId id = invalidDebugVariableId;
    DebugVariableKind kind = DebugVariableKind::local;
    FunctionId function = invalidFunctionId;
    DebugScopeId scope = invalidDebugScopeId;
    std::optional<semantic::SymbolId> symbol;
    std::string name;
    TypeId type = typing::invalidTypeId;
    SourceSpan span;
    std::optional<uint32_t> parameterIndex;
    bool isMutable = false;
};

struct DebugVariableBinding {
    DebugVariableId variable = invalidDebugVariableId;
    ValueId address = invalidValueId;
};

} // namespace joyeer::ir

#endif
