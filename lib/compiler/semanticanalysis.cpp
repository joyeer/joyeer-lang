#include "joyeer/compiler/semanticanalysis.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <utility>

namespace joyeer::analysis {

namespace {

class Builder {
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

} // namespace

bool Result::succeeded() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == Severity::error;
    });
}

Result Analyzer::analyze(const typing::TypeCheckedModel::Ptr& model) const {
    return Builder().build(model);
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::missingReturn: return "semantic-analysis.missing-return";
        case DiagnosticId::unreachableCode: return "semantic-analysis.unreachable-code";
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
