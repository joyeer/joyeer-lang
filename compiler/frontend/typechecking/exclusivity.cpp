#include "builder.h"

#include <algorithm>

namespace joyeer::typing {

void TypeCheckingBuilder::collectCallReceiverAccess(
        const syntax::CallExprSyntax::Ptr& expression,
        const semantic::Symbol* target,
        std::vector<CallAccess>& accesses,
        bool sustained) const {
    const auto receiverEffect = receiverEffectOf(target);
    if (!receiverEffect.has_value() ||
        expression->callee->kind != syntax::Kind::memberExpr) {
        collectEvaluationAccesses(expression->callee, accesses);
        return;
    }

    const auto member =
            std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
    const auto path = storagePath(member->base);
    if (path.has_value()) {
        collectSubscriptIndexAccesses(member->base, accesses);
        accesses.push_back(CallAccess {
            *path,
            *receiverEffect,
            member->base->span,
            sustained,
        });
    } else {
        collectEvaluationAccesses(member->base, accesses);
    }
}

void TypeCheckingBuilder::collectCallArgumentAccess(
        const syntax::CallArgumentSyntax& argument,
        syntax::AccessEffect effect,
        std::vector<CallAccess>& accesses,
        bool sustained) const {
    // A direct `place_address(&place)` argument holds `place` inout while the enclosing call runs, so a
    // sibling argument that reads or writes the same place conflicts (decision 0026).
    auto direct = argument.value;
    while (direct != nullptr && direct->kind == syntax::Kind::parenthesizedExpr) {
        direct = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(direct)->expression;
    }
    if (direct != nullptr && direct->kind == syntax::Kind::callExpr) {
        const auto inner = std::static_pointer_cast<syntax::CallExprSyntax>(direct);
        const auto innerTarget = model->callTarget(inner);
        const auto* innerSymbol = innerTarget.has_value() ? model->semanticModelValue->symbol(*innerTarget) : nullptr;
        const bool place = innerSymbol != nullptr && innerSymbol->name == "joyeer_intrinsic_place_address";
        const bool borrowed = innerSymbol != nullptr && innerSymbol->name == "joyeer_intrinsic_borrow_address";
        if ((place || borrowed) && innerSymbol->kind == semantic::SymbolKind::builtinFunction &&
            inner->arguments.size() == 1) {
            // `borrow_address` holds a read of its place for the call (decision 0027).
            const auto placePath = storagePath(inner->arguments[0]->value);
            if (placePath.has_value()) {
                collectSubscriptIndexAccesses(inner->arguments[0]->value, accesses);
                accesses.push_back(CallAccess { *placePath,
                        place ? syntax::AccessEffect::inout : syntax::AccessEffect::borrowing,
                        argument.value->span, true });
                return;
            }
        }
    }
    const auto path = storagePath(argument.value);
    if (path.has_value()) {
        collectSubscriptIndexAccesses(argument.value, accesses);
        accesses.push_back(CallAccess {
            *path,
            effect,
            argument.value->span,
            sustained,
        });
    } else {
        collectEvaluationAccesses(argument.value, accesses);
    }
}

std::optional<TypeCheckingBuilder::AccessPath> TypeCheckingBuilder::storagePath(const syntax::ExprPtr& expression) const {
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
        return AccessPath { *symbol, {} };
    }
    if (expression->kind == syntax::Kind::memberExpr) {
        const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
        if (referencedEnumCase(member) != nullptr) return std::nullopt;
        auto path = storagePath(member->base);
        if (!path.has_value()) return std::nullopt;
        const auto symbol = model->referencedSymbol(member);
        const auto* semanticMember = symbol.has_value()
                ? model->semanticModelValue->symbol(*symbol)
                : nullptr;
        if (semanticMember != nullptr &&
            semanticMember->kind == semantic::SymbolKind::structureField) {
            path->projections.push_back(*symbol);
        }
        return path;
    }
    if (expression->kind == syntax::Kind::subscriptExpr) {
        const auto subscript =
                std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
        auto path = storagePath(subscript->base);
        if (!path.has_value()) return std::nullopt;
        path->projections.push_back(std::nullopt);
        return path;
    }
    return std::nullopt;
}

void TypeCheckingBuilder::collectSubscriptIndexAccesses(
        const syntax::ExprPtr& expression,
        std::vector<CallAccess>& accesses) const {
    if (expression == nullptr) return;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        collectSubscriptIndexAccesses(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)
                        ->expression,
                accesses);
    } else if (expression->kind == syntax::Kind::accessExpr) {
        collectSubscriptIndexAccesses(
                std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                accesses);
    } else if (expression->kind == syntax::Kind::memberExpr) {
        collectSubscriptIndexAccesses(
                std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base,
                accesses);
    } else if (expression->kind == syntax::Kind::subscriptExpr) {
        const auto subscript =
                std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
        collectSubscriptIndexAccesses(subscript->base, accesses);
        collectEvaluationAccesses(subscript->index, accesses);
    }
}

