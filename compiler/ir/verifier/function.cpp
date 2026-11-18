// FunctionVerifier: the order of the per-function phases, the function's signature and
// native binding, the value, block and use tables the body checks read, the element-address
// escape rule, and the walk over every block. Instruction checks start in instructions.cpp.

#include "internal.h"

#include "compiler/frontend/intrinsics.h"

#include <algorithm>
#include <utility>

namespace joyeer::ir::verifier {

FunctionVerifier::FunctionVerifier(ModuleVerifier& context, const Function& function)
    : context(context),
      module(context.module),
      function(function),
      functionId(function.id),
      types(context.types),
      structures(context.structures),
      functions(context.functions),
      debugScopes(context.debugScopes),
      debugVariables(context.debugVariables),
      noncopyableTypes(context.noncopyableTypes),
      nonConcreteTypes(context.nonConcreteTypes),
      entryTypes(context.entryTypes),
      literalFactories(context.literalFactories),
      destruction(context.destruction),
      literalPathWork(context.result.literalPathWork),
      dominance(function) {}

void FunctionVerifier::run() {
    // The signature and metadata, which an external function has too.
    verifyAuthorization();
    verifyNativeBinding();
    verifyDebugScope();
    verifyResultType();
    verifyParameters();
    const bool hasDebugVariables = verifyDebugParameters();
    if (function.isExternal) {
        verifyExternalFunction(hasDebugVariables);
        return;
    }

    // Tables of the body that the remaining checks read.
    indexBlocks();
    indexValueDefinitions();
    indexLiteralUses();
    indexEdges();

    // Whole-function protocols, which admit the values the instruction checks then allow.
    verifyLiteralStorage();
    verifyElementAddresses();
    verifyDebugBindings();

    verifyEntryBlock();
    verifyNoEntryValues();
    verifyBlocks();
}

// Function flags that need runtime authorization.
void FunctionVerifier::verifyAuthorization() {
    if (function.alwaysInline && (!function.runtimeContext || function.isExternal)) {
        report(VerificationErrorId::invalidInstruction, functionId, {}, {},
               "an always-inline runtime wrapper must be an authorized function with a body");
    }
    if (module.isRuntimeObject && function.isRootModule &&
        !function.isExternal && !function.runtimeContext) {
        report(VerificationErrorId::invalidInstruction, functionId, {}, {},
               "runtime-object root functions require runtime authorization");
    }
}

// A C import or export (spec 19.4.6): an authorized function with a scalar or buffer ABI, a
// compiler intrinsic with exactly its signature, and one signature per C symbol module-wide.
void FunctionVerifier::verifyNativeBinding() {
    if (!function.nativeBinding.has_value()) return;
    const auto& binding = *function.nativeBinding;
    const auto hasKind = [&](TypeId id, typing::TypeKind kind) {
        const auto found = types.find(id);
        return found != types.end() && found->second->kind == kind;
    };
    const bool importing = binding.kind == NativeBindingKind::cImport;
    bool valid = (importing || binding.kind == NativeBindingKind::cExport) &&
            function.runtimeContext && isCIdentifier(binding.symbol) &&
            function.isExternal == importing &&
            !(function.isRootModule && function.name == "main") &&
            (!function.coreContext || function.runtimeContext) &&
            (importing || !isCEntrySymbol(binding.symbol) ||
             (function.coreContext && isCoreEntrySymbol(binding.symbol)));
    const auto scalar = [&](TypeId id) {
        return types.contains(id) && typing::isNativeScalar(types.at(id)->kind);
    };
    const bool scalarResult = scalar(function.resultType);
    // A data symbol is an import with no parameters whose result is its address.
    valid &= !binding.dataSymbol ||
            (importing && function.parameters.empty() && binding.pointerParameters.empty() &&
             hasKind(function.resultType, typing::TypeKind::integer) &&
             !isCEntrySymbol(binding.symbol) && !intrinsic::reserved(binding.symbol));
    valid &= (scalarResult || hasKind(function.resultType, typing::TypeKind::voidType) ||
               hasKind(function.resultType, typing::TypeKind::never)) &&
            function.returnsValue == scalarResult;
    const auto bufferKind = [&](TypeId id) {
        return typing::nativeBufferKind(id, [&](TypeId value) {
            const auto found = types.find(value);
            return found == types.end() ? nullptr : found->second;
        }, [&](TypeId value) {
            const auto found = structures.find(value);
            return found != structures.end() && found->second->role == StructureRole::array;
        });
    };
    for (const auto pointer : binding.pointerParameters) {
        valid &= importing && pointer < function.parameters.size();
    }
    for (size_t parameterIndex = 0; parameterIndex < function.parameters.size(); ++parameterIndex) {
        const auto& parameter = function.parameters[parameterIndex];
        const auto buffer = bufferKind(parameter.value.type);
        if (binding.passesPointer(parameterIndex)) {
            // A bare data pointer: borrowed bytes, or a mutable byte buffer.
            valid &= buffer == typing::NativeBufferKind::bytes &&
                    !parameter.isConsuming && !parameter.isInitializing && !parameter.acceptsAnyType &&
                    ((parameter.value.category == ValueCategory::value && !parameter.isMutable) ||
                     (parameter.value.category == ValueCategory::address && parameter.isMutable));
            continue;
        }
        const bool borrowed =
                (scalar(parameter.value.type) ||
                 (importing && buffer != typing::NativeBufferKind::none)) &&
                parameter.value.category == ValueCategory::value &&
                !parameter.isMutable && !parameter.isConsuming &&
                !parameter.isInitializing && !parameter.acceptsAnyType;
        const bool output = importing && buffer != typing::NativeBufferKind::none &&
                buffer != typing::NativeBufferKind::byteArrays &&
                parameter.value.category == ValueCategory::address &&
                parameter.isMutable && parameter.isInitializing &&
                !parameter.isConsuming && !parameter.acceptsAnyType;
        valid &= borrowed || output;
    }
    if (intrinsic::reserved(binding.symbol)) {
        // A compiler intrinsic must be a known import with exactly its signature.
        const auto* descriptor = intrinsic::find(binding.symbol);
        std::vector<typing::TypeKind> kinds;
        std::vector<bool> pointers;
        std::vector<bool> mutables;
        for (size_t index = 0; index < function.parameters.size(); ++index) {
            const auto found = types.find(function.parameters[index].value.type);
            kinds.push_back(found == types.end() ? typing::TypeKind::error : found->second->kind);
            pointers.push_back(binding.passesPointer(index));
            mutables.push_back(function.parameters[index].isMutable);
        }
        const auto result = types.find(function.resultType);
        valid &= importing && descriptor != nullptr && result != types.end() &&
                intrinsic::matches(*descriptor, result->second->kind, kinds, pointers, mutables);
    }
    if (!valid) {
        report(VerificationErrorId::invalidInstruction, functionId, {}, {},
               "invalid or unauthorized C binding; expected scalar ABI or supported C import buffers");
    }
    const auto [previous, inserted] = context.nativeSymbols.emplace(binding.symbol, &function);
    if (!inserted) {
        const auto& other = *previous->second;
        const auto compatible = [&](TypeId a, TypeId b) {
            if (bufferKind(a) != typing::NativeBufferKind::none ||
                bufferKind(b) != typing::NativeBufferKind::none) {
                return bufferKind(a) == bufferKind(b);
            }
            return types.contains(a) && types.contains(b) &&
                    typing::sameNativeRepresentation(types.at(a)->kind, types.at(b)->kind);
        };
        bool matches = binding.dataSymbol == other.nativeBinding->dataSymbol &&
                compatible(function.resultType, other.resultType) &&
                function.parameters.size() == other.parameters.size();
        for (size_t index = 0; index < std::min(function.parameters.size(), other.parameters.size()); ++index) {
            matches &= compatible(function.parameters[index].value.type, other.parameters[index].value.type);
            matches &= function.parameters[index].isInitializing == other.parameters[index].isInitializing;
            matches &= function.parameters[index].isMutable == other.parameters[index].isMutable;
            matches &= binding.passesPointer(index) == other.nativeBinding->passesPointer(index);
        }
        if (!matches) {
            report(VerificationErrorId::callMismatch, functionId, {}, {},
                   "conflicting signatures for C symbol '" + binding.symbol + "'");
        }
    }
    if (binding.kind == NativeBindingKind::cExport &&
        !context.nativeExports.insert(binding.symbol).second) {
        report(VerificationErrorId::duplicateId, functionId, {}, {},
               "duplicate C export '" + binding.symbol + "'");
    }
}

void FunctionVerifier::verifyResultType() {
    context.verifyConcreteType(function.resultType, functionId, std::nullopt, std::nullopt, "function result");
    if (!types.contains(function.resultType)) {
        report(
                VerificationErrorId::invalidReference,
                functionId,
                std::nullopt,
                std::nullopt,
                "function result references unknown type " +
                        std::to_string(function.resultType));
    }
}

// Each parameter's type and access category; adds the parameters to `values`.
void FunctionVerifier::verifyParameters() {
    for (const auto& parameter : function.parameters) {
        context.verifyConcreteType(parameter.value.type, functionId, std::nullopt, std::nullopt,
                           "parameter '" + parameter.name + "'");
        if (!types.contains(parameter.value.type)) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "parameter '" + parameter.name + "' references unknown type " +
                            std::to_string(parameter.value.type));
        }
        const auto expectedCategory = parameter.isMutable
                ? ValueCategory::address
                : ValueCategory::value;
        if (parameter.value.category != expectedCategory) {
            report(
                    VerificationErrorId::invalidInstruction,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "parameter '" + parameter.name +
                            (parameter.isMutable
                                    ? "' must be an address for inout access"
                                    : "' must be an object value"));
        }
        if (parameter.isMutable && parameter.isConsuming) {
            report(
                    VerificationErrorId::invalidInstruction,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "parameter '" + parameter.name +
                            "' cannot be both inout and consuming");
        }
        if (parameter.isInitializing && !parameter.isMutable) {
            report(
                    VerificationErrorId::invalidInstruction,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "initializing parameter '" + parameter.name +
                            "' must be an address projection");
        }
        if (!values.emplace(parameter.value.id, parameter.value).second) {
            report(
                    VerificationErrorId::duplicateId,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "duplicate value id " + std::to_string(parameter.value.id));
        }
    }
}

