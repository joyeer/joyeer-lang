#include "joyeer/compiler/semantic.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <utility>

namespace joyeer::semantic {

SemanticModel::SemanticModel(syntax::SourceFileSyntax::Ptr root):
        root_(std::move(root)) {
}

const syntax::SourceFileSyntax::Ptr& SemanticModel::root() const {
    return root_;
}

ScopeId SemanticModel::preludeScope() const {
    return preludeScope_;
}

ScopeId SemanticModel::fileScope() const {
    return fileScope_;
}

std::optional<NodeId> SemanticModel::nodeId(const syntax::NodePtr& node) const {
    if (node == nullptr) return std::nullopt;
    const auto found = nodeIds_.find(node.get());
    if (found == nodeIds_.end()) return std::nullopt;
    return found->second;
}

const syntax::NodePtr& SemanticModel::node(NodeId id) const {
    assert(id < nodes_.size());
    return nodes_[id];
}

const Symbol* SemanticModel::symbol(SymbolId id) const {
    if (id >= symbols_.size()) return nullptr;
    return &symbols_[id];
}

const Scope* SemanticModel::scope(ScopeId id) const {
    if (id >= scopes_.size()) return nullptr;
    return &scopes_[id];
}

namespace {

template<typename Value>
std::optional<Value> mappedValue(
        const SemanticModel& model,
        const syntax::NodePtr& node,
        const std::unordered_map<NodeId, Value>& values) {
    const auto id = model.nodeId(node);
    if (!id.has_value()) return std::nullopt;
    const auto found = values.find(*id);
    if (found == values.end()) return std::nullopt;
    return found->second;
}

template<typename Value>
std::vector<std::pair<NodeId, Value>> sortedEntries(
        const std::unordered_map<NodeId, Value>& values) {
    std::vector<std::pair<NodeId, Value>> result(values.begin(), values.end());
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    return result;
}

} // namespace

std::optional<ScopeId> SemanticModel::containingScope(const syntax::NodePtr& node) const {
    return mappedValue(*this, node, containingScopes_);
}

std::optional<ScopeId> SemanticModel::introducedScope(const syntax::NodePtr& node) const {
    return mappedValue(*this, node, introducedScopes_);
}

std::optional<SymbolId> SemanticModel::declaredSymbol(const syntax::NodePtr& node) const {
    return mappedValue(*this, node, declarationSymbols_);
}

std::optional<SymbolId> SemanticModel::referencedSymbol(const syntax::NodePtr& node) const {
    return mappedValue(*this, node, referenceSymbols_);
}

std::optional<SymbolId> SemanticModel::qualifierSymbol(const syntax::NodePtr& node) const {
    return mappedValue(*this, node, qualifierSymbols_);
}

std::optional<SymbolId> SemanticModel::callTarget(const syntax::NodePtr& node) const {
    return mappedValue(*this, node, callTargets_);
}

const DeferredReference* SemanticModel::deferredReference(const syntax::NodePtr& node) const {
    const auto id = nodeId(node);
    if (!id.has_value()) return nullptr;
    const auto found = deferredReferenceIndices_.find(*id);
    if (found == deferredReferenceIndices_.end()) return nullptr;
    return &deferredReferences_[found->second];
}

const std::vector<Symbol>& SemanticModel::symbols() const {
    return symbols_;
}

const std::vector<Scope>& SemanticModel::scopes() const {
    return scopes_;
}

const std::vector<DeferredReference>& SemanticModel::deferredReferences() const {
    return deferredReferences_;
}

const char* scopeKindName(ScopeKind kind) {
    switch (kind) {
        case ScopeKind::prelude: return "prelude";
        case ScopeKind::file: return "file";
        case ScopeKind::function: return "function";
        case ScopeKind::block: return "block";
        case ScopeKind::typeMembers: return "type-members";
        case ScopeKind::matchArm: return "match-arm";
    }
    return "unknown";
}

const char* symbolKindName(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::builtinType: return "builtin-type";
        case SymbolKind::builtinFunction: return "builtin-function";
        case SymbolKind::builtinMember: return "builtin-member";
        case SymbolKind::builtinEnumCase: return "builtin-enum-case";
        case SymbolKind::binding: return "binding";
        case SymbolKind::parameter: return "parameter";
        case SymbolKind::function: return "function";
        case SymbolKind::structure: return "struct";
        case SymbolKind::structureField: return "struct-field";
        case SymbolKind::synthesizedInitializer: return "synthesized-initializer";
        case SymbolKind::enumeration: return "enum";
        case SymbolKind::enumCase: return "enum-case";
        case SymbolKind::patternBinding: return "pattern-binding";
    }
    return "unknown";
}

const char* deferredResolutionKindName(DeferredResolutionKind kind) {
    switch (kind) {
        case DeferredResolutionKind::memberNeedsBaseType:
            return "member-needs-base-type";
        case DeferredResolutionKind::contextualEnumCaseNeedsType:
            return "contextual-enum-case-needs-type";
        case DeferredResolutionKind::contextualEnumPatternNeedsType:
            return "contextual-enum-pattern-needs-type";
        case DeferredResolutionKind::callNeedsCalleeType:
            return "call-needs-callee-type";
    }
    return "unknown";
}

std::string dump(const SemanticModel& model) {
    std::ostringstream out;
    for (const auto& scope : model.scopes()) {
        out << "scope " << scope.id << ' ' << scopeKindName(scope.kind);
        if (scope.parent.has_value()) out << " parent=" << *scope.parent;
        if (scope.owner.has_value()) out << " owner=" << *scope.owner;
        out << '\n';
    }
    for (const auto& symbol : model.symbols()) {
        out << "symbol " << symbol.id << ' ' << symbolKindName(symbol.kind)
            << " name=" << symbol.name << " scope=" << symbol.ownerScope;
        if (symbol.declaration.has_value()) out << " decl=" << *symbol.declaration;
        if (symbol.declaredType.has_value()) out << " type=" << *symbol.declaredType;
        if (symbol.memberScope.has_value()) out << " members=" << *symbol.memberScope;
        if (symbol.isInvalid) out << " invalid";
        out << '\n';
    }
    for (const auto& [node, symbol] : sortedEntries(model.declarationSymbols_)) {
        out << "decl " << node << " -> " << symbol << '\n';
    }
    for (const auto& [node, symbol] : sortedEntries(model.referenceSymbols_)) {
        out << "ref " << node << " -> " << symbol << '\n';
    }
    for (const auto& [node, symbol] : sortedEntries(model.callTargets_)) {
        out << "call " << node << " -> " << symbol << '\n';
    }
    for (const auto& deferred : model.deferredReferences()) {
        out << "deferred " << deferred.node << ' '
            << deferredResolutionKindName(deferred.kind)
            << " name=" << deferred.name << '\n';
    }
    return out.str();
}

} // namespace joyeer::semantic