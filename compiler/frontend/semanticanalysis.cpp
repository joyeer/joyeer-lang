#include "semanticanalysis.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace joyeer::analysis {

namespace {

// Visits every function with a body: top-level functions, then the methods of
// structs, enums, and extensions, and a struct's `deinit`. A method is an ordinary function
// whose first parameter is the implicit receiver, so each analysis treats it like any other.
template<class Visitor>
void forEachFunction(const syntax::SourceFileSyntax::Ptr& root, Visitor&& visit) {
    for (const auto& item : root->items) {
        switch (item->kind) {
            case syntax::Kind::functionDecl:
                visit(std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
                break;
            case syntax::Kind::structDecl: {
                const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(item);
                for (const auto& method : structure->methods) visit(method);
                if (structure->deinitializer != nullptr) visit(structure->deinitializer);
                break;
            }
            case syntax::Kind::enumDecl:
                for (const auto& method :
                     std::static_pointer_cast<syntax::EnumDeclSyntax>(item)->methods) {
                    visit(method);
                }
                break;
            case syntax::Kind::extensionDecl:
                for (const auto& method :
                     std::static_pointer_cast<syntax::ExtensionDeclSyntax>(item)->methods) {
                    visit(method);
                }
                break;
            default:
                break;
        }
    }
}

class ControlFlowBuilder {
public:
    Result build(const typing::TypeCheckedModel::Ptr& checkedModel) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        const auto& root = model->semanticModel()->root();
        if (root != nullptr) {
            forEachFunction(root, [this](const syntax::FunctionDeclSyntax::Ptr& function) {
                analyzeFunction(function);
            });
        }
        return Result { std::move(diagnostics) };
    }

private:
    typing::TypeCheckedModel::Ptr model;
    std::vector<Diagnostic> diagnostics;
    std::vector<bool> loopTransfers;

    void report(
            DiagnosticId id,
            Severity severity,
            SourceSpan span,
            std::string message) {
        diagnostics.push_back(Diagnostic { id, severity, span, std::move(message) });
    }

    void analyzeFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        const auto* signature = symbol.has_value() ? model->callable(*symbol) : nullptr;
        if (signature == nullptr) return;
        const auto* binding = model->nativeBinding(*symbol);
        if (binding != nullptr && binding->kind == NativeBindingKind::cImport) return;

        const auto terminates = analyzeBlock(declaration->body);
        if (signature->result == model->types().voidType() || terminates) return;

        const auto trailing = trailingExpression(declaration->body);
        const auto trailingType = trailing == nullptr
                ? std::optional<typing::TypeId>()
                : model->typeOf(trailing);
        if (trailingType.has_value() &&
            model->types().isAssignable(*trailingType, signature->result)) {
            return;
        }

        const auto* semanticSymbol = model->semanticModel()->symbol(*symbol);
        report(
                DiagnosticId::missingReturn,
                Severity::error,
                declaration->span,
                "function '" +
                        (semanticSymbol == nullptr ? std::string() : semanticSymbol->name) +
                        "' does not return '" +
                        model->types().displayName(signature->result) +
                        "' on every path");
    }

    syntax::ExprPtr trailingExpression(const syntax::BlockExprSyntax::Ptr& block) const {
        if (block == nullptr || block->items.empty()) return nullptr;
        const auto& item = block->items.back();
        return isExpressionKind(item->kind)
                ? std::static_pointer_cast<syntax::ExprSyntax>(item)
                : nullptr;
    }

    bool analyzeBlock(const syntax::BlockExprSyntax::Ptr& block) {
        if (block == nullptr) return false;
        bool terminated = false;
        for (const auto& item : block->items) {
            if (terminated) {
                report(
                        DiagnosticId::unreachableCode,
                        Severity::warning,
                        item->span,
                        "statement is unreachable");
                continue;
            }
            terminated = analyzeNode(item);
        }
        return terminated;
    }

    bool analyzeNode(const syntax::NodePtr& node) {
        if (node == nullptr) return false;
        if (node->kind == syntax::Kind::bindingDecl) {
            return analyzeExpression(
                    std::static_pointer_cast<syntax::BindingDeclSyntax>(node)->initializer);
        }
        if (node->kind == syntax::Kind::whileStmt) {
            const auto statement = std::static_pointer_cast<syntax::WhileStmtSyntax>(node);
            loopTransfers.push_back(false);
            const auto conditionTerminates = analyzeExpression(statement->condition);
            if (!conditionTerminates) analyzeBlock(statement->body);
            const bool hasTransfer = loopTransfers.back();
            loopTransfers.pop_back();
            return conditionTerminates && !hasTransfer;
        }
        if (node->kind == syntax::Kind::breakStmt ||
            node->kind == syntax::Kind::continueStmt) {
            assert(!loopTransfers.empty());
            loopTransfers.back() = true;
            return true;
        }
        if (node->kind == syntax::Kind::blockExpr) {
            return analyzeBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(node));
        }
        if (isExpressionKind(node->kind)) {
            return analyzeExpression(std::static_pointer_cast<syntax::ExprSyntax>(node));
        }
        return false;
    }

    bool analyzeExpression(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return false;
        switch (expression->kind) {
            case syntax::Kind::returnExpr: {
                analyzeExpression(
                        std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value);
                return true;
            }
            case syntax::Kind::ifExpr: {
                const auto conditional =
                        std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                if (analyzeExpression(conditional->condition)) return true;
                const auto thenTerminates = analyzeBlock(conditional->thenBranch);
                const auto elseTerminates = conditional->elseBranch != nullptr &&
                        analyzeExpression(conditional->elseBranch);
                return conditional->elseBranch != nullptr &&
                        thenTerminates && elseTerminates;
            }
            case syntax::Kind::matchExpr: {
                const auto match = std::static_pointer_cast<syntax::MatchExprSyntax>(expression);
                if (analyzeExpression(match->scrutinee)) return true;
                bool terminates = !match->arms.empty();
                for (const auto& arm : match->arms) {
                    const auto armTerminates = analyzeExpression(arm->body);
                    terminates = terminates && armTerminates;
                }
                return terminates;
            }
            case syntax::Kind::blockExpr:
                return analyzeBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression));
            case syntax::Kind::parenthesizedExpr:
                return analyzeExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::prefixExpr:
                return analyzeExpression(
                        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand);
            case syntax::Kind::accessExpr:
                return analyzeExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
            case syntax::Kind::propagateExpr:
                return analyzeExpression(
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression)->operand) ||
                        model->typeOf(expression) == model->types().neverType();
            case syntax::Kind::binaryExpr: {
                const auto binary = std::static_pointer_cast<syntax::BinaryExprSyntax>(expression);
                if (analyzeExpression(binary->left)) return true;
                const auto rightTerminates = analyzeExpression(binary->right);
                return (binary->op == nullptr ||
                        (binary->op->kind != andAnd && binary->op->kind != orOr)) &&
                        rightTerminates;
            }
            case syntax::Kind::assignmentExpr: {
                const auto assignment =
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                return analyzeExpression(assignment->target) ||
                        analyzeExpression(assignment->value);
            }
            case syntax::Kind::memberExpr:
                return analyzeExpression(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base);
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                if (analyzeExpression(call->callee)) return true;
                for (const auto& argument : call->arguments) {
                    if (analyzeExpression(argument->value)) return true;
                }
                return model->typeOf(expression) == model->types().neverType();
            }
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                return analyzeExpression(subscript->base) ||
                        analyzeExpression(subscript->index);
            }
            case syntax::Kind::arrayExpr: {
                const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
                return std::any_of(
                        array->elements.begin(),
                        array->elements.end(),
                        [this](const auto& element) { return analyzeExpression(element); });
            }
            case syntax::Kind::dictionaryExpr: {
                const auto dictionary =
                        std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression);
                return std::any_of(
                        dictionary->entries.begin(),
                        dictionary->entries.end(),
                        [this](const auto& entry) {
                            return analyzeExpression(entry->key) ||
                                    analyzeExpression(entry->value);
                        });
            }
            case syntax::Kind::contextualCaseExpr: {
                const auto enumCase =
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
                return std::any_of(
                        enumCase->arguments.begin(),
                        enumCase->arguments.end(),
                        [this](const auto& argument) {
                            return analyzeExpression(argument->value);
                        });
            }
            default:
                return model->typeOf(expression) == model->types().neverType();
        }
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::unitExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::propagateExpr:
            case syntax::Kind::binaryExpr:
            case syntax::Kind::assignmentExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
            case syntax::Kind::subscriptExpr:
            case syntax::Kind::arrayExpr:
            case syntax::Kind::dictionaryExpr:
            case syntax::Kind::contextualCaseExpr:
            case syntax::Kind::blockExpr:
            case syntax::Kind::ifExpr:
            case syntax::Kind::returnExpr:
            case syntax::Kind::matchExpr:
                return true;
            default:
                return false;
        }
    }
};