// An external function has neither a body nor source debug metadata.
void FunctionVerifier::verifyExternalFunction(bool hasDebugVariables) {
    if (function.debugScope.has_value() || function.debugLocation.has_value() ||
        hasDebugVariables || !function.entryDebugVariableBindings.empty()) {
        report(
                VerificationErrorId::invalidInstruction,
                functionId,
                std::nullopt,
                std::nullopt,
                "external function must not carry source debug scopes or variables");
    }
    if (!function.blocks.empty() || function.entry != invalidBlockId) {
        report(
                VerificationErrorId::invalidInstruction,
                functionId,
                std::nullopt,
                std::nullopt,
                "external function must not contain blocks or an entry block");
    }
}

// Fills `blocks`, and adds every instruction result to `values`, checking each result's type and
// each instruction's debug location on the way.
void FunctionVerifier::indexBlocks() {
    for (const auto& block : function.blocks) {
        if (!blocks.emplace(block.id, &block).second) {
            report(
                    VerificationErrorId::duplicateId,
                    functionId,
                    block.id,
                    std::nullopt,
                    "duplicate block id " + std::to_string(block.id));
        }
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (instruction.debugLocation.has_value()) {
                context.verifyDebugLocation(
                        *instruction.debugLocation,
                        functionId,
                        block.id,
                        index,
                        std::string(opcodeName(instruction.opcode)) + " instruction");
                if (instruction.debugLocation->scope.has_value()) {
                    const auto scope = debugScopes.find(*instruction.debugLocation->scope);
                    if (scope == debugScopes.end() ||
                        scope->second->function != function.id) {
                        report(
                                VerificationErrorId::invalidReference,
                                functionId,
                                block.id,
                                index,
                                "instruction location references a scope from another function");
                    }
                } else if (!function.isExternal && function.debugScope.has_value()) {
                    report(
                            VerificationErrorId::invalidReference,
                            functionId,
                            block.id,
                            index,
                            "source instruction location has no lexical scope");
                }
            }
            if (!instruction.result.has_value()) continue;
            context.verifyConcreteType(instruction.result->type, functionId, block.id, index, "instruction result");
            if (!types.contains(instruction.result->type)) {
                report(
                        VerificationErrorId::invalidReference,
                        functionId,
                        block.id,
                        index,
                        "result references unknown type " +
                                std::to_string(instruction.result->type));
            }
            if (!values.emplace(instruction.result->id, *instruction.result).second) {
                report(
                        VerificationErrorId::duplicateId,
                        functionId,
                        block.id,
                        index,
                        "duplicate value id " +
                                std::to_string(instruction.result->id));
            }
        }
    }
}

