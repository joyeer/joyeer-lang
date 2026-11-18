// Source tables and debug metadata. Module-wide: the source tables, debug spans, debug scopes
// and debug variables. Per function: the function's root scope and location, debug parameters,
// and the bindings that attach each debug variable to its storage exactly once.

#include "internal.h"

#include <algorithm>
#include <limits>

namespace joyeer::ir::verifier {

// One source table: a name, a 32-bit length, and strictly increasing line starts from 0.
void ModuleVerifier::verifySourceInfo(const SourceInfo& source) {
    if (source.byteLength > std::numeric_limits<uint32_t>::max()) {
        report(
                VerificationErrorId::invalidSourceLocation,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                "source byte length exceeds the 32-bit SourceSpan limit");
    }
    if (source.fileName.empty()) {
        report(
                VerificationErrorId::invalidSourceLocation,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                "source info requires a file name");
    }
    if (source.lineStarts.empty() || source.lineStarts.front() != 0) {
        report(
                VerificationErrorId::invalidSourceLocation,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                "source line starts must begin with byte offset 0");
    }
    for (size_t index = 0; index < source.lineStarts.size(); ++index) {
        if (source.lineStarts[index] > source.byteLength) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "source line start exceeds the source byte length");
            break;
        }
        if (index > 0 && source.lineStarts[index - 1] >= source.lineStarts[index]) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "source line starts must be strictly increasing");
            break;
        }
    }
}

// Every source table, or none at all when the module carries no debug locations.
void ModuleVerifier::verifySourceFiles() {
    if (!module.sourceFiles.empty()) {
        for (const auto& source : module.sourceFiles) verifySourceInfo(source);
    } else if (module.sourceInfo.has_value()) {
        verifySourceInfo(*module.sourceInfo);
    } else {
        const auto hasDebugLocations = !module.debugScopes.empty() ||
                !module.debugVariables.empty() || std::any_of(
                module.functions.begin(),
                module.functions.end(),
                [](const auto& function) {
                    if (function.debugLocation.has_value() || function.debugScope.has_value() ||
                        !function.entryDebugVariableBindings.empty()) {
                        return true;
                    }
                    return std::any_of(
                            function.blocks.begin(),
                            function.blocks.end(),
                            [](const auto& block) {
                                return std::any_of(
                                        block.instructions.begin(),
                                        block.instructions.end(),
                                        [](const auto& instruction) {
                                            return instruction.debugLocation.has_value() ||
                                                    !instruction.debugVariableBindings.empty();
                                        });
                            });
                });
        if (hasDebugLocations) {
            report(
                    VerificationErrorId::invalidSourceLocation,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "module has debug locations but no source info");
        }
    }
}

void ModuleVerifier::verifyDebugLocation(
        const DebugLocation& location,
        std::optional<FunctionId> function,
        std::optional<BlockId> block,
        std::optional<size_t> instruction,
        const std::string& owner) {
    if (module.sourceFile(0) == nullptr) {
        return;
    }
    const auto* source = module.sourceFile(location.span.sourceId);
    if (source == nullptr) {
        report(
                VerificationErrorId::invalidSourceLocation,
                function,
                block,
                instruction,
                owner + " debug span references unknown source file " +
                        std::to_string(location.span.sourceId));
        return;
    }
    const auto end = static_cast<uint64_t>(location.span.offset) +
            static_cast<uint64_t>(location.span.length);
    if (location.span.offset > source->byteLength || end > source->byteLength) {
        report(
                VerificationErrorId::invalidSourceLocation,
                function,
                block,
                instruction,
                owner + " debug span exceeds the source byte length");
    }
}

