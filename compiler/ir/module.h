#ifndef __joyeer_ir_module_h__
#define __joyeer_ir_module_h__

// The IR module: functions made of basic blocks of instructions, plus the definitions
// (definitions.h) and debug metadata (debuginfo.h) they reference. Lowering builds it
// (irlowering.h), the verifier checks it (verifier.h), and the backend consumes it.

#include "debuginfo.h"
#include "definitions.h"
#include "opcode.h"
#include "value.h"

#include "compiler/frontend/semantic.h"
#include "compiler/frontend/typechecking.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::ir {

enum class PatternKind {
    wildcard,
    integerLiteral,
    booleanLiteral,
    stringLiteral,
    byteLiteral,
    enumCase,
    floatingLiteral,
};

struct Pattern {
    PatternKind kind = PatternKind::wildcard;
    TypeId type = typing::invalidTypeId;
    std::optional<semantic::SymbolId> symbol;
    int64_t integerValue = 0;
    std::string text;
    std::vector<Pattern> payloads;
};

struct SwitchCase {
    Pattern pattern;
    BlockId target = invalidBlockId;
};

struct Instruction {
    Opcode opcode;
    std::optional<Value> result;
    std::vector<ValueId> operands;
    std::vector<BlockId> targets;
    std::optional<FunctionId> callee;
    std::optional<semantic::SymbolId> symbol;
    int64_t integerValue = 0;
    std::string text;
    SourceSpan span;
    std::vector<SwitchCase> switchCases;
    std::optional<DebugLocation> debugLocation;
    std::vector<DebugVariableBinding> debugVariableBindings;
    // The opcode's modifier (`OpcodeDescriptor::modifier`): a `ComparePredicate`, `ConversionMode` or
    // `TypeHelperKind`, or 0 for an opcode without one (decision 0028).
    uint8_t modifier = 0;
};

[[nodiscard]] inline ComparePredicate comparePredicate(const Instruction& instruction) {
    return static_cast<ComparePredicate>(instruction.modifier);
}
[[nodiscard]] inline ConversionMode conversionMode(const Instruction& instruction) {
    return static_cast<ConversionMode>(instruction.modifier);
}
[[nodiscard]] inline TypeHelperKind typeHelperKind(const Instruction& instruction) {
    return static_cast<TypeHelperKind>(instruction.modifier);
}

struct BasicBlock {
    BlockId id = invalidBlockId;
    std::string name;
    std::vector<Instruction> instructions;
};

struct Parameter {
    Value value;
    std::optional<semantic::SymbolId> symbol;
    std::string name;
    bool isMutable = false;
    SourceSpan span;
    bool acceptsAnyType = false;
    bool isConsuming = false;
    bool isInitializing = false;
    std::string label;
};

struct Function {
    FunctionId id = invalidFunctionId;
    std::optional<semantic::SymbolId> symbol;
    std::string name;
    std::vector<Parameter> parameters;
    TypeId resultType = typing::invalidTypeId;
    bool returnsValue = false;
    // The `array_element` addressor of a library array instance (decision 0025): the one kind of
    // function whose result a non-runtime function may turn into an element address.
    bool elementAddressor = false;
    // The literal method of a role-bound collection (decisions 0026, 0027): called once, on an inert zero value,
    // with the literal's packed storage.
    bool literalFactory = false;
    // Closed runtime wrappers whose extra call frame is not part of the unoptimized library cost.
    bool alwaysInline = false;
    bool isExternal = false;
    BlockId entry = invalidBlockId;
    std::vector<BasicBlock> blocks;
    std::optional<DebugLocation> debugLocation;
    std::optional<DebugScopeId> debugScope;
    std::vector<DebugVariableBinding> entryDebugVariableBindings;
    bool isRootModule = true;
    bool runtimeContext = false;
    // From a core unit (spec 19.4.6); implies runtimeContext. Only a core unit may
    // export the program entry point (main or wmain).
    bool coreContext = false;
    std::optional<NativeBinding> nativeBinding;
};

struct Module {
    std::string sourceName;
    std::vector<TypeName> types;
    std::vector<StructureDefinition> structures;
    std::vector<EnumerationDefinition> enumerations;
    std::vector<Function> functions;
    std::vector<Constant> constants;
    std::optional<SourceInfo> sourceInfo;
    std::vector<DebugScope> debugScopes;
    std::vector<DebugVariable> debugVariables;
    // SourceSpan::sourceId indexes this table; sourceInfo is the legacy ID-zero fallback.
    std::vector<SourceInfo> sourceFiles;
    bool isRuntimeObject = false;

    [[nodiscard]] const SourceInfo* sourceFile(uint32_t sourceId) const {
        if (!sourceFiles.empty()) {
            return sourceId < sourceFiles.size() ? &sourceFiles[sourceId] : nullptr;
        }
        return sourceId == 0 && sourceInfo.has_value() ? &*sourceInfo : nullptr;
    }
};

} // namespace joyeer::ir

#endif
