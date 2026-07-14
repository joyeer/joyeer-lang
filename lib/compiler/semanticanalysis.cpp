#include "joyeer/compiler/semanticanalysis.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace joyeer::analysis {

namespace {

class ControlFlowBuilder {
public:
    Result build(const typing::TypeCheckedModel::Ptr& checkedModel) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        const auto& root = model->semanticModel()->root();
        if (root != nullptr) {
            for (const auto& item : root->items) {
                if (item->kind == syntax::Kind::functionDecl) {
                    analyzeFunction(
                            std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
                }
            }
        }
        return Result { std::move(diagnostics) };
    }

private:
    typing::TypeCheckedModel::Ptr model;
    std::vector<Diagnostic> diagnostics;

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
            if (analyzeExpression(statement->condition)) return true;
            analyzeBlock(statement->body);
            return false;
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
                return !match->arms.empty() && std::all_of(
                        match->arms.begin(),
                        match->arms.end(),
                        [this](const auto& arm) {
                            return analyzeExpression(arm->body);
                        });
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
            case syntax::Kind::binaryExpr: {
                const auto binary = std::static_pointer_cast<syntax::BinaryExprSyntax>(expression);
                return analyzeExpression(binary->left) || analyzeExpression(binary->right);
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
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
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
    bool reachable = true;
};

class InitializationBuilder {
public:
    Result build(const typing::TypeCheckedModel::Ptr& checkedModel) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        const auto& root = model->semanticModel()->root();
        if (root != nullptr) {
            for (const auto& item : root->items) {
                if (item->kind == syntax::Kind::functionDecl) {
                    analyzeFunction(
                            std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
                }
            }
        }
        return Result { std::move(diagnostics) };
    }

private:
    typing::TypeCheckedModel::Ptr model;
    std::vector<Diagnostic> diagnostics;
    std::unordered_set<semantic::SymbolId> tracked;

    void analyzeFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        tracked.clear();
        InitializationState state;
        for (const auto& parameter : declaration->parameters) {
            const auto symbol = model->semanticModel()->declaredSymbol(parameter);
            if (!symbol.has_value()) continue;
            tracked.insert(*symbol);
            state.initialized.insert(*symbol);
        }
        analyzeBlock(declaration->body, state);
    }

    void analyzeBlock(
            const syntax::BlockExprSyntax::Ptr& block,
            InitializationState& state) {
        if (block == nullptr) return;
        std::vector<semantic::SymbolId> locals;
        for (const auto& item : block->items) {
            if (!state.reachable) break;
            if (item->kind == syntax::Kind::bindingDecl) {
                const auto declaration =
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(item);
                const auto symbol = model->semanticModel()->declaredSymbol(declaration);
                if (symbol.has_value()) {
                    tracked.insert(*symbol);
                    locals.push_back(*symbol);
                }
                analyzeExpression(declaration->initializer, state);
                if (state.reachable && declaration->initializer != nullptr &&
                    symbol.has_value()) {
                    state.initialized.insert(*symbol);
                }
                continue;
            }
            if (item->kind == syntax::Kind::whileStmt) {
                analyzeWhile(std::static_pointer_cast<syntax::WhileStmtSyntax>(item), state);
                continue;
            }
            if (item->kind == syntax::Kind::blockExpr) {
                analyzeBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(item), state);
                continue;
            }
            if (isExpressionKind(item->kind)) {
                analyzeExpression(std::static_pointer_cast<syntax::ExprSyntax>(item), state);
            }
        }
        for (const auto symbol : locals) state.initialized.erase(symbol);
    }

    void analyzeWhile(
            const syntax::WhileStmtSyntax::Ptr& statement,
            InitializationState& state) {
        analyzeExpression(statement->condition, state);
        if (!state.reachable) return;
        const auto afterCondition = state;
        auto bodyState = afterCondition;
        analyzeBlock(statement->body, bodyState);
        state = afterCondition;
    }