struct InitializationState {
    std::unordered_set<semantic::SymbolId> initialized;
    std::unordered_set<semantic::SymbolId> possiblyInitialized;
    std::unordered_set<semantic::SymbolId> consumed;
    struct Projection {
        std::optional<semantic::SymbolId> field;
        // A binding identifies its current value only until a write or scope exit.
        std::optional<semantic::SymbolId> indexBinding;
        std::optional<std::string> indexLiteral;

        bool operator==(const Projection& other) const = default;
    };
    struct ProjectionPath {
        semantic::SymbolId root = semantic::invalidSymbolId;
        std::vector<Projection> projections;

        bool operator==(const ProjectionPath& other) const = default;
    };
    std::vector<ProjectionPath> consumedProjections;
    // The projections consumed on every path to this point (consumedProjections is the union).
    // A linear aggregate is ended once all of its linear field paths are in here.
    std::vector<ProjectionPath> definitelyConsumedProjections;
    bool reachable = true;

    bool operator==(const InitializationState& other) const = default;
};

class InitializationBuilder {
public:
    Result build(const typing::TypeCheckedModel::Ptr& checkedModel) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        const auto& root = model->semanticModel()->root();
        if (root != nullptr) {
            forEachFunction(root, [this](const syntax::FunctionDeclSyntax::Ptr& function) {
                analyzeFunction(function);
            });
        }
        return Result { std::move(diagnostics) };
    }