// Fills `valueDefinitions`: where each instruction result is defined.
void FunctionVerifier::indexValueDefinitions() {
    for (const auto& block : function.blocks) {
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (instruction.result.has_value()) {
                valueDefinitions.emplace(
                        instruction.result->id,
                        std::make_pair(block.id, index));
            }
        }
    }
}

// Fills `literalUses`: every operand use of every value.
void FunctionVerifier::indexLiteralUses() {
    for (const auto& block : function.blocks) {
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            for (size_t operand = 0; operand < instruction.operands.size(); ++operand) {
                literalUses[instruction.operands[operand]].push_back({ &instruction, block.id, index, operand });
            }
        }
    }
}

const Instruction* FunctionVerifier::definingInstruction(ValueId value) const {
    const auto found = valueDefinitions.find(value);
    if (found == valueDefinitions.end()) return nullptr;
    const auto block = blocks.find(found->second.first);
    if (block == blocks.end() || found->second.second >= block->second->instructions.size()) return nullptr;
    return &block->second->instructions[found->second.second];
}

// Fills `predecessors` and `successors` from the block terminators.
void FunctionVerifier::indexEdges() {
    for (const auto& block : function.blocks) {
        if (block.instructions.empty()) continue;
        const auto connect = [&](BlockId target) {
            if (!blocks.contains(target)) return;
            predecessors[target].insert(block.id);
            successors[block.id].insert(target);
        };
        for (const auto target : block.instructions.back().targets) connect(target);
        if (block.instructions.back().opcode == Opcode::switchPattern) {
            for (const auto& switchCase : block.instructions.back().switchCases) connect(switchCase.target);
        }
    }
}

