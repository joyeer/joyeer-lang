#ifndef __joyeer_ir_verifier_internal_h__
#define __joyeer_ir_verifier_internal_h__

// Private to the IR verifier: compiler/ir/verifier.cpp and compiler/ir/verifier/*.cpp. Nothing
// outside the verifier includes this header; the public interface is compiler/ir/verifier.h.
//
// How the verifier is organized
// -----------------------------
// Verifier::verify() creates one ModuleVerifier and calls run(). Verification is a fixed sequence
// of phases. Every phase appends to the same VerificationResult and none stops early, so a broken
// module reports all of its problems, always in the same order.
//
//   1. ModuleVerifier::run() checks module-wide data: the source tables, the type table, struct
//      and enum definitions and their language roles, the functions those roles name, and the
//      debug scopes and variables. While it does so it fills the lookup tables (`types`,
//      `structures`, `functions`, ...) that the later phases read.
//   2. Then, for each function in module order, FunctionVerifier::run() checks the signature and
//      metadata, builds tables of the body (blocks, values, definitions, uses, edges), checks the
//      whole-function protocols (literal storage, element addresses, debug bindings), and then
//      every instruction.
//   3. FunctionVerifier::verifyInstruction() runs the checks all instructions share, then
//      dispatches on the opcode to one check method.
//
// A later phase may rely on what an earlier one filled in, so keep the order in the two run()
// methods when you move a check.
//
// Where each part lives
// ---------------------
//   module.cpp        ModuleVerifier: phase order, reporting helpers, the function table
//   definitions.cpp   types, copyability, concreteness, structs, enums, roles, role functions
//   debuginfo.cpp     source tables, debug spans, scopes and variables; function debug info
//                     and debug bindings
//   function.cpp      FunctionVerifier: phase order, signature, C bindings, body tables, the
//                     element-address escape rule, the block walk
//   literals.cpp      the closed array and dictionary literal protocols (decisions 0026, 0027)
//   instructions.cpp  shared instruction checks, the opcode dispatcher, constants, storage,
//                     operators and terminators
//   aggregates.cpp    struct, enum and String instructions
//   calls.cpp         direct calls, function values and indirect calls
//   capabilities.cpp  runtime-only layout queries, collection capabilities and raw addresses

#include "compiler/ir/destruction.h"
#include "compiler/ir/dominance.h"
#include "compiler/ir/module.h"
#include "compiler/ir/verifier.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace joyeer::ir::verifier {

// Module-wide verification state and checks. One instance verifies one module.
class ModuleVerifier {
public:
    ModuleVerifier(const Module& module, VerificationResult& result);

    // Runs every phase in order, the function phases last.
    void run();

    // Appends one error to the result. Verification always continues after a report.
    void report(VerificationErrorId id,
                std::optional<FunctionId> function,
                std::optional<BlockId> block,
                std::optional<size_t> instruction,
                std::string message);
    // Reports a debug span outside its source file. Does nothing when the module has no source info.
    void verifyDebugLocation(const DebugLocation& location,
                             std::optional<FunctionId> function,
                             std::optional<BlockId> block,
                             std::optional<size_t> instruction,
                             const std::string& owner);
    // Reports `type` when it is in `nonConcreteTypes`; `owner` names the user of the type.
    void verifyConcreteType(TypeId type,
                            std::optional<FunctionId> function,
                            std::optional<BlockId> block,
                            std::optional<size_t> instruction,
                            const std::string& owner);
    // The case of the enumeration `type` with `symbol`, or null when there is none.
    [[nodiscard]] const EnumCaseDefinition* enumCaseFor(TypeId type, semantic::SymbolId symbol) const;

    const Module& module;
    VerificationResult& result;