private:
    typing::TypeCheckedModel::Ptr model;
    std::vector<Diagnostic> diagnostics;
    std::unordered_set<semantic::SymbolId> tracked;
    std::unordered_set<semantic::SymbolId> initializingParameters;
    // Consuming parameters of linear type: owning bindings that must be ended like a local.
    std::vector<semantic::SymbolId> linearParameters;
    // Set just before analysing an expression that is a statement, so a block, `if` or `match` in
    // that position knows its tail value is dropped rather than used.
    bool statementPosition = false;
    std::vector<InitializationState::ProjectionPath*> activePaths;
    std::vector<std::vector<semantic::SymbolId>> localScopes;
    struct LoopFlow {
        size_t scopeDepth;
        std::vector<InitializationState> breaks;
        std::vector<InitializationState> continues;
    };
    std::vector<LoopFlow> loops;

    void analyzeFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        assert(activePaths.empty());
        assert(localScopes.empty());
        assert(loops.empty());
        tracked.clear();
        initializingParameters.clear();
        linearParameters.clear();
        const auto function = model->semanticModel()->declaredSymbol(declaration);
        const auto* binding = function.has_value() ? model->nativeBinding(*function) : nullptr;
        if (binding != nullptr && binding->kind == NativeBindingKind::cImport) return;
        InitializationState state;
        for (const auto& parameter : declaration->parameters) {
            const auto symbol = model->semanticModel()->declaredSymbol(parameter);
            if (!symbol.has_value()) continue;
            tracked.insert(*symbol);
            if (parameter->accessEffect() == syntax::AccessEffect::initializing) {
                initializingParameters.insert(*symbol);
            } else {
                state.initialized.insert(*symbol);
                state.possiblyInitialized.insert(*symbol);
                const auto parameterType = model->typeOf(*symbol);
                if (parameter->accessEffect() == syntax::AccessEffect::consuming &&
                    parameterType.has_value() && model->isLinear(*parameterType)) {
                    linearParameters.push_back(*symbol);
                }
            }
        }
        const auto resultType = signatureResult(function);
        const bool bodyYieldsValue = resultType.has_value() &&
                *resultType != model->types().voidType() &&
                *resultType != model->types().neverType();
        analyzeBlock(declaration->body, state, bodyYieldsValue);
        if (state.reachable) {
            reportUninitializedParameters(declaration->span, state);
            for (const auto symbol : linearParameters) {
                checkLinearEnded(symbol, state, declaration->span, "the end of the function");
            }
        }
    }

    // True when the definitely-consumed projections cover `path`: some consumed projection is
    // `path` itself or one of its ancestors.
    bool linearPathEnded(
            const InitializationState::ProjectionPath& path,
            const InitializationState& state) const {
        return std::any_of(
                state.definitelyConsumedProjections.begin(),
                state.definitelyConsumedProjections.end(),
                [&path](const auto& consumed) {
                    return consumed.root == path.root &&
                            consumed.projections.size() <= path.projections.size() &&
                            std::equal(consumed.projections.begin(), consumed.projections.end(),
                                       path.projections.begin());
                });
    }

    // Reports `symbol` when it may still hold a linear value that was never ended: the binding is
    // possibly initialised (so not consumed on every path) and, for an aggregate, some linear field
    // path is not consumed on every path (decision 0015, "The check").
    void checkLinearEnded(
            semantic::SymbolId symbol,
            const InitializationState& state,
            SourceSpan span,
            const char* where) {
        if (!state.reachable || !tracked.contains(symbol) ||
            !state.possiblyInitialized.contains(symbol)) {
            return;
        }
        const auto type = model->typeOf(symbol);
        if (!type.has_value() || !model->isLinear(*type)) return;
        const auto leaves = model->linearLeafPaths(*type);
        // A linear type with no field to consume has no way to end, so it stays live.
        bool live = leaves.empty();
        for (const auto& leaf : leaves) {
            InitializationState::ProjectionPath path { symbol, {} };
            for (const auto field : leaf) {
                path.projections.push_back({ field, std::nullopt, std::nullopt });
            }
            if (!linearPathEnded(path, state)) { live = true; break; }
        }
        if (!live) return;
        const auto* declaration = model->semanticModel()->symbol(symbol);
        diagnostics.push_back(Diagnostic {
            DiagnosticId::linearNotConsumed,
            Severity::error,
            span,
            "'" + (declaration == nullptr ? std::string() : declaration->name) +
                    "' holds a linear value of type '" + model->types().displayName(*type) +
                    "' that is not consumed before " + where,
            "consume it (or return it) on every path before this point; a function that only needs to use it can take it 'inout'",
        });
    }

    // Every owning binding in scope, innermost first, and the consuming parameters: an exit of the
    // whole function (`return`, or the failure path of `?`).
    void checkAllLinearEnded(const InitializationState& state, SourceSpan span, const char* where) {
        for (auto scope = localScopes.rbegin(); scope != localScopes.rend(); ++scope) {
            for (const auto symbol : *scope) checkLinearEnded(symbol, state, span, where);
        }
        for (const auto symbol : linearParameters) checkLinearEnded(symbol, state, span, where);
    }

    // Assigning over a place that may still hold a linear value would drop it.
    void checkLinearOverwrite(
            const syntax::ExprPtr& target,
            const InitializationState::ProjectionPath& path,
            const InitializationState& state) {
        if (!state.reachable || !state.possiblyInitialized.contains(path.root)) return;
        const auto type = model->typeOf(target);
        if (!type.has_value() || !model->isLinear(*type)) return;
        const bool element = std::any_of(
                path.projections.begin(), path.projections.end(),
                [](const auto& projection) { return !projection.field.has_value(); });
        if (!element) {
            // Live while any linear leaf beneath the target is not consumed on every path.
            bool live = false;
            const auto leaves = model->linearLeafPaths(*type);
            if (leaves.empty()) live = !linearPathEnded(path, state);
            for (const auto& leaf : leaves) {
                auto leafPath = path;
                for (const auto field : leaf) {
                    leafPath.projections.push_back({ field, std::nullopt, std::nullopt });
                }
                if (!linearPathEnded(leafPath, state)) { live = true; break; }
            }
            if (!live) return;
        }
        diagnostics.push_back(Diagnostic {
            DiagnosticId::linearOverwrite,
            Severity::error,
            target->span,
            "assigning here would drop a linear value of type '" +
                    model->types().displayName(*type) + "'",
            element ? std::optional<std::string>("drain the array with removeLast instead of overwriting an element")
                    : std::optional<std::string>("consume the old value first, then assign"),
        });
    }

    // A statement whose value is a linear value that nothing receives drops it.
    void reportDroppedLinearValue(const syntax::ExprPtr& statement, const InitializationState& state) {
        if (!state.reachable || statement == nullptr || isPlaceStatement(statement)) return;
        // A block, `if` or `match` reports for its own tails.
        if (statement->kind == syntax::Kind::blockExpr || statement->kind == syntax::Kind::ifExpr ||
            statement->kind == syntax::Kind::matchExpr) {
            return;
        }
        const auto type = model->typeOf(statement);
        if (!type.has_value() || !model->isLinear(*type)) return;
        diagnostics.push_back(Diagnostic {
            DiagnosticId::linearResultDropped,
            Severity::error,
            statement->span,
            "this expression produces a linear value of type '" +
                    model->types().displayName(*type) + "' that is dropped",
            "bind it to a variable and consume it",
        });
    }

    // A memberwise initializer, an enum case and `append` store their arguments, so a linear value
    // passed to one is moved in, not borrowed.
    static bool storesItsArguments(const semantic::Symbol& target) {
        if (target.kind == semantic::SymbolKind::builtinMember) return target.name == "append";
        return target.callable.has_value() &&
                (target.callable->kind == semantic::CallableKind::structureInitializer ||
                 target.callable->kind == semantic::CallableKind::enumCase);
    }

    // An owned linear temporary that is only borrowed (a borrowing argument, a field read, a
    // borrowing receiver) is destroyed afterwards, which drops it (spec 4.7.1, rule 4).
    void reportBorrowedLinearTemporary(const syntax::ExprPtr& value, const InitializationState& state) {
        if (!state.reachable || value == nullptr || storagePath(value).has_value()) return;
        auto inner = value;
        while (inner != nullptr && inner->kind == syntax::Kind::parenthesizedExpr) {
            inner = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(inner)->expression;
        }
        if (inner == nullptr || inner->kind == syntax::Kind::accessExpr) return;
        const auto type = model->typeOf(value);
        if (!type.has_value() || !model->isLinear(*type)) return;
        diagnostics.push_back(Diagnostic {
            DiagnosticId::linearResultDropped,
            Severity::error,
            value->span,
            "this linear value of type '" + model->types().displayName(*type) +
                    "' is only borrowed here and would be dropped",
            "bind it to a variable, borrow the variable, and consume it afterwards",
        });
    }

    // A bare name or field read used as a statement is not a produced value.
    bool isPlaceStatement(const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return false;
        // `consume x;` with nothing to receive the value drops it.
        if (expression->kind == syntax::Kind::accessExpr) {
            const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
            if (access->marker != nullptr && access->marker->kind == kwConsume) return false;
        }
        return storagePath(expression).has_value();
    }

    std::optional<typing::TypeId> signatureResult(
            const std::optional<semantic::SymbolId>& function) const {
        const auto* signature = function.has_value() ? model->callable(*function) : nullptr;
        return signature == nullptr ? std::optional<typing::TypeId>() : signature->result;
    }

    // `valueUsed` is false when the block's last expression is a statement whose value is dropped.
    void analyzeBlock(
            const syntax::BlockExprSyntax::Ptr& block,
            InitializationState& state,
            bool valueUsed = true) {
        if (block == nullptr) return;
        localScopes.emplace_back();
        for (const auto& item : block->items) {
            if (!state.reachable) break;
            if (item->kind == syntax::Kind::bindingDecl) {
                const auto declaration =
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(item);
                const auto symbol = model->semanticModel()->declaredSymbol(declaration);
                if (symbol.has_value()) {
                    tracked.insert(*symbol);
                    localScopes.back().push_back(*symbol);
                }
                analyzeExpression(declaration->initializer, state);
                if (state.reachable && declaration->initializer != nullptr &&
                    symbol.has_value()) {
                    markInitialized({ *symbol, {} }, state);
                }
                continue;
            }
            if (item->kind == syntax::Kind::whileStmt) {
                analyzeWhile(std::static_pointer_cast<syntax::WhileStmtSyntax>(item), state);
                continue;
            }
            if (item->kind == syntax::Kind::breakStmt ||
                item->kind == syntax::Kind::continueStmt) {
                analyzeLoopExit(item->kind, state, item->span);
                continue;
            }
            const bool isValueItem = valueUsed && item == block->items.back();
            if (item->kind == syntax::Kind::blockExpr) {
                analyzeBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(item), state, isValueItem);
                continue;
            }
            if (isExpressionKind(item->kind)) {
                const auto statement = std::static_pointer_cast<syntax::ExprSyntax>(item);
                statementPosition = !isValueItem;
                analyzeExpression(statement, state);
                statementPosition = false;
                // A statement's value is dropped, so a linear one would be lost; the last
                // expression of a block whose value is used is moved on by the caller.
                if (!isValueItem) reportDroppedLinearValue(statement, state);
            }
        }
        for (const auto symbol : localScopes.back()) {

            checkLinearEnded(symbol, state, block->span, "the end of its block");
            forgetBinding(symbol, state);
        }
        localScopes.pop_back();
    }

    void analyzeLoopExit(syntax::Kind kind, InitializationState& state, SourceSpan span) {
        assert(!loops.empty());
        auto edge = state;
        auto& loop = loops.back();
        for (size_t scope = loop.scopeDepth; scope < localScopes.size(); ++scope) {
            for (const auto symbol : localScopes[scope]) {
                checkLinearEnded(symbol, edge, span,
                                 kind == syntax::Kind::breakStmt ? "this 'break'" : "this 'continue'");
                forgetBinding(symbol, edge);
            }
        }
        auto& edges = kind == syntax::Kind::breakStmt ? loop.breaks : loop.continues;
        edges.push_back(std::move(edge));
        state.reachable = false;
    }

    void analyzeWhile(
            const syntax::WhileStmtSyntax::Ptr& statement,
            InitializationState& state) {
        const auto analyzeIteration = [&](InitializationState entry) {
            loops.push_back(LoopFlow { localScopes.size(), {}, {} });
            analyzeExpression(statement->condition, entry);
            auto bodyState = entry;
            analyzeBlock(statement->body, bodyState, false);
            auto flow = std::move(loops.back());
            loops.pop_back();
            flow.continues.push_back(std::move(bodyState));
            return std::make_pair(std::move(entry), std::move(flow));
        };
        auto conditionEntry = state;
        for (;;) {
            const auto diagnosticCount = diagnostics.size();
            auto iteration = analyzeIteration(conditionEntry);
            diagnostics.resize(diagnosticCount);

            // Retain the zero-iteration path and monotonically join every back-edge.
            iteration.second.continues.insert(
                    iteration.second.continues.begin(), conditionEntry);
            auto nextEntry = merge(std::move(iteration.second.continues));
            if (nextEntry == conditionEntry) break;
            conditionEntry = std::move(nextEntry);
        }
        auto iteration = analyzeIteration(std::move(conditionEntry));
        iteration.second.breaks.push_back(std::move(iteration.first));
        state = merge(std::move(iteration.second.breaks));
    }

    void analyzeExpression(
            const syntax::ExprPtr& expression,
            InitializationState& state) {
        const bool statement = std::exchange(statementPosition, false);
        if (expression == nullptr || !state.reachable) return;
        switch (expression->kind) {
            case syntax::Kind::nameExpr:
                analyzeName(std::static_pointer_cast<syntax::NameExprSyntax>(expression), state);
                break;
            case syntax::Kind::parenthesizedExpr:
                analyzeExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression,
                        state);
                break;
            case syntax::Kind::prefixExpr:
                analyzeExpression(
                        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand,
                        state);
                break;
            case syntax::Kind::accessExpr: {
                const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
                // `consume x` where a value is stored moves it out of `x`.
                if (access->marker != nullptr && access->marker->kind == kwConsume) {
                    analyzeConsume(access->operand, state);
                } else {
                    analyzeExpression(access->operand, state);
                }
                break;
            }
            case syntax::Kind::propagateExpr: {
                const auto propagated =
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression);
                analyzeExpression(propagated->operand, state);
                // The failure path of `?` leaves the function, so nothing linear may be live.
                if (state.reachable) {
                    checkAllLinearEnded(state, expression->span, "this '?' returns early");
                }
                if (state.reachable) {
                    const auto operandType = model->typeOf(propagated->operand);
                    const auto* container = operandType.has_value()
                            ? model->types().type(*operandType)
                            : nullptr;
                    if (container == nullptr ||
                        container->kind != typing::TypeKind::result ||
                        container->arguments[1] != model->types().neverType()) {
                        reportUninitializedParameters(expression->span, state);
                    }
                    if (model->typeOf(expression) == model->types().neverType()) {
                        state.reachable = false;
                    }
                }
                break;
            }
            case syntax::Kind::binaryExpr:
                analyzeBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression), state);
                return;
            case syntax::Kind::assignmentExpr:
                analyzeAssignment(
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression),
                        state);
                break;
            case syntax::Kind::memberExpr:
                if (storagePath(expression).has_value()) {
                    analyzeStorageOperands(expression, state);
                    if (state.reachable) analyzeStorageRead(expression, state);
                } else {
                    const auto base =
                            std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base;
                    analyzeExpression(base, state);
                    // Reading through an owned linear temporary borrows it and then drops it.
                    reportBorrowedLinearTemporary(base, state);
                }
                break;
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                const auto target = model->callTarget(call);
                const auto* callableSymbol = target.has_value()
                        ? model->semanticModel()->symbol(*target)
                        : nullptr;
                std::vector<semantic::SymbolId> initializedArguments;
                std::vector<semantic::SymbolId> mutatedArguments;
                // A user method's receiver follows the access convention of its
                // `self` parameter: consuming it consumes the receiver storage.
                const auto receiverEffect = callableSymbol != nullptr &&
                        callableSymbol->kind == semantic::SymbolKind::method &&
                        callableSymbol->callable.has_value() &&
                        callableSymbol->callable->receiver.has_value() &&
                        call->callee->kind == syntax::Kind::memberExpr
                        ? std::optional<syntax::AccessEffect>(
                                callableSymbol->callable->receiver->access)
                        : std::nullopt;
                if (receiverEffect == syntax::AccessEffect::consuming) {
                    analyzeConsume(
                            std::static_pointer_cast<syntax::MemberExprSyntax>(call->callee)->base,
                            state);
                } else {
                    analyzeExpression(call->callee, state);
                    if (receiverEffect == syntax::AccessEffect::inout) {
                        const auto path = storagePath(
                                std::static_pointer_cast<syntax::MemberExprSyntax>(call->callee)
                                        ->base);
                        if (path.has_value()) mutatedArguments.push_back(path->root);
                    }
                }
                for (size_t index = 0; state.reachable && index < call->arguments.size(); ++index) {
                    const auto& argument = call->arguments[index];
                    // A `consume` marker on an argument that is stored (initializer, enum case,
                    // `append`) moves it even though the parameter itself only borrows.
                    const auto marked = argument->accessMarker != nullptr &&
                            argument->accessMarker->kind == kwConsume;
                    auto effect = callableSymbol != nullptr &&
                                        callableSymbol->callable.has_value() &&
                                        index < callableSymbol->callable->parameters.size()
                            ? callableSymbol->callable->parameters[index].access
                            : syntax::AccessEffect::borrowing;
                    if (marked && effect == syntax::AccessEffect::borrowing) {
                        effect = syntax::AccessEffect::consuming;
                    }
                    if (effect == syntax::AccessEffect::consuming) {
                        analyzeConsume(argument->value, state);
                    } else if (effect == syntax::AccessEffect::initializing) {
                        const auto symbol = analyzeInitialize(argument->value, state);
                        if (symbol.has_value()) initializedArguments.push_back(*symbol);
                    } else {
                        analyzeExpression(argument->value, state);
                        if (effect == syntax::AccessEffect::inout) {
                            const auto path = storagePath(argument->value);
                            if (path.has_value()) mutatedArguments.push_back(path->root);
                        } else if (effect == syntax::AccessEffect::borrowing && callableSymbol != nullptr &&
                                   !storesItsArguments(*callableSymbol)) {
                            // A borrowed argument that is an owned linear temporary is dropped
                            // once the call returns.
                            reportBorrowedLinearTemporary(argument->value, state);
                        }
                    }
                }
                if (state.reachable &&
                    model->typeOf(expression) != model->types().neverType()) {
                    if (callableSymbol != nullptr && callableSymbol->isMutable) {
                        const auto receiver = storagePath(call->callee);
                        if (receiver.has_value()) {
                            invalidateIndexIdentity(receiver->root, state);
                        }
                    }
                    for (const auto symbol : mutatedArguments) {
                        invalidateIndexIdentity(symbol, state);
                    }
                    for (const auto symbol : initializedArguments) {
                        markInitialized({ symbol, {} }, state);
                    }
                }
                break;
            }
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                if (storagePath(expression).has_value()) {
                    analyzeStorageOperands(expression, state);
                    if (state.reachable) analyzeStorageRead(expression, state);
                } else {
                    analyzeExpression(subscript->base, state);
                    analyzeExpression(subscript->index, state);
                }
                break;
            }
            case syntax::Kind::arrayExpr: {
                const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
                for (const auto& element : array->elements) {
                    analyzeExpression(element, state);
                }
                break;
            }
            case syntax::Kind::dictionaryExpr: {
                const auto dictionary =
                        std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression);
                for (const auto& entry : dictionary->entries) {
                    analyzeExpression(entry->key, state);
                    analyzeExpression(entry->value, state);
                }
                break;
            }
            case syntax::Kind::contextualCaseExpr: {
                const auto enumCase =
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
                for (const auto& argument : enumCase->arguments) {
                    if (argument->accessMarker != nullptr &&
                        argument->accessMarker->kind == kwConsume) {
                        analyzeConsume(argument->value, state);
                    } else {
                        analyzeExpression(argument->value, state);
                    }
                }
                break;
            }
            case syntax::Kind::blockExpr:
                analyzeBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression), state, !statement);
                break;
            case syntax::Kind::ifExpr:
                analyzeIf(std::static_pointer_cast<syntax::IfExprSyntax>(expression), state, statement);
                break;
            case syntax::Kind::returnExpr: {
                const auto returned =
                        std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value;
                // Returning a linear local moves it out, so it is no longer live at this exit.
                const auto returnedType = returned == nullptr
                        ? std::optional<typing::TypeId>()
                        : model->typeOf(returned);
                if (returned != nullptr && returnedType.has_value() &&
                    model->isLinear(*returnedType) && storagePath(returned).has_value()) {
                    analyzeConsume(returned, state);
                } else {
                    analyzeExpression(returned, state);
                }
                if (state.reachable) {
                    checkAllLinearEnded(state, expression->span, "this 'return'");
                    reportUninitializedParameters(expression->span, state);
                }
                state.reachable = false;
                break;
            }
            case syntax::Kind::matchExpr:
                analyzeMatch(std::static_pointer_cast<syntax::MatchExprSyntax>(expression), state, statement);
                break;
            default:
                break;
        }
        if (state.reachable &&
            model->typeOf(expression) == model->types().neverType()) {
            state.reachable = false;
        }
    }

    void analyzeName(
            const syntax::NameExprSyntax::Ptr& expression,
            const InitializationState& state) {
        analyzeStorageRead(expression, state);
    }

    void analyzeConsume(
            const syntax::ExprPtr& expression,
            InitializationState& state) {
        if (expression == nullptr || !state.reachable) return;
        auto path = storagePath(expression);
        if (!path.has_value()) {
            analyzeExpression(expression, state);
            return;
        }
        activePaths.push_back(&*path);
        analyzeStorageOperands(expression, state);
        activePaths.pop_back();
        if (!state.reachable || !tracked.contains(path->root)) return;
        analyzeStorageRead(expression, state);
        invalidateIndexIdentity(path->root, state);
        if (path->projections.empty()) {
            state.initialized.erase(path->root);
            state.possiblyInitialized.erase(path->root);
            state.consumed.insert(path->root);
            clearConsumedProjections(path->root, state);
            // The whole value is consumed, which covers every field path under it at a join.
            state.definitelyConsumedProjections.push_back({ path->root, {} });
        } else {
            reportPartialConsumeOfDeinitType(expression);
            addConsumedProjection(*path, state);
        }
    }

    // A struct with `deinit` must reach its destructor whole (decision 0015), so no part of
    // it may be moved out: `deinit` would otherwise run over a value with a hole in it.
    void reportPartialConsumeOfDeinitType(const syntax::ExprPtr& expression) {
        auto current = expression;
        while (current != nullptr) {
            syntax::ExprPtr base;
            if (current->kind == syntax::Kind::parenthesizedExpr) {
                base = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)
                               ->expression;
            } else if (current->kind == syntax::Kind::accessExpr) {
                base = std::static_pointer_cast<syntax::AccessExprSyntax>(current)->operand;
            } else if (current->kind == syntax::Kind::memberExpr) {
                base = std::static_pointer_cast<syntax::MemberExprSyntax>(current)->base;
                reportIfDeinitType(base, current->span);
            } else if (current->kind == syntax::Kind::subscriptExpr) {
                base = std::static_pointer_cast<syntax::SubscriptExprSyntax>(current)->base;
            }
            current = base;
        }
    }

    void reportIfDeinitType(const syntax::ExprPtr& base, SourceSpan span) {
        const auto type = model->typeOf(base);
        if (!type.has_value() || model->deinitializerOf(*type) == nullptr) return;
        diagnostics.push_back(Diagnostic {
            DiagnosticId::partialConsumeOfDeinitType,
            Severity::error,
            span,
            "cannot move a part out of '" + model->types().displayName(*type) +
                    "' because it declares 'deinit'",
            "consume the whole value, or read the field and let the value be destroyed",
        });
    }

    std::optional<semantic::SymbolId> analyzeInitialize(
            const syntax::ExprPtr& expression,
            InitializationState& state) {
        if (expression == nullptr || !state.reachable) return std::nullopt;
        const auto symbol = directlyAssignedSymbol(expression);
        if (!symbol.has_value()) {
            analyzeExpression(expression, state);
            return std::nullopt;
        }
        if (!tracked.contains(*symbol)) return std::nullopt;
        if (state.possiblyInitialized.contains(*symbol)) {
            const auto* declaration = model->semanticModel()->symbol(*symbol);
            diagnostics.push_back(Diagnostic {
                DiagnosticId::initializingInitializedStorage,
                Severity::error,
                expression->span,
                "binding '" +
                        (declaration == nullptr ? std::string() : declaration->name) +
                        "' is already initialized",
                    "pass uninitialized storage, or consume the old value before initializing it",
            });
        }
        return symbol;
    }

    void reportUninitializedParameters(
            SourceSpan span,
            const InitializationState& state) {
        for (const auto symbol : initializingParameters) {
            if (state.initialized.contains(symbol)) continue;
            const auto* declaration = model->semanticModel()->symbol(symbol);
            diagnostics.push_back(Diagnostic {
                DiagnosticId::initializingParameterNotInitialized,
                Severity::error,
                span,
                "initializing parameter '" +
                        (declaration == nullptr ? std::string() : declaration->name) +
                        "' is not initialized on this return path",
                    "assign to this initializing parameter on every normal return path",
            });
        }
    }

    void analyzeBinary(
            const syntax::BinaryExprSyntax::Ptr& expression,
            InitializationState& state) {
        analyzeExpression(expression->left, state);
        if (!state.reachable) return;
        if (expression->op != nullptr &&
            (expression->op->kind == andAnd || expression->op->kind == orOr)) {
            const auto skippedRight = state;
            auto evaluatedRight = state;
            analyzeExpression(expression->right, evaluatedRight);
            state = merge({ skippedRight, evaluatedRight });
            return;
        }
        analyzeExpression(expression->right, state);
    }

    void analyzeAssignment(
            const syntax::AssignmentExprSyntax::Ptr& expression,
            InitializationState& state) {
        if (expression->op != nullptr && expression->op->kind != equal) {
            // `x op= y` reads x first, so it must already be initialized.
            auto read = expression->target;
            while (read != nullptr && (read->kind == syntax::Kind::accessExpr ||
                                       read->kind == syntax::Kind::parenthesizedExpr)) {
                read = read->kind == syntax::Kind::accessExpr
                        ? std::static_pointer_cast<syntax::AccessExprSyntax>(read)->operand
                        : std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(read)->expression;
            }
            analyzeExpression(read, state);
        }
        auto assigned = storagePath(expression->target);
        if (assigned.has_value()) activePaths.push_back(&*assigned);
        analyzeAssignmentTarget(expression->target, state);
        analyzeExpression(expression->value, state);
        if (assigned.has_value()) activePaths.pop_back();
        if (state.reachable && assigned.has_value() && tracked.contains(assigned->root)) {
            checkLinearOverwrite(expression->target, *assigned, state);
            markInitialized(*assigned, state);
        }
    }

    void analyzeAssignmentTarget(
            const syntax::ExprPtr& target,
            InitializationState& state) {
        if (target == nullptr || !state.reachable) return;
        const auto path = storagePath(target);
        if (!path.has_value()) {
            analyzeExpression(target, state);
            return;
        }
        analyzeStorageOperands(target, state);
        if (state.reachable && !path->projections.empty()) {
            analyzeProjectionTarget(target, state);
        }
    }

    void analyzeStorageOperands(
            const syntax::ExprPtr& expression,
            InitializationState& state) {
        if (expression == nullptr || !state.reachable) return;
        switch (expression->kind) {
            case syntax::Kind::parenthesizedExpr:
                analyzeStorageOperands(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression,
                        state);
                break;
            case syntax::Kind::accessExpr:
                analyzeStorageOperands(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        state);
                break;
            case syntax::Kind::memberExpr:
                analyzeStorageOperands(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base,
                        state);
                break;
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                analyzeStorageOperands(subscript->base, state);
                analyzeExpression(subscript->index, state);
                break;
            }
            default:
                break;
        }
    }

    std::optional<semantic::SymbolId> directlyAssignedSymbol(
            const syntax::ExprPtr& target) const {
        if (target == nullptr) return std::nullopt;
        if (target->kind == syntax::Kind::nameExpr) {
            return model->referencedSymbol(target);
        }
        if (target->kind == syntax::Kind::parenthesizedExpr) {
            return directlyAssignedSymbol(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression);
        }
        if (target->kind == syntax::Kind::accessExpr) {
            return directlyAssignedSymbol(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand);
        }
        return std::nullopt;
    }

    void analyzeIf(
            const syntax::IfExprSyntax::Ptr& expression,
            InitializationState& state,
            bool statement = false) {
        analyzeExpression(expression->condition, state);
        if (!state.reachable) return;
        auto thenState = state;
        analyzeBlock(expression->thenBranch, thenState, !statement);
        auto elseState = state;
        if (expression->elseBranch != nullptr) {
            statementPosition = statement;
            analyzeExpression(expression->elseBranch, elseState);
            statementPosition = false;
        }
        state = merge({ std::move(thenState), std::move(elseState) });
    }

    // The payload types of an enum case pattern: from the case's signature, or for the builtin
    // Optional and Result cases from the scrutinee's type arguments.
    std::vector<typing::TypeId> payloadTypesOf(
            const std::optional<semantic::SymbolId>& caseSymbol,
            typing::TypeId scrutineeType) const {
        if (!caseSymbol.has_value()) return {};
        const auto* signature = model->callable(*caseSymbol);
        if (signature != nullptr && !signature->parameters.empty()) return signature->parameters;
        const auto* symbol = model->semanticModel()->symbol(*caseSymbol);
        const auto* type = model->types().type(scrutineeType);
        if (symbol == nullptr || type == nullptr) return {};
        if (symbol->name == "Some" || symbol->name == "Ok") {
            if (!type->arguments.empty()) return { type->arguments[0] };
        } else if (symbol->name == "Err") {
            if (type->arguments.size() > 1) return { type->arguments[1] };
        }
        return {};
    }

    // A pattern that skips a linear payload (`_`, a literal) would leave it to be destroyed with the
    // scrutinee, which is a drop: when the scrutinee is moved, every linear payload must be bound
    // (spec 7.1.2). A borrowed scrutinee stays live, so peeking at it with `_` is fine.
    void checkLinearPatternDrops(const syntax::PatternPtr& pattern, typing::TypeId scrutineeType, bool top) {
        if (pattern == nullptr) return;
        if (pattern->kind != syntax::Kind::enumCasePattern) {
            if (top && pattern->kind != syntax::Kind::bindingPattern && model->isLinear(scrutineeType)) {
                diagnostics.push_back(Diagnostic {
                    DiagnosticId::linearResultDropped,
                    Severity::error,
                    pattern->span,
                    "this pattern ignores a linear value of type '" +
                            model->types().displayName(scrutineeType) + "'",
                    "match its cases and bind the linear payloads",
                });
            }
            return;
        }
        const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        const auto payloads = payloadTypesOf(model->referencedSymbol(enumPattern), scrutineeType);
        for (size_t index = 0; index < enumPattern->arguments.size(); ++index) {
            const auto& payloadPattern = enumPattern->arguments[index]->pattern;
            if (payloadPattern == nullptr || index >= payloads.size()) continue;
            if (payloadPattern->kind == syntax::Kind::enumCasePattern) {
                checkLinearPatternDrops(payloadPattern, payloads[index], false);
                continue;
            }
            if (payloadPattern->kind == syntax::Kind::bindingPattern ||
                !model->isLinear(payloads[index])) {
                continue;
            }
            diagnostics.push_back(Diagnostic {
                DiagnosticId::linearResultDropped,
                Severity::error,
                payloadPattern->span,
                "this pattern ignores a linear payload of type '" +
                        model->types().displayName(payloads[index]) + "'",
                "bind the payload to a name and consume it",
            });
        }
    }

    // Whether a match moves its scrutinee: `match consume x` or an owned temporary. A borrowed
    // place stays where it is.
    bool matchMovesScrutinee(const syntax::ExprPtr& scrutinee) const {
        auto current = scrutinee;
        while (current != nullptr && current->kind == syntax::Kind::parenthesizedExpr) {
            current = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(current)->expression;
        }
        if (current == nullptr) return false;
        if (current->kind == syntax::Kind::accessExpr) {
            const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(current);
            return access->marker != nullptr && access->marker->kind == kwConsume;
        }
        return !storagePath(current).has_value();
    }

    void analyzeMatch(
            const syntax::MatchExprSyntax::Ptr& expression,
            InitializationState& state,
            bool statement = false) {
        analyzeExpression(expression->scrutinee, state);
        if (!state.reachable) return;
        const auto scrutineeType = model->typeOf(expression->scrutinee);
        const bool movesLinear = scrutineeType.has_value() && model->isLinear(*scrutineeType) &&
                matchMovesScrutinee(expression->scrutinee);
        std::vector<InitializationState> armStates;
        armStates.reserve(expression->arms.size());
        for (const auto& arm : expression->arms) {
            auto armState = state;
            localScopes.emplace_back();
            if (movesLinear) checkLinearPatternDrops(arm->pattern, *scrutineeType, true);
            initializePattern(arm->pattern, armState, localScopes.back());
            statementPosition = statement;
            analyzeExpression(arm->body, armState);
            statementPosition = false;
            if (statement) reportDroppedLinearValue(arm->body, armState);
            for (const auto symbol : localScopes.back()) {
                checkLinearEnded(symbol, armState, arm->span, "the end of this match arm");
                forgetBinding(symbol, armState);
            }
            localScopes.pop_back();
            armStates.push_back(std::move(armState));
        }
        if (!armStates.empty()) state = merge(std::move(armStates));
    }

    void initializePattern(
            const syntax::PatternPtr& pattern,
            InitializationState& state,
            std::vector<semantic::SymbolId>& bindings) {
        if (pattern == nullptr) return;
        if (pattern->kind == syntax::Kind::bindingPattern) {
            const auto symbol = model->semanticModel()->declaredSymbol(pattern);
            if (symbol.has_value()) {
                tracked.insert(*symbol);
                markInitialized({ *symbol, {} }, state);
                bindings.push_back(*symbol);
            }
            return;
        }
        if (pattern->kind != syntax::Kind::enumCasePattern) return;
        const auto enumCase = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        for (const auto& argument : enumCase->arguments) {
            initializePattern(argument->pattern, state, bindings);
        }
    }

    std::optional<InitializationState::ProjectionPath> storagePath(
            const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return std::nullopt;
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return storagePath(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                            ->expression);
        }
        if (expression->kind == syntax::Kind::accessExpr) {
            return storagePath(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        }
        if (expression->kind == syntax::Kind::nameExpr) {
            const auto symbol = model->referencedSymbol(expression);
            if (!symbol.has_value()) return std::nullopt;
            return InitializationState::ProjectionPath { *symbol, {} };
        }
        if (expression->kind == syntax::Kind::memberExpr) {
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
            auto path = storagePath(member->base);
            if (!path.has_value()) return std::nullopt;
            const auto symbol = model->referencedSymbol(member);
            const auto* semanticMember = symbol.has_value()
                    ? model->semanticModel()->symbol(*symbol)
                    : nullptr;
            if (semanticMember != nullptr &&
                semanticMember->kind == semantic::SymbolKind::structureField) {
                path->projections.push_back({ *symbol, std::nullopt, std::nullopt });
            }
            return path;
        }
        if (expression->kind == syntax::Kind::subscriptExpr) {
            const auto subscript =
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
            auto path = storagePath(subscript->base);
            if (!path.has_value()) return std::nullopt;
            path->projections.push_back(indexProjection(subscript->index));
            return path;
        }
        return std::nullopt;
    }

    InitializationState::Projection indexProjection(
            const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return {};
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return indexProjection(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        }
        if (expression->kind == syntax::Kind::literalExpr) {
            const auto literal =
                    std::static_pointer_cast<syntax::LiteralExprSyntax>(expression)->literal;
            if (literal != nullptr) {
                return { std::nullopt, std::nullopt,
                         std::to_string(literal->kind) + ":" + literal->rawValue };
            }
        }
        if (expression->kind == syntax::Kind::nameExpr) {
            const auto symbol = model->referencedSymbol(expression);
            const auto* declaration = symbol.has_value()
                    ? model->semanticModel()->symbol(*symbol)
                    : nullptr;
            if (declaration != nullptr &&
                (tracked.contains(*symbol) || !declaration->isMutable)) {
                return { std::nullopt, *symbol, std::nullopt };
            }
        }
        return {};
    }

    bool pathsOverlap(
            const InitializationState::ProjectionPath& left,
            const InitializationState::ProjectionPath& right) const {
        if (left.root != right.root) return false;
        const auto count = std::min(left.projections.size(), right.projections.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& leftField = left.projections[index].field;
            const auto& rightField = right.projections[index].field;
            if (leftField.has_value() && rightField.has_value() &&
                leftField != rightField) return false;
        }
        return true;
    }

    bool pathIsPrefix(
            const InitializationState::ProjectionPath& prefix,
            const InitializationState::ProjectionPath& value) const {
        if (prefix.root != value.root ||
            prefix.projections.size() > value.projections.size()) {
            return false;
        }
        for (size_t index = 0; index < prefix.projections.size(); ++index) {
            const auto& projection = prefix.projections[index];
            if ((!projection.field.has_value() &&
                 !projection.indexBinding.has_value() &&
                 !projection.indexLiteral.has_value()) ||
                projection != value.projections[index]) return false;
        }
        return true;
    }

    bool analyzeStorageRead(
            const syntax::ExprPtr& expression,
            const InitializationState& state) {
        const auto path = storagePath(expression);
        if (!path.has_value() || !tracked.contains(path->root)) return true;
        const auto* declaration = model->semanticModel()->symbol(path->root);
        const auto reportRead = [&](bool consumed) {
            diagnostics.push_back(Diagnostic {
                consumed
                        ? DiagnosticId::useAfterConsume
                        : DiagnosticId::useBeforeInitialization,
                Severity::error,
                expression->span,
                "binding '" +
                        (declaration == nullptr ? std::string() : declaration->name) +
                        (consumed
                                ? "' is used after all or part of its ownership was consumed"
                                : "' is used before being initialized"),
                consumed
                        ? std::optional<std::string>(
                                "reinitialize the consumed storage before reading it")
                        : std::optional<std::string>(
                                "initialize this binding on every continuing path before reading it"),
            });
        };
        if (!state.initialized.contains(path->root)) {
            reportRead(state.consumed.contains(path->root));
            return false;
        }
        const auto consumed = std::find_if(
                state.consumedProjections.begin(),
                state.consumedProjections.end(),
                [&](const auto& candidate) { return pathsOverlap(*path, candidate); });
        if (consumed != state.consumedProjections.end()) {
            reportRead(true);
            return false;
        }
        return true;
    }

    void analyzeProjectionTarget(
            const syntax::ExprPtr& expression,
            const InitializationState& state) {
        const auto path = storagePath(expression);
        if (!path.has_value() || !tracked.contains(path->root)) return;
        if (!state.initialized.contains(path->root)) {
            analyzeStorageRead(expression, state);
            return;
        }
        const auto ancestor = std::find_if(
                state.consumedProjections.begin(),
                state.consumedProjections.end(),
                [&](const auto& candidate) {
                    return candidate.projections.size() < path->projections.size() &&
                            pathsOverlap(candidate, *path);
                });
        if (ancestor != state.consumedProjections.end()) {
            analyzeStorageRead(expression, state);
        }
    }

    void addConsumedProjection(
            InitializationState::ProjectionPath path,
            InitializationState& state) {
        const auto ancestor = std::find_if(
                state.consumedProjections.begin(),
                state.consumedProjections.end(),
                [&](const auto& candidate) {
                    return candidate == path || pathIsPrefix(candidate, path);
                });
        if (ancestor != state.consumedProjections.end()) return;
        std::erase_if(state.consumedProjections, [&](const auto& candidate) {
            return pathIsPrefix(path, candidate);
        });
        if (std::find(state.definitelyConsumedProjections.begin(),
                      state.definitelyConsumedProjections.end(), path) ==
            state.definitelyConsumedProjections.end()) {
            state.definitelyConsumedProjections.push_back(path);
        }
        state.consumedProjections.push_back(std::move(path));
    }

    void markInitialized(
            const InitializationState::ProjectionPath& path,
            InitializationState& state) {
        invalidateIndexIdentity(path.root, state);
        if (path.projections.empty()) {
            state.initialized.insert(path.root);
            state.possiblyInitialized.insert(path.root);
            state.consumed.erase(path.root);
            clearConsumedProjections(path.root, state);
            return;
        }
        std::erase_if(state.consumedProjections, [&](const auto& candidate) {
            return pathIsPrefix(path, candidate);
        });
        std::erase_if(state.definitelyConsumedProjections, [&](const auto& candidate) {
            return pathIsPrefix(path, candidate);
        });
    }

    void invalidateIndexIdentity(
            semantic::SymbolId symbol,
            InitializationState& state) {
        const auto invalidate = [symbol](auto& path) {
            for (auto& projection : path.projections) {
                if (projection.indexBinding == symbol) projection.indexBinding.reset();
            }
        };
        for (auto& path : state.consumedProjections) invalidate(path);
        for (auto& path : state.definitelyConsumedProjections) invalidate(path);
        // Captured assignment destinations must not acquire a later index value's identity.
        for (auto* path : activePaths) invalidate(*path);
    }

    void forgetBinding(
            semantic::SymbolId symbol,
            InitializationState& state) {
        invalidateIndexIdentity(symbol, state);
        state.initialized.erase(symbol);
        state.possiblyInitialized.erase(symbol);
        state.consumed.erase(symbol);
        clearConsumedProjections(symbol, state);
    }

    void clearConsumedProjections(
            semantic::SymbolId root,
            InitializationState& state) const {
        std::erase_if(state.consumedProjections, [root](const auto& path) {
            return path.root == root;
        });
        std::erase_if(state.definitelyConsumedProjections, [root](const auto& path) {
            return path.root == root;
        });
    }

    // True when some projection in `set` is `path` itself or one of its ancestors.
    static bool coveredBy(
            const std::vector<InitializationState::ProjectionPath>& set,
            const InitializationState::ProjectionPath& path) {
        return std::any_of(set.begin(), set.end(), [&path](const auto& consumed) {
            return consumed.root == path.root &&
                    consumed.projections.size() <= path.projections.size() &&
                    std::equal(consumed.projections.begin(), consumed.projections.end(),
                               path.projections.begin());
        });
    }

    // What both paths consumed: a projection one path consumed counts when the other consumed it or
    // an ancestor of it, so `consume p` on one branch and `consume p.a`, `consume p.b` on the other
    // leave both fields consumed.
    static std::vector<InitializationState::ProjectionPath> intersectByCoverage(
            const std::vector<InitializationState::ProjectionPath>& left,
            const std::vector<InitializationState::ProjectionPath>& right) {
        std::vector<InitializationState::ProjectionPath> result;
        const auto keep = [&result](const InitializationState::ProjectionPath& path) {
            if (std::find(result.begin(), result.end(), path) == result.end()) result.push_back(path);
        };
        for (const auto& path : left) {
            if (coveredBy(right, path)) keep(path);
        }
        for (const auto& path : right) {
            if (coveredBy(left, path)) keep(path);
        }
        return result;
    }

    InitializationState merge(std::vector<InitializationState> paths) const {
        InitializationState result;
        result.reachable = false;
        for (const auto& path : paths) {
            if (!path.reachable) continue;
            if (!result.reachable) {
                result = path;
                // Scope exits can collapse distinct index identities to the same path.
                result.consumedProjections.clear();
            } else {
                result.definitelyConsumedProjections = intersectByCoverage(
                        result.definitelyConsumedProjections, path.definitelyConsumedProjections);
            }
            for (auto symbol = result.initialized.begin();
                 symbol != result.initialized.end();) {
                if (!path.initialized.contains(*symbol)) {
                    symbol = result.initialized.erase(symbol);
                } else {
                    ++symbol;
                }
            }
            result.possiblyInitialized.insert(
                    path.possiblyInitialized.begin(), path.possiblyInitialized.end());
            result.consumed.insert(path.consumed.begin(), path.consumed.end());
            for (const auto& projection : path.consumedProjections) {
                if (std::find(
                            result.consumedProjections.begin(),
                            result.consumedProjections.end(),
                            projection) == result.consumedProjections.end()) {
                    result.consumedProjections.push_back(projection);
                }
            }
        }
        for (const auto symbol : result.initialized) result.consumed.erase(symbol);
        return result;
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::unitExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::propagateExpr:
            case syntax::Kind::binaryExpr:
            case syntax::Kind::assignmentExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
            case syntax::Kind::subscriptExpr:
            case syntax::Kind::arrayExpr:
            case syntax::Kind::dictionaryExpr:
            case syntax::Kind::contextualCaseExpr:
            case syntax::Kind::blockExpr:
            case syntax::Kind::ifExpr:
            case syntax::Kind::returnExpr:
            case syntax::Kind::matchExpr:
                return true;
            default:
                return false;
        }
    }
};

