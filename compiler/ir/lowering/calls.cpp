// Calls: the function a direct call reaches (a function, a generic instance or a protocol
// witness), calls through function values, the per-parameter argument conventions, and struct
// construction through the synthesized initializer.

#include "internal.h"

#include "compiler/frontend/intrinsics.h"

#include <algorithm>
#include <iterator>

namespace joyeer::lowering::detail {

std::optional<ir::Value> Builder::lowerCall(const syntax::CallExprSyntax::Ptr& expression) {
    const auto target = model->callTarget(expression);
    if (!target.has_value()) {
        const auto calleeType = typeOf(expression->callee);
        const auto* record = calleeType.has_value() ? model->types().type(*calleeType) : nullptr;
        if (record != nullptr && record->kind == typing::TypeKind::function) {
            return lowerFunctionValueCall(expression, *calleeType);
        }
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "call target has no resolved symbol");
        return std::nullopt;
    }

    const auto* targetSymbol = model->semanticModel()->symbol(*target);
    if (targetSymbol != nullptr &&
        targetSymbol->kind == semantic::SymbolKind::synthesizedInitializer) {
        return lowerStructConstruction(expression, *target);
    }
    if (targetSymbol != nullptr &&
        (targetSymbol->kind == semantic::SymbolKind::enumCase ||
         targetSymbol->kind == semantic::SymbolKind::builtinEnumCase)) {
        const auto type = typeOf(expression);
        if (!type.has_value()) return std::nullopt;
        return lowerEnumArguments(
                *type, *target, expression->arguments, expression->span);
    }
    if (targetSymbol != nullptr &&
        targetSymbol->kind == semantic::SymbolKind::builtinFunction &&
        intrinsic::isLayoutIntrinsic(targetSymbol->name)) {
        return lowerLayoutIntrinsic(expression, targetSymbol->name);
    }
    if (targetSymbol != nullptr &&
        targetSymbol->kind == semantic::SymbolKind::builtinMember &&
        targetSymbol->name == "utf8") {
        return lowerStringUtf8(expression);
    }
    if (const auto builtin = lowerBuiltinEqualsRequirement(expression, targetSymbol)) return *builtin;
    const auto calleeId = calledFunction(expression, *target, targetSymbol);
    std::vector<ir::Parameter> divergingParameters;
    if (!calleeId.has_value()) {
        // Evaluate the checked operands with normal ownership even when no call can be emitted.
        const auto* generic = model->genericCall(expression);
        const auto* declared = model->callable(*target);
        const auto* parameters = generic == nullptr ? nullptr : model->genericParameters(generic->callee);
        const bool diverges = std::any_of(expression->arguments.begin(), expression->arguments.end(),
                [&](const auto& argument) { return typeOf(argument->value) == model->types().neverType(); });
        if (!diverges || targetSymbol == nullptr || !targetSymbol->callable.has_value() ||
            declared == nullptr || parameters == nullptr || parameters->size() != generic->arguments.size() ||
            declared->parameters.size() != targetSymbol->callable->parameters.size()) {
            report(DiagnosticId::missingSymbol, expression->span, "call target has no IR function declaration");
            return std::nullopt;
        }
        std::unordered_map<semantic::SymbolId, typing::TypeId> substitution;
        for (size_t index = 0; index < parameters->size(); ++index) {
            substitution.emplace((*parameters)[index], substituted(generic->arguments[index]));
        }
        const auto add = [&](typing::TypeId type, syntax::AccessEffect effect) {
            ir::Parameter parameter;
            parameter.value.type = model->types().substitute(type, substitution);
            parameter.value.category = effect == syntax::AccessEffect::inout ||
                    effect == syntax::AccessEffect::initializing
                    ? ir::ValueCategory::address : ir::ValueCategory::value;
            parameter.isConsuming = effect == syntax::AccessEffect::consuming;
            divergingParameters.push_back(parameter);
        };
        if (declared->receiverType.has_value() && targetSymbol->callable->receiver.has_value()) {
            add(*declared->receiverType, targetSymbol->callable->receiver->access);
        }
        for (size_t index = 0; index < declared->parameters.size(); ++index) {
            add(declared->parameters[index], targetSymbol->callable->parameters[index].access);
        }
    }

