// The lowering driver, Builder::build(), and the context every other part reads: diagnostics,
// the per-instance keys, and the types of nodes and symbols with the current instance's type
// arguments substituted.

#include "internal.h"

#include "compiler/frontend/sourcefile.h"

namespace joyeer::lowering::detail {

Result Builder::build(
        const typing::TypeCheckedModel::Ptr& checkedModel,
        std::string sourceName,
        std::optional<ir::SourceInfo> sourceInfo,
        std::vector<ir::SourceInfo> sources) {
    assert(checkedModel != nullptr);
    model = checkedModel;
    module = std::make_shared<ir::Module>();
    module->sourceName = std::move(sourceName);
    module->sourceInfo = std::move(sourceInfo);
    module->sourceFiles = std::move(sources);
    if (!module->sourceFiles.empty() || module->sourceInfo.has_value()) {
        for (const auto& prelude : model->semanticModel()->preludeSources()) {
            if (prelude == nullptr || prelude->tokens.empty()) continue;
            if (module->sourceFiles.empty()) module->sourceFiles.push_back(*module->sourceInfo);
            const auto sourceId = prelude->tokens.front()->span.sourceId;
            // The driver supplies these already; a direct caller gets them appended in identity order.
            if (module->sourceFiles.size() == sourceId) {
                module->sourceFiles.push_back(ir::SourceInfo {
                    prelude->getAbstractPath().filename().string(), prelude->getParentFolder(),
                    prelude->content.size(), prelude->lineStarts });
            }
        }
    }

    snapshotTypes();
    collectAggregateDefinitions();
    destruction.emplace(*module);
    collectFunctions();
    lowerFunctions();

    if (model->types().size() != module->types.size()) {
        report(DiagnosticId::missingType, {},
               "an instance needed a type the type checker did not intern before lowering");
    }
    if (diagnostics.empty()) {
        const auto verification = ir::Verifier().verify(*module);
        for (const auto& error : verification.errors) {
            report(
                    DiagnosticId::verificationFailed,
                    ir::verificationErrorSpan(*module, error),
                    std::string(ir::verificationErrorName(error.id)) +
                            ": " + error.message);
        }
    }
    return Result { module, std::move(diagnostics) };
}

void Builder::report(DiagnosticId id, SourceSpan span, std::string message) {
    diagnostics.push_back(Diagnostic { id, span, std::move(message) });
}

uint64_t Builder::instanceKey(uint32_t id) const {
    const uint64_t slot = currentInstanceIndex.has_value() ? *currentInstanceIndex + 1 : 0;
    return (slot << 32U) | id;
}

// The type lowering uses for a node or symbol: in an instance, with the type arguments
// substituted. Every concrete type was interned by the type checker, so this never grows the
// type table after it was snapshotted (checked at the end of `build`).
typing::TypeId Builder::substituted(typing::TypeId type) const {
    if (currentInstance == nullptr) return type;
    return model->types().substitute(type, currentInstance->substitution);
}

std::optional<typing::TypeId> Builder::typeOf(const syntax::NodePtr& node) const {
    const auto type = model->typeOf(node);
    return type.has_value() ? std::optional<typing::TypeId>(substituted(*type)) : std::nullopt;
}

std::optional<typing::TypeId> Builder::typeOf(semantic::SymbolId symbol) const {
    const auto type = model->typeOf(symbol);
    return type.has_value() ? std::optional<typing::TypeId>(substituted(*type)) : std::nullopt;
}

// Whether a value is copied or moved is decided by the generic body (decision 0021): in an
// instance, `node`'s unsubstituted type decides, not the concrete one.
bool Builder::copyableInBody(const syntax::NodePtr& node, typing::TypeId concrete) const {
    if (currentInstance != nullptr && node != nullptr) {
        if (const auto raw = model->typeOf(node)) return model->isCopyable(*raw);
    }
    return model->isCopyable(concrete);
}

uint64_t Builder::spanKey(SourceSpan span) {
    return (static_cast<uint64_t>(span.sourceId) << 48U) ^ (static_cast<uint64_t>(span.offset) << 20U) ^
            span.length;
}

// Spans in a generic function's body whose expressions have a type that mentions a type
// parameter and is noncopyable there. A copy at one of them is a type-checking gap.
const std::unordered_set<uint64_t>& Builder::noncopyableSpans(const syntax::FunctionDeclSyntax::Ptr& declaration) {
    const auto symbol = model->semanticModel()->declaredSymbol(declaration);
    auto& spans = genericNoncopyableSpans[symbol.value_or(semantic::invalidSymbolId)];
    if (!spans.empty() || !symbol.has_value()) return spans;
    const auto within = declaration->span;
    for (const auto& [node, type] : model->nodeTypeTable()) {
        const auto span = model->semanticModel()->node(node)->span;
        if (span.sourceId != within.sourceId || span.offset < within.offset ||
            span.offset + span.length > within.offset + within.length) {
            continue;
        }
        if (model->types().mentionsTypeParameter(type) && !model->isCopyable(type)) spans.insert(spanKey(span));
    }
    spans.insert(0);
    return spans;
}

} // namespace joyeer::lowering::detail
