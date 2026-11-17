#ifndef __joyeer_ir_ir_h__
#define __joyeer_ir_ir_h__

#include "compiler/frontend/semantic.h"
#include "compiler/frontend/typechecking.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
    // Wire slots 35-38 belonged to the removed builtin array operations.
    constructDictionary = 39,
    dictionarySet,
    dictionaryGet,
    fieldAddress,
    extractField,
    constructEnum,
    extractPayload,
    payloadAddress,
    stringUtf8,
    count,
    subscript,
    subscriptAddress,

    branch,
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
};

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

enum class VerificationErrorId {
    duplicateId,
    invalidReference,
    invalidInstruction,
    missingTerminator,
    instructionAfterTerminator,
    typeMismatch,
    callMismatch,
    invalidSourceLocation,
};

struct VerificationError {
    VerificationErrorId id;
    std::optional<FunctionId> function;
    std::optional<BlockId> block;
    std::optional<size_t> instruction;
    std::string message;
};

struct VerificationResult {
    std::vector<VerificationError> errors;

    [[nodiscard]] bool succeeded() const {
        return errors.empty();
    }
};

class Verifier {
public:
    [[nodiscard]] VerificationResult verify(const Module& module) const;
};

// Snapshot of one module's type graph. Rebuild after changing type definitions.
class DestructionAnalysis {
public:
    explicit DestructionAnalysis(const Module& module);
    [[nodiscard]] bool requiresDestruction(TypeId type) const;

private:
    std::unordered_map<TypeId, bool> required;
};

[[nodiscard]] bool requiresDestruction(const Module& module, TypeId type);
[[nodiscard]] bool isTerminator(Opcode opcode);
[[nodiscard]] const char* opcodeName(Opcode opcode);
[[nodiscard]] const char* verificationErrorName(VerificationErrorId id);
[[nodiscard]] std::string dump(const Module& module);
[[nodiscard]] std::string dump(const VerificationResult& result);

} // namespace joyeer::ir

#endif