void TypeCheckingBuilder::collectEvaluationAccesses(
        const syntax::NodePtr& expression,
        std::vector<CallAccess>& accesses) const {
    if (expression == nullptr) return;
    if (expression->kind == syntax::Kind::parenthesizedExpr) {
        collectEvaluationAccesses(
                std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression, accesses);
        return;
    }
    const auto value = isExpressionKind(expression->kind)
            ? std::static_pointer_cast<syntax::ExprSyntax>(expression)
            : nullptr;
    // `consume place` moves the place, so it conflicts with a sustained access of it, such as the borrowed left
    // operand of a non-builtin `==` (spec 20.13.2).
    if (expression->kind == syntax::Kind::accessExpr) {
        const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
        const auto moved = access->marker != nullptr && access->marker->kind == kwConsume
                ? storagePath(access->operand) : std::nullopt;
        if (moved.has_value()) {
            collectSubscriptIndexAccesses(access->operand, accesses);
            accesses.push_back(CallAccess { *moved, syntax::AccessEffect::consuming, expression->span, false });
            return;
        }
    }
    const auto path = storagePath(value);
    if (path.has_value()) {
        collectSubscriptIndexAccesses(value, accesses);
        accesses.push_back(CallAccess {
            *path,
            syntax::AccessEffect::borrowing,
            expression->span,
            false,
        });
        return;
    }
    switch (expression->kind) {
        case syntax::Kind::bindingDecl:
            collectEvaluationAccesses(
                    std::static_pointer_cast<syntax::BindingDeclSyntax>(expression)
                            ->initializer,
                    accesses);
            break;
        case syntax::Kind::whileStmt: {
            const auto loop = std::static_pointer_cast<syntax::WhileStmtSyntax>(expression);
            collectEvaluationAccesses(loop->condition, accesses);
            collectEvaluationAccesses(loop->body, accesses);
            break;
        }
        case syntax::Kind::prefixExpr:
            collectEvaluationAccesses(
                    std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand,
                    accesses);
            break;
        case syntax::Kind::accessExpr:
            collectEvaluationAccesses(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                    accesses);
            break;
        case syntax::Kind::propagateExpr:
            collectEvaluationAccesses(
                    std::static_pointer_cast<syntax::PropagateExprSyntax>(expression)->operand,
                    accesses);
            break;
        case syntax::Kind::binaryExpr: {
            const auto binary = std::static_pointer_cast<syntax::BinaryExprSyntax>(expression);
            collectEvaluationAccesses(binary->left, accesses);
            collectEvaluationAccesses(binary->right, accesses);
            break;
        }
        case syntax::Kind::assignmentExpr: {
            const auto assignment =
                    std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
            const auto target = storagePath(assignment->target);
            if (target.has_value()) {
                collectSubscriptIndexAccesses(assignment->target, accesses);
                accesses.push_back(CallAccess {
                    *target,
                    syntax::AccessEffect::inout,
                    assignment->target->span,
                    false,
                });
            } else {
                collectEvaluationAccesses(assignment->target, accesses);
            }
            collectEvaluationAccesses(assignment->value, accesses);
            break;
        }
        case syntax::Kind::memberExpr:
            if (referencedEnumCase(expression) != nullptr) break;
            collectEvaluationAccesses(
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base,
                    accesses);
            break;
        case syntax::Kind::subscriptExpr: {
            const auto subscript =
                    std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
            collectEvaluationAccesses(subscript->base, accesses);
            collectEvaluationAccesses(subscript->index, accesses);
            break;
        }
        case syntax::Kind::callExpr: {
            const auto call = std::static_pointer_cast<syntax::CallExprSyntax>(expression);
            const auto target = model->callTarget(call);
            const auto* symbol = target.has_value()
                    ? model->semanticModelValue->symbol(*target)
                    : nullptr;
            if (isEnumCase(symbol)) {
                collectEnumEvaluationAccesses(call->arguments, accesses);
                break;
            }
            collectCallReceiverAccess(call, symbol, accesses, false);
            // A call through a function value takes its effects from the value's type, by
            // position (spec 5.6.1).
            std::vector<syntax::AccessEffect> valueEffects;
            if (!target.has_value()) {
                const auto calleeType = model->typeOf(call->callee);
                const auto* record = calleeType.has_value() ? model->typeContext.type(*calleeType) : nullptr;
                if (record != nullptr && record->kind == TypeKind::function) valueEffects = record->effects;
            }
            for (size_t index = 0; index < call->arguments.size(); ++index) {
                const auto& argument = call->arguments[index];
                const auto parameterIndex = symbol == nullptr
                        ? std::optional<size_t>()
                        : parameterIndexForArgument(*symbol, *argument, index);
                auto effect = symbol != nullptr &&
                                    symbol->callable.has_value() &&
                                    parameterIndex.has_value()
                        ? symbol->callable->parameters[*parameterIndex].access
                        : index < valueEffects.size() ? valueEffects[index]
                                                      : syntax::AccessEffect::borrowing;
                if (symbol != nullptr && parameterIndex.has_value() && argument->accessMarker == nullptr &&
                    model->isOwnedArgumentContext(symbol->id, *parameterIndex)) {
                    effect = syntax::AccessEffect::borrowing;
                }
                collectCallArgumentAccess(*argument, effect, accesses, false);
            }
            break;
        }
        case syntax::Kind::arrayExpr: {
            const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
            for (const auto& element : array->elements) {
                collectEvaluationAccesses(element, accesses);
            }
            break;
        }
        case syntax::Kind::dictionaryExpr: {
            const auto dictionary =
                    std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression);
            for (const auto& entry : dictionary->entries) {
                collectEvaluationAccesses(entry->key, accesses);
                collectEvaluationAccesses(entry->value, accesses);
            }
            break;
        }
        case syntax::Kind::contextualCaseExpr: {
            const auto enumCase =
                    std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression);
            collectEnumEvaluationAccesses(enumCase->arguments, accesses);
            break;
        }
        case syntax::Kind::blockExpr: {
            const auto block =
                    std::static_pointer_cast<syntax::BlockExprSyntax>(expression);
            for (const auto& item : block->items) {
                collectEvaluationAccesses(item, accesses);
            }
            break;
        }
        case syntax::Kind::ifExpr: {
            const auto conditional =
                    std::static_pointer_cast<syntax::IfExprSyntax>(expression);
            collectEvaluationAccesses(conditional->condition, accesses);
            collectEvaluationAccesses(conditional->thenBranch, accesses);
            collectEvaluationAccesses(conditional->elseBranch, accesses);
            break;
        }
        case syntax::Kind::matchExpr: {
            const auto match = std::static_pointer_cast<syntax::MatchExprSyntax>(expression);
            collectEvaluationAccesses(match->scrutinee, accesses);
            for (const auto& arm : match->arms) {
                collectEvaluationAccesses(arm->body, accesses);
            }
            break;
        }
        case syntax::Kind::returnExpr:
            collectEvaluationAccesses(
                    std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value,
                    accesses);
            break;
        default:
            break;
    }
}

