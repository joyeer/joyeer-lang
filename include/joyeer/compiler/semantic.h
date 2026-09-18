#ifndef __joyeer_compiler_semantic_h__
#define __joyeer_compiler_semantic_h__

#include "joyeer/compiler/syntax.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace joyeer::semantic {

using NodeId = uint32_t;
using SymbolId = uint32_t;
using ScopeId = uint32_t;

inline constexpr NodeId invalidNodeId = std::numeric_limits<NodeId>::max();
inline constexpr SymbolId invalidSymbolId = std::numeric_limits<SymbolId>::max();
inline constexpr ScopeId invalidScopeId = std::numeric_limits<ScopeId>::max();

enum class ScopeKind {
    prelude,
    file,
    function,
    block,
    typeMembers,
    matchArm,
};

enum class SymbolNamespace {
    value,
    type,
};

enum class SymbolKind {
    builtinType,
    builtinFunction,
    builtinMember,
    builtinEnumCase,
    binding,
    parameter,
    function,
    structure,
    structureField,
    synthesizedInitializer,
    enumeration,
    enumCase,
    patternBinding,
};

enum class CallableKind {
    function,
    structureInitializer,
    enumCase,
};

enum class DeferredResolutionKind {
    memberNeedsBaseType,
    contextualEnumCaseNeedsType,
    contextualEnumPatternNeedsType,
    callNeedsCalleeType,
};

struct CallableParameter {
    std::optional<std::string> label;
    bool required = true;
    std::optional<NodeId> declaration;
    std::optional<NodeId> typeSyntax;
    std::optional<SymbolId> type;
    syntax::AccessEffect access = syntax::AccessEffect::borrowing;
};

struct CallableSignature {
    CallableKind kind;
    bool acceptsArgumentClause = true;
    std::vector<CallableParameter> parameters;
};

struct Symbol {
    SymbolId id = invalidSymbolId;
    SymbolKind kind;
    SymbolNamespace nameSpace;
    std::string name;
    ScopeId ownerScope = invalidScopeId;
    SourceSpan span;
    std::optional<NodeId> declaration;
    std::optional<SymbolId> containingSymbol;
    std::optional<ScopeId> memberScope;
    std::optional<SymbolId> declaredType;
    std::optional<SymbolId> synthesizedInitializer;
    std::optional<CallableSignature> callable;
    bool isMutable = false;
    bool isInvalid = false;
};

struct Scope {
    ScopeId id = invalidScopeId;
    ScopeKind kind;
    std::optional<ScopeId> parent;
    std::optional<NodeId> owner;
    std::unordered_map<std::string, SymbolId> values;
    std::unordered_map<std::string, SymbolId> types;
};

struct DeferredReference {
    NodeId node = invalidNodeId;
    DeferredResolutionKind kind;
    std::string name;
    SourceSpan span;
};

class NameResolutionBuilder;

class SemanticModel {
public:
    using Ptr = std::shared_ptr<SemanticModel>;

    [[nodiscard]] const syntax::SourceFileSyntax::Ptr& root() const;
    [[nodiscard]] ScopeId preludeScope() const;
    [[nodiscard]] ScopeId fileScope() const;

    [[nodiscard]] std::optional<NodeId> nodeId(const syntax::NodePtr& node) const;
    [[nodiscard]] const syntax::NodePtr& node(NodeId id) const;
    [[nodiscard]] const Symbol* symbol(SymbolId id) const;
    [[nodiscard]] const Scope* scope(ScopeId id) const;

    [[nodiscard]] std::optional<ScopeId> containingScope(const syntax::NodePtr& node) const;
    [[nodiscard]] std::optional<ScopeId> introducedScope(const syntax::NodePtr& node) const;
    [[nodiscard]] std::optional<SymbolId> declaredSymbol(const syntax::NodePtr& node) const;
    [[nodiscard]] std::optional<SymbolId> referencedSymbol(const syntax::NodePtr& node) const;
    [[nodiscard]] std::optional<SymbolId> qualifierSymbol(const syntax::NodePtr& node) const;
    [[nodiscard]] std::optional<SymbolId> callTarget(const syntax::NodePtr& node) const;
    [[nodiscard]] const DeferredReference* deferredReference(const syntax::NodePtr& node) const;

    [[nodiscard]] const std::vector<Symbol>& symbols() const;
    [[nodiscard]] const std::vector<Scope>& scopes() const;
    [[nodiscard]] const std::vector<DeferredReference>& deferredReferences() const;

private:
    friend class NameResolutionBuilder;
    friend std::string dump(const SemanticModel& model);

    explicit SemanticModel(syntax::SourceFileSyntax::Ptr root);

    syntax::SourceFileSyntax::Ptr root_;
    ScopeId preludeScope_ = invalidScopeId;
    ScopeId fileScope_ = invalidScopeId;

    std::vector<syntax::NodePtr> nodes_;
    std::vector<Symbol> symbols_;
    std::vector<Scope> scopes_;
    std::vector<DeferredReference> deferredReferences_;

    std::unordered_map<const syntax::Node*, NodeId> nodeIds_;
    std::unordered_map<NodeId, ScopeId> containingScopes_;
    std::unordered_map<NodeId, ScopeId> introducedScopes_;
    std::unordered_map<NodeId, SymbolId> declarationSymbols_;
    std::unordered_map<NodeId, SymbolId> referenceSymbols_;
    std::unordered_map<NodeId, SymbolId> qualifierSymbols_;
    std::unordered_map<NodeId, SymbolId> callTargets_;
    std::unordered_map<NodeId, size_t> deferredReferenceIndices_;
};

[[nodiscard]] const char* scopeKindName(ScopeKind kind);
[[nodiscard]] const char* symbolKindName(SymbolKind kind);
[[nodiscard]] const char* deferredResolutionKindName(DeferredResolutionKind kind);
[[nodiscard]] std::string dump(const SemanticModel& model);

} // namespace joyeer::semantic

#endif