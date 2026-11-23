// Top-level `let` constants: checked when collected, evaluated by the ConstantEvaluator
// (compiler/ir/constants.h), and lowered at each use. Scalars and strings fold into the use;
// arrays become shared module constants.

#include "internal.h"

#include <bit>
#include <cstdio>

namespace joyeer::lowering::detail {

bool Builder::isConstantType(typing::TypeId id) const {
    const auto* type = model->types().type(id);
    if (type == nullptr) return false;
    if (model->isArrayRoleType(id)) {
        return type->arguments.size() == 1 && isConstantType(type->arguments.front());
    }
    return typing::isNativeScalar(type->kind) || type->kind == typing::TypeKind::string;
}

void Builder::collectConstant(const syntax::BindingDeclSyntax::Ptr& declaration) {
    const auto symbol = model->semanticModel()->declaredSymbol(declaration);
    if (!symbol.has_value()) return;
    topLevelBindings.insert(*symbol);
    if (declaration->isMutable()) {
        report(DiagnosticId::invalidConstant, declaration->span,
                "top-level 'var' is not supported; declare an immutable 'let' constant");
        return;
    }
    if (declaration->annotation == nullptr) {
        report(DiagnosticId::invalidConstant, declaration->span,
                "a top-level constant requires an explicit type");
        return;
    }
    const auto type = typeOf(*symbol);
    if (declaration->initializer == nullptr || !type.has_value() || !isConstantType(*type)) {
        report(DiagnosticId::invalidConstant, declaration->span,
                "a top-level constant must be a Bool, number, String, or array of those, "
                "with an initializer");
        return;
    }
    constantDeclarations.emplace(*symbol, declaration);
    constantOrder.push_back(*symbol);
}

void Builder::evaluateConstants() {
    if (constantOrder.empty()) return;
    constantEvaluator.emplace(
            model,
            [this](semantic::SymbolId symbol) -> syntax::ExprPtr {
                const auto found = constantDeclarations.find(symbol);
                return found == constantDeclarations.end() ? nullptr : found->second->initializer;
            },
            [this](SourceSpan span, std::string message) {
                report(DiagnosticId::invalidConstant, span, std::move(message));
            });
    // Evaluate every constant, used or not, so an invalid one is always reported.
    for (const auto symbol : constantOrder) {
        const auto& declaration = constantDeclarations.at(symbol);
        const auto value = constantEvaluator->evaluateSymbol(symbol, declaration->span);
        const auto type = typeOf(symbol);
        // Encoding enforces the size limit, so an oversized table fails here once.
        if (!value.has_value() ||
            (model->isArrayRoleType(*type) &&
             !constantEvaluator->encode(*value, *type, declaration->span).has_value())) {
            failedConstants.insert(symbol);
        }
    }
}

std::optional<ir::Value> Builder::lowerConstant(semantic::SymbolId symbol, SourceSpan span) {
    const auto type = typeOf(symbol);
    if (!type.has_value()) {
        report(DiagnosticId::missingType, span, "constant has no resolved type");
        return std::nullopt;
    }
    if (failedConstants.contains(symbol)) return std::nullopt;
    const auto value = constantEvaluator->evaluateSymbol(symbol, span);
    if (!value.has_value()) return std::nullopt;

    const auto kind = model->types().type(*type)->kind;
    auto instruction = makeInstruction(ir::Opcode::literal, span);
    if (model->isArrayRoleType(*type)) {
        auto found = constantIds.find(symbol);
        if (found == constantIds.end()) {
            const auto encoded = constantEvaluator->encode(*value, *type, span);
            if (!encoded.has_value()) {
                failedConstants.insert(symbol);
                return std::nullopt;
            }
            ir::Constant constant;
            constant.id = static_cast<uint32_t>(module->constants.size());
            constant.symbol = symbol;
            constant.type = *type;
            constant.name = model->semanticModel()->symbol(symbol)->name;
            constant.data = *encoded;
            module->constants.push_back(std::move(constant));
            found = constantIds.emplace(symbol, module->constants.back().id).first;
        }
        instruction.opcode = ir::Opcode::constantValue;
        instruction.integerValue = found->second;
    } else if (kind == typing::TypeKind::string) {
        instruction.opcode = ir::Opcode::stringConstant;
        instruction.text = value->text;
    } else if (typing::isFloating(kind)) {
        instruction.opcode = ir::Opcode::floatingConstant;
        char text[40] = {};
        std::snprintf(text, sizeof(text), kind == typing::TypeKind::float32 ? "%.9g" : "%.17g",
                value->floating);
        instruction.text = text;
    } else if (kind == typing::TypeKind::boolean) {
        instruction.opcode = ir::Opcode::literal;
        instruction.integerValue = static_cast<int64_t>(value->integer);
    } else {
        const auto width = typing::scalarBitWidth(kind);
        auto bits = value->integer;
        if (typing::isSignedInteger(kind) && width < 64 && ((bits >> (width - 1)) & 1) != 0) {
            bits |= ~((uint64_t{1} << width) - 1);
        }
        instruction.opcode = ir::Opcode::literal;
        instruction.integerValue = std::bit_cast<int64_t>(bits);
    }
    instruction.result = makeValue(*type, ir::ValueCategory::value);
    const auto result = *instruction.result;
    emit(std::move(instruction));
    // Constants are never owned: every use borrows, and an owned use copies.
    recordValue(result, model->isArrayRoleType(*type) || kind == typing::TypeKind::string
            ? ValueOwnership::borrowed : ValueOwnership::trivial);
    return result;
}

} // namespace joyeer::lowering::detail