    void analyzeExpression(
            const syntax::ExprPtr& expression,
            InitializationState& state) {
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
            case syntax::Kind::accessExpr:
                analyzeExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        state);
                break;
            case syntax::Kind::binaryExpr:
                analyzeBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression), state);
                break;
            case syntax::Kind::assignmentExpr:
                analyzeAssignment(
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression),
                        state);
                break;
            case syntax::Kind::memberExpr:
                analyzeExpression(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base,
                        state);
                break;
            case syntax::Kind::callExpr: {
                const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
                analyzeExpression(call->callee, state);
                for (const auto& argument : call->arguments) {
                    analyzeExpression(argument->value, state);
                }
                break;
            }
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                analyzeExpression(subscript->base, state);
                analyzeExpression(subscript->index, state);
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
                    analyzeExpression(argument->value, state);
                }
                break;
            }
            case syntax::Kind::blockExpr:
                analyzeBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression), state);
                break;
            case syntax::Kind::ifExpr:
                analyzeIf(std::static_pointer_cast<syntax::IfExprSyntax>(expression), state);
                break;
            case syntax::Kind::returnExpr:
                analyzeExpression(
                        std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value,
                        state);
                state.reachable = false;
                break;
            case syntax::Kind::matchExpr:
                analyzeMatch(std::static_pointer_cast<syntax::MatchExprSyntax>(expression), state);
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
        const auto symbol = model->referencedSymbol(expression);
        if (!symbol.has_value() || !tracked.contains(*symbol) ||
            state.initialized.contains(*symbol)) {
            return;
        }
        const auto* declaration = model->semanticModel()->symbol(*symbol);
        diagnostics.push_back(Diagnostic {
            DiagnosticId::useBeforeInitialization,
            Severity::error,
            expression->span,
            "binding '" +
                    (declaration == nullptr ? std::string() : declaration->name) +
                    "' is used before being initialized",
        });
    }

    void analyzeBinary(
            const syntax::BinaryExprSyntax::Ptr& expression,
            InitializationState& state) {
        analyzeExpression(expression->left, state);
        if (!state.reachable) return;
        if (expression->op != nullptr && expression->op->kind == andAnd) {
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
        const auto assigned = directlyAssignedSymbol(expression->target);
        analyzeAssignmentTarget(expression->target, state);
        analyzeExpression(expression->value, state);
        if (state.reachable && assigned.has_value() && tracked.contains(*assigned)) {
            state.initialized.insert(*assigned);
        }
    }

    void analyzeAssignmentTarget(
            const syntax::ExprPtr& target,
            InitializationState& state) {
        if (target == nullptr || !state.reachable) return;
        switch (target->kind) {
            case syntax::Kind::nameExpr:
                return;
            case syntax::Kind::parenthesizedExpr:
                analyzeAssignmentTarget(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression,
                        state);
                return;
            case syntax::Kind::accessExpr:
                analyzeAssignmentTarget(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand,
                        state);
                return;
            case syntax::Kind::memberExpr:
                analyzeExpression(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(target)->base,
                        state);
                return;
            case syntax::Kind::subscriptExpr: {
                const auto subscript =
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
                analyzeExpression(subscript->base, state);
                analyzeExpression(subscript->index, state);
                return;
            }
            default:
                analyzeExpression(target, state);
                return;
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
            InitializationState& state) {
        analyzeExpression(expression->condition, state);
        if (!state.reachable) return;
        auto thenState = state;
        analyzeBlock(expression->thenBranch, thenState);
        auto elseState = state;
        if (expression->elseBranch != nullptr) {
            analyzeExpression(expression->elseBranch, elseState);
        }
        state = merge({ std::move(thenState), std::move(elseState) });
    }

    void analyzeMatch(
            const syntax::MatchExprSyntax::Ptr& expression,
            InitializationState& state) {
        analyzeExpression(expression->scrutinee, state);
        if (!state.reachable) return;
        std::vector<InitializationState> armStates;
        armStates.reserve(expression->arms.size());
        for (const auto& arm : expression->arms) {
            auto armState = state;
            std::vector<semantic::SymbolId> bindings;
            initializePattern(arm->pattern, armState, bindings);
            analyzeExpression(arm->body, armState);
            for (const auto symbol : bindings) armState.initialized.erase(symbol);
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
                state.initialized.insert(*symbol);
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

    InitializationState merge(std::vector<InitializationState> paths) const {
        InitializationState result;
        result.reachable = false;
        for (const auto& path : paths) {
            if (!path.reachable) continue;
            if (!result.reachable) {
                result = path;
                continue;
            }
            for (auto symbol = result.initialized.begin();
                 symbol != result.initialized.end();) {
                if (!path.initialized.contains(*symbol)) {
                    symbol = result.initialized.erase(symbol);
                } else {
                    ++symbol;
                }
            }
        }
        return result;
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
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
            for (const auto& item : root->items) {
                if (item->kind == syntax::Kind::functionDecl) {
                    analyzeFunction(
                            std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
                }
            }
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
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
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
