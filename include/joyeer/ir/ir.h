#ifndef __joyeer_ir_ir_h__
#define __joyeer_ir_ir_h__

#include "joyeer/compiler/semantic.h"
#include "joyeer/compiler/typechecking.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::ir {

using TypeId = typing::TypeId;
using ValueId = uint32_t;
using BlockId = uint32_t;
using FunctionId = uint32_t;

inline constexpr ValueId invalidValueId = std::numeric_limits<ValueId>::max();
inline constexpr BlockId invalidBlockId = std::numeric_limits<BlockId>::max();
inline constexpr FunctionId invalidFunctionId = std::numeric_limits<FunctionId>::max();

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
    less,
    lessEqual,
    greater,
    greaterEqual,
    equal,
    notEqual,
    logicalAnd,

    call,
    constructStruct,
    constructArray,
    constructDictionary,
    fieldAddress,
    extractField,
    constructEnum,
    extractPayload,
    count,
    subscript,
    subscriptAddress,

    branch,
    conditionalBranch,
    switchPattern,
    returnValue,
    returnVoid,
    unreachable,
};

enum class PatternKind {
    wildcard,
    integerLiteral,
    booleanLiteral,
    stringLiteral,
    byteLiteral,
    enumCase,
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
};

struct Function {
    FunctionId id = invalidFunctionId;
    std::optional<semantic::SymbolId> symbol;
    std::string name;
    std::vector<Parameter> parameters;
    TypeId resultType = typing::invalidTypeId;
    bool returnsValue = false;
    bool isExternal = false;
    BlockId entry = invalidBlockId;
    std::vector<BasicBlock> blocks;
};

struct TypeName {
    TypeId id = typing::invalidTypeId;
    std::string name;
    typing::TypeKind kind = typing::TypeKind::error;
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::vector<TypeId> arguments;
};

struct FieldDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::string name;
    TypeId type = typing::invalidTypeId;
    bool isMutable = false;
};

struct StructureDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    TypeId type = typing::invalidTypeId;
    std::string name;
    std::vector<FieldDefinition> fields;
};

struct EnumCaseDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::string name;
    std::vector<TypeId> payloadTypes;
};

struct EnumerationDefinition {
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    TypeId type = typing::invalidTypeId;
    std::string name;
    std::vector<EnumCaseDefinition> cases;
};

struct Module {
    std::string sourceName;
    std::vector<TypeName> types;
    std::vector<StructureDefinition> structures;
    std::vector<EnumerationDefinition> enumerations;
    std::vector<Function> functions;
};

enum class VerificationErrorId {
    duplicateId,
    invalidReference,
    invalidInstruction,
    missingTerminator,
    instructionAfterTerminator,
    typeMismatch,
    callMismatch,
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

[[nodiscard]] bool requiresDestruction(const Module& module, TypeId type);
[[nodiscard]] bool isTerminator(Opcode opcode);
[[nodiscard]] const char* opcodeName(Opcode opcode);
[[nodiscard]] const char* verificationErrorName(VerificationErrorId id);
[[nodiscard]] std::string dump(const Module& module);
[[nodiscard]] std::string dump(const VerificationResult& result);

} // namespace joyeer::ir

#endif
