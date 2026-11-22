// Debug scopes and variables. Each is created on first use and keyed by `instanceKey`, so every
// instance of a generic function gets its own. Nothing is created when the module has no source
// tables.

#include "internal.h"

namespace joyeer::lowering::detail {

std::optional<ir::DebugScopeId> Builder::ensureDebugScope(
        semantic::ScopeId semanticScopeId,
        ir::FunctionId functionId) {
    if (module->sourceFile(0) == nullptr) return std::nullopt;
    const auto existing = debugScopes.find(instanceKey(semanticScopeId));
    if (existing != debugScopes.end()) return existing->second;

    const auto* semanticScope = model->semanticModel()->scope(semanticScopeId);
    if (semanticScope == nullptr) return std::nullopt;
    if (semanticScope->kind != semantic::ScopeKind::function &&
        semanticScope->kind != semantic::ScopeKind::block &&
        semanticScope->kind != semantic::ScopeKind::matchArm) {
        return std::nullopt;
    }

    std::optional<ir::DebugScopeId> parent;
    auto semanticParent = semanticScope->parent;
    while (semanticParent.has_value()) {
        parent = ensureDebugScope(*semanticParent, functionId);
        if (parent.has_value()) break;
        const auto* parentScope = model->semanticModel()->scope(*semanticParent);
        semanticParent = parentScope == nullptr
                ? std::optional<semantic::ScopeId>()
                : parentScope->parent;
    }

    SourceSpan span;
    if (semanticScope->owner.has_value()) {
        span = model->semanticModel()->node(*semanticScope->owner)->span;
    }
    const auto id = static_cast<ir::DebugScopeId>(module->debugScopes.size());
    module->debugScopes.push_back(ir::DebugScope {
        id,
        semanticScope->kind == semantic::ScopeKind::function
                ? ir::DebugScopeKind::function
                : ir::DebugScopeKind::lexicalBlock,
        functionId,
        parent,
        semanticScopeId,
        span,
    });
    debugScopes.emplace(instanceKey(semanticScopeId), id);
    return id;
}

std::optional<ir::DebugScopeId> Builder::debugScopeForNode(
        const syntax::NodePtr& node,
        ir::FunctionId functionId) {
    if (node == nullptr) return std::nullopt;
    const auto semanticScope = model->semanticModel()->introducedScope(node);
    if (!semanticScope.has_value()) return std::nullopt;
    return ensureDebugScope(*semanticScope, functionId);
}

std::optional<ir::DebugVariableId> Builder::ensureDebugVariable(
        semantic::SymbolId symbolId,
        ir::FunctionId functionId,
        std::optional<uint32_t> parameterIndex) {
    if (module->sourceFile(0) == nullptr) return std::nullopt;
    const auto existing = debugVariables.find(instanceKey(symbolId));
    if (existing != debugVariables.end()) return existing->second;

    const auto* symbol = model->semanticModel()->symbol(symbolId);
    const auto type = typeOf(symbolId);
    if (symbol == nullptr || !type.has_value()) return std::nullopt;
    ir::DebugVariableKind kind;
    switch (symbol->kind) {
        case semantic::SymbolKind::parameter:
            kind = ir::DebugVariableKind::parameter;
            break;
        case semantic::SymbolKind::binding:
            kind = ir::DebugVariableKind::local;
            break;
        case semantic::SymbolKind::patternBinding:
            kind = ir::DebugVariableKind::patternBinding;
            break;
        default:
            return std::nullopt;
    }
    const auto scope = ensureDebugScope(symbol->ownerScope, functionId);
    if (!scope.has_value()) return std::nullopt;

    const auto id = static_cast<ir::DebugVariableId>(module->debugVariables.size());
    module->debugVariables.push_back(ir::DebugVariable {
        id,
        kind,
        functionId,
        *scope,
        symbolId,
        symbol->name,
        *type,
        symbol->span,
        parameterIndex,
        symbol->isMutable,
    });
    debugVariables.emplace(instanceKey(symbolId), id);
    return id;
}

} // namespace joyeer::lowering::detail