// Fills `debugScopes`, and checks the scope tree: one function root per function, no cycles.
void ModuleVerifier::verifyDebugScopes() {
    for (const auto& scope : module.debugScopes) {
        if (!debugScopes.emplace(scope.id, &scope).second) {
            report(
                    VerificationErrorId::duplicateId,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "duplicate debug scope id " + std::to_string(scope.id));
        }
        if (!functions.contains(scope.function)) {
            report(
                    VerificationErrorId::invalidReference,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "debug scope references unknown function " +
                            std::to_string(scope.function));
        }
        verifyDebugLocation(
                DebugLocation { scope.span },
                scope.function,
                std::nullopt,
                std::nullopt,
                "debug scope " + std::to_string(scope.id));
    }
    for (const auto& scope : module.debugScopes) {
        if (scope.kind == DebugScopeKind::function && scope.parent.has_value()) {
            report(
                    VerificationErrorId::invalidReference,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "function debug scope must not have a parent");
        }
        if (scope.parent.has_value()) {
            const auto parent = debugScopes.find(*scope.parent);
            if (parent == debugScopes.end()) {
                report(
                        VerificationErrorId::invalidReference,
                        scope.function,
                        std::nullopt,
                        std::nullopt,
                        "debug scope " + std::to_string(scope.id) +
                                " references unknown parent " +
                                std::to_string(*scope.parent));
            } else if (parent->second->function != scope.function) {
                report(
                        VerificationErrorId::invalidReference,
                        scope.function,
                        std::nullopt,
                        std::nullopt,
                        "debug scope parent belongs to another function");
            }
        } else if (scope.kind != DebugScopeKind::function) {
            report(
                    VerificationErrorId::invalidReference,
                    scope.function,
                    std::nullopt,
                    std::nullopt,
                    "lexical debug scope requires a parent");
        }
        std::unordered_set<DebugScopeId> visited;
        auto current = std::optional<DebugScopeId>(scope.id);
        while (current.has_value()) {
            if (!visited.insert(*current).second) {
                report(
                        VerificationErrorId::invalidReference,
                        scope.function,
                        std::nullopt,
                        std::nullopt,
                        "cyclic debug scope hierarchy at scope " +
                                std::to_string(scope.id));
                break;
            }
            const auto found = debugScopes.find(*current);
            if (found == debugScopes.end()) break;
            current = found->second->parent;
        }
    }
}

// Fills `debugVariables`, and checks each variable's scope, type and parameter index.
void ModuleVerifier::verifyDebugVariables() {
    std::unordered_map<FunctionId, std::unordered_set<uint32_t>> parameterIndices;
    for (const auto& variable : module.debugVariables) {
        verifyConcreteType(variable.type, variable.function, std::nullopt, std::nullopt,
                           "debug variable '" + variable.name + "'");
        if (!debugVariables.emplace(variable.id, &variable).second) {
            report(
                    VerificationErrorId::duplicateId,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "duplicate debug variable id " + std::to_string(variable.id));
        }
        const auto scope = debugScopes.find(variable.scope);
        if (scope == debugScopes.end() || scope->second->function != variable.function) {
            report(
                    VerificationErrorId::invalidReference,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "debug variable '" + variable.name +
                            "' references an invalid scope");
        }
        if (!types.contains(variable.type)) {
            report(
                    VerificationErrorId::invalidReference,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "debug variable '" + variable.name +
                            "' references unknown type " + std::to_string(variable.type));
        }
        verifyDebugLocation(
                DebugLocation { variable.span },
                variable.function,
                std::nullopt,
                std::nullopt,
                "debug variable '" + variable.name + "'");
        if (variable.kind == DebugVariableKind::parameter) {
            if (!variable.parameterIndex.has_value() || *variable.parameterIndex == 0 ||
                !functions.contains(variable.function) ||
                *variable.parameterIndex > functions.at(variable.function)->parameters.size() ||
                !parameterIndices[variable.function].insert(*variable.parameterIndex).second) {
                report(
                        VerificationErrorId::invalidReference,
                        variable.function,
                        std::nullopt,
                        std::nullopt,
                        "debug parameter '" + variable.name +
                                "' has an invalid or duplicate parameter index");
            }
        } else if (variable.parameterIndex.has_value()) {
            report(
                    VerificationErrorId::invalidReference,
                    variable.function,
                    std::nullopt,
                    std::nullopt,
                    "non-parameter debug variable '" + variable.name +
                            "' has a parameter index");
        }
    }
}