// The escape rule of a library array's element place (decision 0025): an element address is
// used only in blocks its definition dominates, and only to load, store, take, destroy, project
// or pass as a call argument, never converted back to an Int, returned, stored as a value or
// gathered into an aggregate. The rule is transitive through derived addresses.
void FunctionVerifier::verifyElementAddresses() {
    std::unordered_set<ValueId> elementAddresses;
    const auto addressorCall = [&](const Instruction& instruction) {
        if (instruction.opcode != Opcode::call || !instruction.callee.has_value()) return false;
        const auto callee = functions.find(*instruction.callee);
        return callee != functions.end() && callee->second->elementAddressor;
    };
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != Opcode::integerAddress || !instruction.result.has_value() ||
                instruction.operands.size() != 1) {
                continue;
            }
            const auto definition = valueDefinitions.find(instruction.operands[0]);
            if (definition == valueDefinitions.end()) continue;
            const auto definingBlock = blocks.find(definition->second.first);
            if (definingBlock != blocks.end() &&
                definition->second.second < definingBlock->second->instructions.size() &&
                addressorCall(definingBlock->second->instructions[definition->second.second])) {
                elementAddresses.insert(instruction.result->id);
            }
        }
    }
    for (bool grew = !elementAddresses.empty(); grew;) {
        grew = false;
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if ((instruction.opcode == Opcode::fieldAddress || instruction.opcode == Opcode::payloadAddress) &&
                    instruction.result.has_value() && !instruction.operands.empty() &&
                    elementAddresses.contains(instruction.operands[0]) &&
                    elementAddresses.insert(instruction.result->id).second) {
                    grew = true;
                }
            }
        }
    }
    for (const auto& block : function.blocks) {
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            for (size_t position = 0; position < instruction.operands.size(); ++position) {
                const auto operand = instruction.operands[position];
                if (!elementAddresses.contains(operand)) continue;
                bool allowed = false;
                switch (instruction.opcode) {
                    case Opcode::load:
                    case Opcode::take:
                    case Opcode::destroy:
                    case Opcode::fieldAddress:
                    case Opcode::payloadAddress:
                        allowed = position == 0;
                        break;
                    case Opcode::store:
                        allowed = position == 1;
                        break;
                    case Opcode::call:
                        allowed = true;
                        break;
                    case Opcode::callIndirect:
                        allowed = position >= 1;
                        break;
                    default:
                        break;
                }
                if (!allowed) {
                    report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                           std::string("an element address may not be used by ") +
                                   opcodeName(instruction.opcode) +
                                   ": it is only loaded, stored, taken, destroyed, projected or "
                                   "passed as an argument");
                }
                const auto definition = valueDefinitions.find(operand);
                if (definition != valueDefinitions.end() &&
                    (definition->second.first == block.id
                             ? definition->second.second >= index
                             : !dominance.dominates(definition->second.first, block.id))) {
                    report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                           "an element address is used where its definition does not dominate");
                }
            }
        }
    }
}

void FunctionVerifier::verifyEntryBlock() {
    if (!blocks.contains(function.entry)) {
        report(
                VerificationErrorId::invalidReference,
                functionId,
                std::nullopt,
                std::nullopt,
                "function entry references unknown block " +
                        std::to_string(function.entry));
    }
}

// A dictionary entry is never a value (decision 0027), so no signature carries one.
void FunctionVerifier::verifyNoEntryValues() {
    for (const auto& parameter : function.parameters) {
        if (entryTypes.contains(parameter.value.type)) {
            report(VerificationErrorId::typeMismatch, functionId, std::nullopt, std::nullopt,
                   "a dictionary entry is never a value, so no parameter has its type");
        }
    }
    if (entryTypes.contains(function.resultType)) {
        report(VerificationErrorId::typeMismatch, functionId, std::nullopt, std::nullopt,
               "a dictionary entry is never a value, so no function returns one");
    }
}

// Every instruction of every block, then that each block ends in a terminator.
void FunctionVerifier::verifyBlocks() {
    for (const auto& block : function.blocks) {
        bool foundTerminator = false;
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            verifyInstruction(block, index, foundTerminator);
        }

        if (block.instructions.empty() ||
            !isTerminator(block.instructions.back().opcode)) {
            report(
                    VerificationErrorId::missingTerminator,
                    functionId,
                    block.id,
                    std::nullopt,
                    "block does not end in a terminator");
        }
    }
}

} // namespace joyeer::ir::verifier