    // A method's receiver is the callee's first IR parameter, ahead of the
    // written arguments, and follows the same access convention as any other.
    struct Operand {
        syntax::ExprPtr value;
        SourceSpan span;
    };
    std::vector<Operand> operands;
    if (targetSymbol != nullptr && targetSymbol->kind == semantic::SymbolKind::method) {
        if (expression->callee->kind != syntax::Kind::memberExpr) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "a method call requires a receiver expression");
            return std::nullopt;
        }
        const auto member =
                std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
        operands.push_back({member->base, member->base->span});
    }
    for (const auto& argument : expression->arguments) {
        operands.push_back({argument->value, argument->span});
    }

    std::vector<ir::ValueId> arguments;
    std::vector<ir::Value> consumedArguments;
    const auto& parameters = calleeId.has_value() ? module->functions[*calleeId].parameters : divergingParameters;
    const size_t receivers = targetSymbol != nullptr && targetSymbol->kind == semantic::SymbolKind::method ? 1 : 0;
    for (size_t index = 0; index < operands.size(); ++index) {
        const auto& operand = operands[index];
        const auto* parameter = index < parameters.size() ? &parameters[index] : nullptr;
        const auto value = lowerCallOperand(
                operand.value,
                operand.span,
                parameter != nullptr && parameter->value.category == ir::ValueCategory::address,
                parameter != nullptr && parameter->isConsuming,
                parameter != nullptr && !parameter->acceptsAnyType
                        ? std::optional<typing::TypeId>(parameter->value.type)
                        : std::nullopt,
                consumedArguments,
                index < receivers || !model->isOwnedArgumentContext(*target, index - receivers) ||
                        movesArgument(*expression->arguments[index - receivers]));
        if (!value.has_value()) return std::nullopt;
        arguments.push_back(value->id);
    }
    if (!calleeId.has_value()) {
        report(DiagnosticId::unsupportedSyntax, expression->span, "a diverging call argument unexpectedly returned");
        return std::nullopt;
    }
    auto instruction = makeInstruction(ir::Opcode::call, expression->span);
    instruction.callee = *calleeId;
    return finishCall(expression, std::move(instruction), std::move(arguments), consumedArguments);
}

// The IR function of `requirement`'s witness for `base`: an instance when the witness is declared in a generic
// type (decision 0022), else the witness's own function.
std::optional<ir::FunctionId> Builder::witnessFunction(typing::TypeId base, semantic::SymbolId requirement) {
    const auto witness = model->witness(base, requirement);
    if (!witness.has_value()) return std::nullopt;
    if (model->genericParameters(*witness) != nullptr) {
        const auto* receiver = model->types().type(base);
        if (receiver == nullptr) return std::nullopt;
        const auto instance = model->instanceIndex(*witness, receiver->arguments);
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            return std::nullopt;
        }
        return instanceFunctions[*instance];
    }
    const auto found = functions.find(*witness);
    if (found == functions.end()) return std::nullopt;
    return found->second;
}

// The IR function a direct call reaches: an instance for a call of a generic function, the
// witness for a requirement call on a type parameter (decision 0021), else the target.
std::optional<ir::FunctionId> Builder::calledFunction(
        const syntax::CallExprSyntax::Ptr& expression,
        semantic::SymbolId target,
        const semantic::Symbol* targetSymbol) {
    if (const auto* call = model->genericCall(expression)) {
        std::vector<typing::TypeId> arguments;
        for (const auto argument : call->arguments) arguments.push_back(substituted(argument));
        const auto instance = model->instanceIndex(call->callee, arguments);
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            return std::nullopt;
        }
        return instanceFunctions[*instance];
    }
    const auto* owner = targetSymbol != nullptr && targetSymbol->containingSymbol.has_value()
            ? model->semanticModel()->symbol(*targetSymbol->containingSymbol)
            : nullptr;
    if (owner != nullptr && owner->kind == semantic::SymbolKind::protocol &&
        expression->callee->kind == syntax::Kind::memberExpr) {
        const auto base = typeOf(std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base);
        return base.has_value() ? witnessFunction(*base, target) : std::nullopt;
    }
    return demandFunction(target);
}