// The function's root debug scope and location, and that every scope it owns hangs off that root.
void FunctionVerifier::verifyDebugScope() {
    if (function.debugScope.has_value()) {
        const auto scope = debugScopes.find(*function.debugScope);
        if (scope == debugScopes.end() || scope->second->function != function.id ||
            scope->second->kind != DebugScopeKind::function ||
            scope->second->parent.has_value()) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function '" + function.name +
                            "' references an invalid root debug scope");
        }
    }
    size_t functionScopeCount = 0;
    for (const auto& scope : module.debugScopes) {
        if (scope.function != function.id) continue;
        if (scope.kind == DebugScopeKind::function) ++functionScopeCount;
        if (!function.debugScope.has_value()) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function '" + function.name +
                            "' owns debug scopes but has no root scope");
            continue;
        }
        std::unordered_set<DebugScopeId> visited;
        auto current = std::optional<DebugScopeId>(scope.id);
        DebugScopeId root = invalidDebugScopeId;
        while (current.has_value() && visited.insert(*current).second) {
            const auto found = debugScopes.find(*current);
            if (found == debugScopes.end()) break;
            root = found->second->id;
            current = found->second->parent;
        }
        if (root != *function.debugScope) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "debug scope " + std::to_string(scope.id) +
                            " is detached from the function root scope");
        }
        if (scope.id != *function.debugScope &&
            scope.kind != DebugScopeKind::lexicalBlock) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "only the function root may have function debug-scope kind");
        }
    }
    if (function.debugScope.has_value() && functionScopeCount != 1) {
        report(
                VerificationErrorId::invalidReference,
                functionId,
                std::nullopt,
                std::nullopt,
                "function '" + function.name + "' has " +
                        std::to_string(functionScopeCount) +
                        " function debug scopes; exactly one is required");
    }
    const auto hasInstructionDebugLocations = std::any_of(
            function.blocks.begin(),
            function.blocks.end(),
            [](const auto& block) {
                return std::any_of(
                        block.instructions.begin(),
                        block.instructions.end(),
                        [](const auto& instruction) {
                            return instruction.debugLocation.has_value();
                        });
            });
    if (hasInstructionDebugLocations && !function.debugLocation.has_value()) {
        report(
                VerificationErrorId::invalidSourceLocation,
                functionId,
                std::nullopt,
                std::nullopt,
                "function '" + function.name +
                        "' has instruction debug locations but no function location");
    }
    if (function.debugLocation.has_value()) {
        context.verifyDebugLocation(
                *function.debugLocation,
                functionId,
                std::nullopt,
                std::nullopt,
                "function '" + function.name + "'");
        if (function.debugLocation->scope != function.debugScope) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "function location does not reference its root debug scope");
        }
    }
}

// Each debug parameter matches its function parameter; returns whether the function owns any
// debug variable.
bool FunctionVerifier::verifyDebugParameters() {
    bool hasFunctionDebugVariables = false;
    for (const auto& variable : module.debugVariables) {
        if (variable.function != function.id) continue;
        hasFunctionDebugVariables = true;
        if (variable.kind != DebugVariableKind::parameter) continue;
        if (!variable.parameterIndex.has_value() || *variable.parameterIndex == 0 ||
            *variable.parameterIndex > function.parameters.size()) {
            continue;
        }
        const auto& parameter = function.parameters[*variable.parameterIndex - 1];
        if (variable.type != parameter.value.type || variable.name != parameter.name ||
            variable.scope != function.debugScope) {
            report(
                    VerificationErrorId::typeMismatch,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "debug parameter '" + variable.name +
                            "' does not match its function parameter or root scope");
        }
    }
    return hasFunctionDebugVariables;
}