    // Lookup tables. Each is filled by the phase named beside it and only read afterwards;
    // FunctionVerifier reads all of them.
    std::unordered_map<TypeId, const TypeName*> types;                      // indexTypes
    std::unordered_set<TypeId> noncopyableTypes;                            // computeNoncopyableTypes
    std::unordered_set<TypeId> nonConcreteTypes;                            // computeNonConcreteTypes
    std::unordered_map<TypeId, const StructureDefinition*> structures;      // verifyStructures
    std::unordered_map<TypeId, const EnumerationDefinition*> enumerations;  // verifyEnumerations
    std::unordered_map<FunctionId, const Function*> functions;              // indexFunctions
    std::unordered_set<TypeId> entryTypes;                                  // verifyEntryTypes
    std::unordered_set<FunctionId> literalFactories;                        // verifyLiteralFactories
    std::unordered_map<DebugScopeId, const DebugScope*> debugScopes;        // verifyDebugScopes
    std::unordered_map<DebugVariableId, const DebugVariable*> debugVariables; // verifyDebugVariables
    const DestructionAnalysis destruction;
    // The C symbols of the functions verified so far, which must agree on one signature.
    std::unordered_map<std::string, const Function*> nativeSymbols;
    std::unordered_set<std::string> nativeExports;

private:
    // definitions.cpp, in run() order.
    void indexTypes();
    void verifyTypeNames();
    void computeNoncopyableTypes();
    void computeNonConcreteTypes();
    void verifyConstants();
    void verifyStructures();
    void verifyEnumerations();
    void verifyEnumerationRoles();
    void verifyDeinitializers();
    void verifyEntryTypes();
    void verifyElementAddressors();
    void verifyCopiers();
    void verifyLiteralFactories();
    // module.cpp
    void indexFunctions();
    // debuginfo.cpp
    void verifySourceInfo(const SourceInfo& source);
    void verifySourceFiles();
    void verifyDebugScopes();
    void verifyDebugVariables();
};

// One use of a value as an instruction operand; the literal protocols follow these.
struct LiteralUse {
    const Instruction* instruction;
    BlockId block;
    size_t index;
    size_t operand;
};

// The instruction an opcode check examines, with its operands already looked up.
struct InstructionSite {
    const BasicBlock& block;
    size_t index;
    const Instruction& instruction;
    // `index`, in the form report() takes.
    std::optional<size_t> location;
    // One entry per operand: the operand's value, or null when the value ID is unknown (already
    // reported). Checks test for null before using an entry.
    std::vector<const Value*> operands;
    // Whether `instruction.callee` names a function of the module.
    bool knownCallee;
    // False when the opcode's fixed operand or target count (opcode.h) does not match; already reported.
    // Checks test it before indexing `operands`.
    bool shapeMatches;
    // Whether the opcode is runtime-only and the function is not a runtime function. The opcode's check
    // reports it unless it admits the instruction as verified literal storage or an element place.
    bool unauthorized;
};

// Verification of one function. Created by ModuleVerifier::run() after the module-wide phases.
class FunctionVerifier {
public:
    FunctionVerifier(ModuleVerifier& context, const Function& function);

    // Runs the function's phases in order.
    void run();

private:
    void report(VerificationErrorId id,
                std::optional<FunctionId> function,
                std::optional<BlockId> block,
                std::optional<size_t> instruction,
                std::string message) {
        context.report(id, function, block, instruction, std::move(message));
    }

    // function.cpp: signature, then the tables of the body, then the block walk.
    void verifyAuthorization();
    void verifyNativeBinding();
    void verifyResultType();
    void verifyParameters();
    void verifyExternalFunction(bool hasDebugVariables);
    void indexBlocks();
    void indexValueDefinitions();
    void indexLiteralUses();
    void indexEdges();
    // The instruction that defines `value`, or null for a parameter or an unknown value.
    [[nodiscard]] const Instruction* definingInstruction(ValueId value) const;
    // Whether `instruction` is a `literal` of an integer type: the constant counts and offsets the literal
    // storage protocols prove things from.
    [[nodiscard]] bool isIntegerLiteral(const Instruction* instruction) const;
    void verifyElementAddresses();
    void verifyEntryBlock();
    void verifyNoEntryValues();
    void verifyBlocks();

    // debuginfo.cpp
    void verifyDebugScope();
    bool verifyDebugParameters();
    void verifyDebugBinding(const DebugVariableBinding& binding,
                            std::optional<BlockId> block,
                            std::optional<size_t> instruction,
                            bool entryBinding,
                            const Instruction* anchor);
    void verifyDebugBindings();

    // literals.cpp
    std::unordered_set<BlockId> reachedFrom(BlockId from, bool forward);
    [[nodiscard]] bool evaluatedBefore(ValueId value, BlockId blockId, size_t storageIndex) const;
    std::string dictionaryLiteralFailure(const BasicBlock& block, size_t index, const LiteralUse& transfer,
                                         const std::vector<LiteralUse>& slots, size_t transfers, bool foreignUse);
    void verifyLiteralStorage();