// One argument of a call: an address for `inout` and `initializing`, a moved value for `consuming`,
// otherwise a value coerced to the parameter type. An owned consumed value is added to
// `consumedArguments`, to be acquired once every argument is evaluated.
std::optional<ir::Value> Builder::lowerCallOperand(
        const syntax::ExprPtr& operand,
        SourceSpan span,
        bool expectsAddress,
        bool consumesValue,
        std::optional<typing::TypeId> parameterType,
        std::vector<ir::Value>& consumedArguments,
        bool movesSource) {
    std::optional<ir::Value> value;
    if (expectsAddress) {
        value = lowerAddress(operand);
    } else if (consumesValue && movesSource && isAddressable(operand)) {
        const auto address = lowerAddress(operand);
        if (address.has_value()) {
            value = emitValue(
                    ir::Opcode::take,
                    address->type,
                    ir::ValueCategory::value,
                    { address->id },
                    span);
        }
    } else {
        value = lowerExpression(operand);
    }
    if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
    if (!expectsAddress && parameterType.has_value()) {
        value = coerce(*value, *parameterType, operand->span);
        if (!value.has_value()) return std::nullopt;
    }
    if (consumesValue && requiresDestroy(value->type)) {
        value = prepareOwned(*value, span);
        if (!value.has_value()) return std::nullopt;
        consumedArguments.push_back(*value);
    }
    return value;
}

// `f(a, &b)` or `(s.f)(a)` (spec 5.6.1): the callee is evaluated first to a copy, then the arguments
// by the function type's effects.
std::optional<ir::Value> Builder::lowerFunctionValueCall(
        const syntax::CallExprSyntax::Ptr& expression,
        typing::TypeId calleeType) {
    const auto* record = model->types().type(calleeType);
    const auto parameters = record->arguments;
    const auto effects = record->effects;
    const auto callee = lowerExpression(expression->callee);
    if (!callee.has_value()) return std::nullopt;
    std::vector<ir::ValueId> arguments { callee->id };
    std::vector<ir::Value> consumedArguments;
    for (size_t index = 0; index < expression->arguments.size() && index < effects.size(); ++index) {
        const auto& argument = expression->arguments[index];
        const auto effect = effects[index];
        const auto value = lowerCallOperand(
                argument->value,
                argument->span,
                effect == syntax::AccessEffect::inout || effect == syntax::AccessEffect::initializing,
                effect == syntax::AccessEffect::consuming,
                parameters[index],
                consumedArguments);
        if (!value.has_value()) return std::nullopt;
        arguments.push_back(value->id);
    }
    return finishCall(
            expression,
            makeInstruction(ir::Opcode::callIndirect, expression->span),
            std::move(arguments),
            consumedArguments);
}

// Emits a call whose operands are lowered: acquires the consumed arguments, then gives the result
// a value, or ends the block after a `Never` call.
std::optional<ir::Value> Builder::finishCall(
        const syntax::CallExprSyntax::Ptr& expression,
        ir::Instruction instruction,
        std::vector<ir::ValueId> arguments,
        const std::vector<ir::Value>& consumedArguments) {
    for (const auto& argument : consumedArguments) {
        if (!acquireOwned(argument, expression->span).has_value()) return std::nullopt;
    }
    instruction.operands = std::move(arguments);
    const auto type = typeOf(expression);
    if (type.has_value() && *type != model->types().voidType() &&
        *type != model->types().neverType()) {
        instruction.result = makeValue(*type, ir::ValueCategory::value);
    }
    const auto result = instruction.result;
    emit(std::move(instruction));
    if (type == model->types().neverType()) {
        emit(makeInstruction(ir::Opcode::unreachable, expression->span));
        return std::nullopt;
    }
    if (result.has_value()) {
        recordValue(
            *result,
            requiresDestroy(result->type)
                ? ValueOwnership::owned
                : ValueOwnership::trivial);
    }
    if (type == model->types().voidType()) return emitUnit(expression->span);
    return result;
}