// One binding of a debug variable to its storage: at function entry for an address parameter,
// otherwise on the instruction (`anchor`) that declares the storage in the variable's scope.
void FunctionVerifier::verifyDebugBinding(const DebugVariableBinding& binding,
                                          std::optional<BlockId> block,
                                          std::optional<size_t> instruction,
                                          bool entryBinding,
                                          const Instruction* anchor) {
    const auto variable = debugVariables.find(binding.variable);
    const auto address = values.find(binding.address);
    if (variable == debugVariables.end() ||
        variable->second->function != function.id) {
        report(
                VerificationErrorId::invalidReference,
                functionId,
                block,
                instruction,
                "debug binding references an unknown variable in this function");
        return;
    }
    if (address == values.end() ||
        address->second.category != ValueCategory::address ||
        address->second.type != variable->second->type) {
        report(
                VerificationErrorId::typeMismatch,
                functionId,
                block,
                instruction,
                "debug binding for '" + variable->second->name +
                        "' requires address storage of the variable type");
        return;
    }
    if (entryBinding) {
        const auto parameterIndex = variable->second->parameterIndex;
        if (variable->second->kind != DebugVariableKind::parameter ||
            !parameterIndex.has_value() || *parameterIndex == 0 ||
            *parameterIndex > function.parameters.size() ||
            function.parameters[*parameterIndex - 1].value.id != binding.address ||
            function.parameters[*parameterIndex - 1].value.category !=
                    ValueCategory::address) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    block,
                    instruction,
                    "entry debug binding does not match an address parameter");
            return;
        }
    } else {
        if (anchor == nullptr || !block.has_value() || !instruction.has_value() ||
            !anchor->debugLocation.has_value() ||
            anchor->debugLocation->scope != variable->second->scope) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    block,
                    instruction,
                    "debug binding for '" + variable->second->name +
                            "' is not anchored in its lexical scope");
            return;
        }
        std::optional<ValueId> declaredAddress;
        switch (anchor->opcode) {
            case Opcode::stackAllocate:
                if (anchor->result.has_value()) {
                    declaredAddress = anchor->result->id;
                }
                break;
            case Opcode::zeroInitialize:
                if (!anchor->operands.empty()) declaredAddress = anchor->operands[0];
                break;
            case Opcode::store:
                if (anchor->operands.size() >= 2) declaredAddress = anchor->operands[1];
                break;
            default:
                break;
        }
        if (!declaredAddress.has_value() || *declaredAddress != binding.address) {
            report(
                    VerificationErrorId::invalidInstruction,
                    functionId,
                    block,
                    instruction,
                    "debug binding for '" + variable->second->name +
                            "' is not attached to its storage declaration");
            return;
        }
        if (variable->second->kind == DebugVariableKind::parameter) {
            const auto parameterIndex = variable->second->parameterIndex;
            if (!parameterIndex.has_value() || *parameterIndex == 0 ||
                *parameterIndex > function.parameters.size()) {
                return;
            }
            const auto& parameter = function.parameters[*parameterIndex - 1];
            if (parameter.value.category == ValueCategory::address ||
                anchor->opcode != Opcode::store || anchor->operands.size() < 2 ||
                anchor->operands[0] != parameter.value.id) {
                report(
                        VerificationErrorId::invalidInstruction,
                        functionId,
                        block,
                        instruction,
                        "value debug parameter '" + variable->second->name +
                                "' must bind at the store of its incoming parameter");
                return;
            }
        }
        const auto isParameterAddress = std::any_of(
                function.parameters.begin(),
                function.parameters.end(),
                [&binding](const auto& parameter) {
                    return parameter.value.id == binding.address &&
                            parameter.value.category == ValueCategory::address;
                });
        const auto definition = valueDefinitions.find(binding.address);
        const auto definedEarlier = anchor->opcode == Opcode::stackAllocate ||
            isParameterAddress ||
            (definition != valueDefinitions.end() &&
             ((definition->second.first == *block &&
               definition->second.second < *instruction) ||
              (definition->second.first != *block &&
               dominance.dominates(definition->second.first, *block))));
        if (!definedEarlier) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    block,
                    instruction,
                    "debug binding for '" + variable->second->name +
                            "' uses storage not available at the declaration point");
            return;
        }
    }
    ++debugBindingCounts[binding.variable];
}

// Every binding, then that each of the function's debug variables is bound exactly once.
void FunctionVerifier::verifyDebugBindings() {
    for (const auto& binding : function.entryDebugVariableBindings) {
        verifyDebugBinding(
                binding,
                std::nullopt,
                std::nullopt,
                true,
                nullptr);
    }
    for (const auto& block : function.blocks) {
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            for (const auto& binding : block.instructions[index].debugVariableBindings) {
                verifyDebugBinding(
                        binding,
                        block.id,
                        index,
                        false,
                        &block.instructions[index]);
            }
        }
    }
    for (const auto& variable : module.debugVariables) {
        if (variable.function != function.id) continue;
        const auto count = debugBindingCounts[variable.id];
        if (count != 1) {
            report(
                    VerificationErrorId::invalidReference,
                    functionId,
                    std::nullopt,
                    std::nullopt,
                    "debug variable '" + variable.name + "' has " +
                            std::to_string(count) +
                            " storage bindings; exactly one is required");
        }
    }
}

} // namespace joyeer::ir::verifier