class UsageBuilder {
public:
    Result build(const typing::TypeCheckedModel::Ptr& checkedModel) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        const auto& root = model->semanticModel()->root();
        if (root != nullptr) {
            forEachFunction(root, [this](const syntax::FunctionDeclSyntax::Ptr& function) {
                analyzeFunction(function);
            });
        }
        return Result { std::move(diagnostics) };
    }

private:
    typing::TypeCheckedModel::Ptr model;
    std::vector<Diagnostic> diagnostics;
    std::vector<semantic::SymbolId> candidates;
    std::unordered_set<semantic::SymbolId> reads;

    void analyzeFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        candidates.clear();
        reads.clear();
        visitBlock(declaration->body);
        for (const auto symbolId : candidates) {
            if (reads.contains(symbolId)) continue;
            const auto* symbol = model->semanticModel()->symbol(symbolId);
            if (symbol == nullptr || symbol->name.starts_with('_')) continue;
            diagnostics.push_back(Diagnostic {
                DiagnosticId::unusedBinding,
                Severity::warning,
                symbol->span,
                "binding '" + symbol->name + "' is never read",
            });
        }
    }

    void addCandidate(const syntax::NodePtr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        if (symbol.has_value()) candidates.push_back(*symbol);
    }

    void visitBlock(const syntax::BlockExprSyntax::Ptr& block) {
        if (block == nullptr) return;
        for (const auto& item : block->items) visitNode(item);
    }

    void visitNode(const syntax::NodePtr& node) {
        if (node == nullptr) return;
        if (node->kind == syntax::Kind::bindingDecl) {
            const auto declaration =
                    std::static_pointer_cast<syntax::BindingDeclSyntax>(node);
            addCandidate(declaration);
            visitExpression(declaration->initializer);
            return;
        }
        if (node->kind == syntax::Kind::whileStmt) {
            const auto statement = std::static_pointer_cast<syntax::WhileStmtSyntax>(node);
            visitExpression(statement->condition);
            visitBlock(statement->body);
            return;
        }
        if (node->kind == syntax::Kind::blockExpr) {
            visitBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(node));
            return;
        }
        if (isExpressionKind(node->kind)) {
            visitExpression(std::static_pointer_cast<syntax::ExprSyntax>(node));
        }
    }

    void visitExpression(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return;
        switch (expression->kind) {
            case syntax::Kind::nameExpr: {
                const auto symbol = model->referencedSymbol(expression);
                if (symbol.has_value()) reads.insert(*symbol);
                break;
            }
            case syntax::Kind::parenthesizedExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
                break;
            case syntax::Kind::prefixExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand);
                break;
            case syntax::Kind::accessExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
                break;
            case syntax::Kind::propagateExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression)->operand);
                break;
            case syntax::Kind::binaryExpr: {
                const auto binary = std::static_pointer_cast<syntax::BinaryExprSyntax>(expression);
                visitExpression(binary->left);
                visitExpression(binary->right);
                break;
            }
            case syntax::Kind::assignmentExpr: {
                const auto assignment =
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                visitAssignmentTarget(assignment->target);
                visitExpression(assignment->value);
                break;
            }
            case syntax::Kind::memberExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base);
                break;
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                visitExpression(call->callee);
                for (const auto& argument : call->arguments) {
                    visitExpression(argument->value);
                }
                break;
            }
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                visitExpression(subscript->base);
                visitExpression(subscript->index);
                break;
            }
            case syntax::Kind::arrayExpr: {
                const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
                for (const auto& element : array->elements) visitExpression(element);
                break;
            }
            case syntax::Kind::dictionaryExpr: {
                const auto dictionary =
                        std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression);
                for (const auto& entry : dictionary->entries) {
                    visitExpression(entry->key);
                    visitExpression(entry->value);
                }
                break;
            }
            case syntax::Kind::contextualCaseExpr: {
                const auto enumCase =
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
                for (const auto& argument : enumCase->arguments) {
                    visitExpression(argument->value);
                }
                break;
            }
            case syntax::Kind::blockExpr:
                visitBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression));
                break;
            case syntax::Kind::ifExpr: {
                const auto conditional =
                        std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                visitExpression(conditional->condition);
                visitBlock(conditional->thenBranch);
                visitExpression(conditional->elseBranch);
                break;
            }
            case syntax::Kind::returnExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value);
                break;
            case syntax::Kind::matchExpr: {
                const auto match = std::static_pointer_cast<syntax::MatchExprSyntax>(expression);
                visitExpression(match->scrutinee);
                for (const auto& arm : match->arms) {
                    visitPattern(arm->pattern);
                    visitExpression(arm->body);
                }
                break;
            }
            default:
                break;
        }
    }

    void visitAssignmentTarget(const syntax::ExprPtr& target) {
        if (target == nullptr) return;
        switch (target->kind) {
            case syntax::Kind::nameExpr:
                return;
            case syntax::Kind::parenthesizedExpr:
                visitAssignmentTarget(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression);
                return;
            case syntax::Kind::accessExpr:
                visitAssignmentTarget(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand);
                return;
            case syntax::Kind::memberExpr:
                visitExpression(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(target)->base);
                return;
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
                visitExpression(subscript->base);
                visitExpression(subscript->index);
                return;
            }
            default:
                visitExpression(target);
                return;
        }
    }

    void visitPattern(const syntax::PatternPtr& pattern) {
        if (pattern == nullptr) return;
        if (pattern->kind == syntax::Kind::bindingPattern) {
            addCandidate(pattern);
            return;
        }
        if (pattern->kind != syntax::Kind::enumCasePattern) return;
        const auto enumCase = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        for (const auto& argument : enumCase->arguments) {
            visitPattern(argument->pattern);
        }
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::unitExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::propagateExpr:
            case syntax::Kind::binaryExpr:
            case syntax::Kind::assignmentExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
            case syntax::Kind::subscriptExpr:
            case syntax::Kind::arrayExpr:
            case syntax::Kind::dictionaryExpr:
            case syntax::Kind::contextualCaseExpr:
            case syntax::Kind::blockExpr:
            case syntax::Kind::ifExpr:
            case syntax::Kind::returnExpr:
            case syntax::Kind::matchExpr:
                return true;
            default:
                return false;
        }
    }
};

} // namespace