std::optional<ir::Value> Builder::lowerStructConstruction(
        const syntax::CallExprSyntax::Ptr& expression,
        semantic::SymbolId initializer) {
    const auto* initializerSymbol = model->semanticModel()->symbol(initializer);
    const auto type = typeOf(expression);
    if (initializerSymbol == nullptr || !initializerSymbol->containingSymbol.has_value() ||
        !type.has_value()) {
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "struct initializer has no containing structure or result type");
        return std::nullopt;
    }
    const auto structure = std::find_if(
            module->structures.begin(),
            module->structures.end(),
            [initializerSymbol, &type](const auto& candidate) {
                return candidate.symbol == *initializerSymbol->containingSymbol &&
                        candidate.type == *type;
            });
    if (structure == module->structures.end()) {
        report(
                DiagnosticId::missingSymbol,
                expression->span,
                "struct initializer has no IR structure definition");
        return std::nullopt;
    }

    std::vector<std::optional<ir::Value>> fieldValues(structure->fields.size());
    for (const auto& argument : expression->arguments) {
        if (argument->label == nullptr) continue;
        const auto field = std::find_if(
                structure->fields.begin(),
                structure->fields.end(),
                [&argument](const auto& candidate) {
                    return candidate.name == argument->label->rawValue;
                });
        if (field == structure->fields.end()) continue;
        const auto value = prepareOperand(
                argument->value, field->type, movesArgument(*argument));
        if (!value.has_value()) return std::nullopt;
        fieldValues[static_cast<size_t>(
                std::distance(structure->fields.begin(), field))] = *value;
    }

    for (size_t index = 0; index < fieldValues.size(); ++index) {
        if (fieldValues[index].has_value()) continue;
        if (structure->fields[index].name == ir::liveFieldName) {
            auto live = makeInstruction(ir::Opcode::literal, expression->span, true);
            live.integerValue = 1;
            live.result = makeValue(structure->fields[index].type, ir::ValueCategory::value);
            fieldValues[index] = *live.result;
            emit(std::move(live));
            recordValue(*fieldValues[index], ValueOwnership::trivial);
            continue;
        }
        const auto* fieldSymbol = model->semanticModel()->symbol(
                structure->fields[index].symbol);
        const auto& declaration = fieldSymbol != nullptr &&
                fieldSymbol->declaration.has_value()
                ? model->semanticModel()->node(*fieldSymbol->declaration)
                : syntax::NodePtr();
        if (declaration == nullptr ||
            declaration->kind != syntax::Kind::structFieldDecl) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "omitted struct field has no default initializer");
            return std::nullopt;
        }
        const auto fieldDeclaration =
                std::static_pointer_cast<syntax::StructFieldDeclSyntax>(declaration);
        const auto value = prepareOperand(
                fieldDeclaration->initializer, structure->fields[index].type);
        if (!value.has_value()) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    fieldDeclaration->span,
                    "struct field default could not be lowered");
            return std::nullopt;
        }
        fieldValues[index] = *value;
    }

    auto instruction = makeInstruction(ir::Opcode::constructStruct, expression->span);
    instruction.result = makeValue(*type, ir::ValueCategory::value);
    instruction.symbol = structure->symbol;
    for (size_t index = 0; index < fieldValues.size(); ++index) {
        const auto value = acquireOwned(*fieldValues[index], expression->span);
        if (!value.has_value()) return std::nullopt;
        instruction.operands.push_back(value->id);
    }
    const auto result = *instruction.result;
    emit(std::move(instruction));
    recordValue(result, ValueOwnership::owned);
    return result;
}

} // namespace joyeer::lowering::detail