void TypeCheckingBuilder::collectEnumEvaluationAccesses(
        const std::vector<syntax::CallArgumentSyntax::Ptr>& arguments,
        std::vector<CallAccess>& accesses) const {
    for (const auto& argument : arguments) {
        const auto moved = consumesArgument(*argument) && isPlaceExpression(argument->value)
                ? storagePath(argument->value) : std::nullopt;
        if (moved.has_value()) {
            collectSubscriptIndexAccesses(argument->value, accesses);
            accesses.push_back({ *moved, syntax::AccessEffect::consuming, argument->value->span, false });
        } else {
            collectEvaluationAccesses(argument->value, accesses);
        }
    }
}

bool TypeCheckingBuilder::pathsOverlap(const AccessPath& left, const AccessPath& right) const {
    if (left.root != right.root) return false;
    const auto count = std::min(left.projections.size(), right.projections.size());
    for (size_t index = 0; index < count; ++index) {
        const auto& leftProjection = left.projections[index];
        const auto& rightProjection = right.projections[index];
        if (!leftProjection.has_value() || !rightProjection.has_value()) return true;
        if (*leftProjection != *rightProjection) return false;
    }
    return true;
}

const char* TypeCheckingBuilder::accessEffectName(syntax::AccessEffect effect) const {
    switch (effect) {
        case syntax::AccessEffect::borrowing: return "borrowing";
        case syntax::AccessEffect::inout: return "inout";
        case syntax::AccessEffect::consuming: return "consuming";
        case syntax::AccessEffect::initializing: return "initializing";
    }
    return "unknown";
}

void TypeCheckingBuilder::validateCallExclusivity(const std::vector<CallAccess>& accesses) {
    for (size_t right = 0; right < accesses.size(); ++right) {
        for (size_t left = 0; left < right; ++left) {
            if (accesses[left].effect == syntax::AccessEffect::borrowing &&
                accesses[right].effect == syntax::AccessEffect::borrowing) {
                continue;
            }
            // Transient evaluations finish before later arguments acquire storage.
            if (!accesses[left].sustained) continue;
            if (!pathsOverlap(accesses[left].path, accesses[right].path)) continue;
            const auto* root = model->semanticModelValue->symbol(accesses[right].path.root);
            report(
                    TypeCheckingDiagnosticId::overlappingAccess,
                    accesses[right].span,
                    "overlapping " +
                            std::string(accessEffectName(accesses[left].effect)) +
                            " and " + accessEffectName(accesses[right].effect) +
                            " access to '" +
                            (root == nullptr ? std::string("<storage>") : root->name) +
                            "' in the same call",
                        std::nullopt,
                        std::nullopt,
                        { DiagnosticSourceNote {
                            accesses[left].span.offset,
                            accesses[left].span.length,
                            "conflicting " +
                                std::string(accessEffectName(accesses[left].effect)) +
                                " access occurs here",
                        } });
            return;
        }
    }
}

} // namespace joyeer::typing