bool Result::succeeded() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == Severity::error;
    });
}

Result Analyzer::analyze(const typing::TypeCheckedModel::Ptr& model) const {
    auto result = ControlFlowBuilder().build(model);
    auto initialization = InitializationBuilder().build(model);
    result.diagnostics.insert(
            result.diagnostics.end(),
            initialization.diagnostics.begin(),
            initialization.diagnostics.end());
    auto usage = UsageBuilder().build(model);
    result.diagnostics.insert(
            result.diagnostics.end(),
            usage.diagnostics.begin(),
            usage.diagnostics.end());
    return result;
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::missingReturn: return "semantic-analysis.missing-return";
        case DiagnosticId::unreachableCode: return "semantic-analysis.unreachable-code";
        case DiagnosticId::useBeforeInitialization:
            return "semantic-analysis.use-before-initialization";
        case DiagnosticId::useAfterConsume:
            return "semantic-analysis.use-after-consume";
        case DiagnosticId::initializingInitializedStorage:
            return "semantic-analysis.initializing-initialized-storage";
        case DiagnosticId::initializingParameterNotInitialized:
            return "semantic-analysis.initializing-parameter-not-initialized";
        case DiagnosticId::partialConsumeOfDeinitType:
            return "semantic-analysis.partial-consume-of-deinit-type";
        case DiagnosticId::linearNotConsumed:
            return "semantic-analysis.linear-not-consumed";
        case DiagnosticId::linearOverwrite:
            return "semantic-analysis.linear-overwrite";
        case DiagnosticId::linearResultDropped:
            return "semantic-analysis.linear-result-dropped";
        case DiagnosticId::unusedBinding: return "semantic-analysis.unused-binding";
    }
    return "semantic-analysis.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length
            << ' ' << (diagnostic.severity == Severity::warning ? "warning" : "error")
            << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::analysis