    // instructions.cpp
    void verifyInstruction(const BasicBlock& block, size_t index, bool& foundTerminator);
    void verifyLiteral(const InstructionSite& site);
    void verifyScalarConstant(const InstructionSite& site);
    void verifyConstantValue(const InstructionSite& site);
    void verifyStackAllocate(const InstructionSite& site);
    void verifyZeroInitialize(const InstructionSite& site);
    void verifyLoad(const InstructionSite& site);
    void verifyStore(const InstructionSite& site);
    void verifyCopy(const InstructionSite& site);
    void verifyTake(const InstructionSite& site);
    void verifyDestroy(const InstructionSite& site);
    void verifyDivision(const InstructionSite& site);
    void verifyBitwiseOperator(const InstructionSite& site);
    void verifyConversion(const InstructionSite& site);
    void verifyIntegerFits(const InstructionSite& site);
    void verifyBinaryOperator(const InstructionSite& site);
    void verifyConditionalBranch(const InstructionSite& site);
    void verifySwitchPattern(const InstructionSite& site);
    void verifyReturn(const InstructionSite& site);

    // aggregates.cpp
    void verifyConstructStruct(const InstructionSite& site);
    void verifyFieldProjection(const InstructionSite& site);
    void verifyConstructEnum(const InstructionSite& site);
    void verifyExtractPayload(const InstructionSite& site);
    void verifyPayloadAddress(const InstructionSite& site);
    void verifyStringUtf8(const InstructionSite& site);
    void verifyStringCount(const InstructionSite& site);
    void verifyStringSubscript(const InstructionSite& site);

    // calls.cpp
    void verifyCall(const InstructionSite& site);
    void verifyFunctionAddress(const InstructionSite& site);
    void verifyIndirectCall(const InstructionSite& site);

    // capabilities.cpp
    void verifyLayoutQuery(const InstructionSite& site);
    void verifyCollectionCapability(const InstructionSite& site);
    void verifyIntegerAddress(const InstructionSite& site);
    void verifyPointerToInteger(const InstructionSite& site);
    void verifyTrapIf(const InstructionSite& site);

    ModuleVerifier& context;
    const Module& module;
    const Function& function;
    const std::optional<FunctionId> functionId;

    // The module-wide tables, under the names the checks use (see ModuleVerifier).
    const std::unordered_map<TypeId, const TypeName*>& types;
    const std::unordered_map<TypeId, const StructureDefinition*>& structures;
    const std::unordered_map<FunctionId, const Function*>& functions;
    const std::unordered_map<DebugScopeId, const DebugScope*>& debugScopes;
    const std::unordered_map<DebugVariableId, const DebugVariable*>& debugVariables;
    const std::unordered_set<TypeId>& noncopyableTypes;
    const std::unordered_set<TypeId>& nonConcreteTypes;
    const std::unordered_set<TypeId>& entryTypes;
    const std::unordered_set<FunctionId>& literalFactories;
    const DestructionAnalysis& destruction;
    // The literal protocols' share of the verification work (VerificationResult::literalPathWork).
    size_t& literalPathWork;

    // Tables of the body, filled by the index* phases in run().
    const DominanceAnalysis dominance;
    // Every parameter and instruction result (verifyParameters, indexBlocks).
    std::unordered_map<ValueId, Value> values;
    std::unordered_map<BlockId, const BasicBlock*> blocks;
    // Where each instruction result is defined: its block and instruction index.
    std::unordered_map<ValueId, std::pair<BlockId, size_t>> valueDefinitions;
    // Every operand use of each value.
    std::unordered_map<ValueId, std::vector<LiteralUse>> literalUses;
    // The control-flow edges of the literal protocols' reachability walks (dominance has its own).
    std::unordered_map<BlockId, std::unordered_set<BlockId>> predecessors;
    std::unordered_map<BlockId, std::unordered_set<BlockId>> successors;

    // What verifyLiteralStorage admitted, by result value, with the element type of the storage.
    std::unordered_map<ValueId, TypeId> literalBlocks;
    std::unordered_map<ValueId, TypeId> literalSlots;
    std::unordered_map<ValueId, TypeId> literalPlaces;
    // The key and value field addresses of a verified dictionary literal: the only field addresses of an entry.
    std::unordered_set<ValueId> literalEntryFields;

    // How many bindings each debug variable has (verifyDebugBindings).
    std::unordered_map<DebugVariableId, size_t> debugBindingCounts;
};

} // namespace joyeer::ir::verifier

#endif
