// Calls that lower to dedicated instructions instead of a call: the layout intrinsics (spec
// 20.12.5) the library collections are written with, and `String.utf8`.

#include "internal.h"

namespace joyeer::lowering::detail {

// The layout intrinsics (spec 20.12.5). The size and alignment are queries of the backend's
// layout; the other three treat an `Int` as the address of a `T` slot. The type argument is
// substituted for the current instance, so a generic function gets the layout of its `T`.
std::optional<ir::Value> Builder::lowerLayoutIntrinsic(
        const syntax::CallExprSyntax::Ptr& expression, const std::string& name) {
    if (name == "joyeer_intrinsic_callback_address") {
        const auto* reference = model->genericCall(expression);
        if (reference == nullptr) {
            report(DiagnosticId::missingSymbol, expression->span, "callback has no typed function reference");
            return std::nullopt;
        }
        std::vector<typing::TypeId> arguments;
        for (const auto argument : reference->arguments) arguments.push_back(substituted(argument));
        const auto instance = model->instanceIndex(reference->callee, arguments);
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            report(DiagnosticId::missingSymbol, expression->span, "callback has no lowered instance");
            return std::nullopt;
        }
        auto instruction = makeInstruction(ir::Opcode::functionAddress, expression->span);
        instruction.callee = instanceFunctions[*instance];
        instruction.result = makeValue(model->types().functionType(
                { model->types().intType() }, { syntax::AccessEffect::consuming }, model->types().voidType()),
                ir::ValueCategory::value);
        const auto address = *instruction.result;
        emit(std::move(instruction));
        recordValue(address, ValueOwnership::trivial);
        return emitValue(ir::Opcode::pointerToInteger, model->types().intType(), ir::ValueCategory::value,
                         { address.id }, expression->span);
    }
    const auto written = model->layoutIntrinsicType(expression);
    if (!written.has_value()) {
        report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' needs one type argument");
        return std::nullopt;
    }
    const auto concrete = substituted(*written);
    // An uninhabited element has no slots. Generic code can describe its empty storage, but cannot load a value.
    if (concrete == model->types().neverType() && name == "joyeer_intrinsic_move_out") {
        emit(makeInstruction(ir::Opcode::unreachable, expression->span));
        return std::nullopt;
    }
    const auto type = concrete == model->types().neverType() ? model->types().voidType() : concrete;
    if (name == "joyeer_intrinsic_stack_slot") {
        if (type == model->types().voidType()) {
            return emitValue(ir::Opcode::literal, model->types().intType(),
                             ir::ValueCategory::value, {}, expression->span);
        }
        const auto slot = emitValue(ir::Opcode::stackAllocate, type, ir::ValueCategory::address,
                                    {}, expression->span);
        return emitValue(ir::Opcode::pointerToInteger, model->types().intType(), ir::ValueCategory::value,
                         { slot.id }, expression->span);
    }
    if (name == "joyeer_intrinsic_needs_destroy") {
        auto instruction = makeInstruction(ir::Opcode::literal, expression->span);
        instruction.integerValue = requiresDestroy(type) ? 1 : 0;
        instruction.result = makeValue(model->types().boolType(), ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }
    if (name == "joyeer_intrinsic_clone_callback" || name == "joyeer_intrinsic_destroy_callback") {
        // A callback only exists for a type that needs one; otherwise the answer is 0 (decision 0025).
        const bool clone = name == "joyeer_intrinsic_clone_callback";
        if (clone ? !model->isCopyable(type) : model->isLinear(type)) {
            return emitValue(ir::Opcode::literal, model->types().intType(), ir::ValueCategory::value,
                             {}, expression->span);
        }
        auto instruction = makeInstruction(ir::Opcode::typeHelper, expression->span);
        instruction.modifier = static_cast<uint8_t>(clone ? ir::TypeHelperKind::clone : ir::TypeHelperKind::destroy);
        instruction.integerValue = static_cast<int64_t>(type);
        instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }
    if (name == "joyeer_intrinsic_key_kind") {
        // The runtime's key kinds (lib/Runtime/core/dictionaries.joyeer); any other type is an error, never 0,
        // which would select the runtime's byte comparison (decision 0027).
        int64_t kind = 0;
        switch (model->types().type(type)->kind) {
            case typing::TypeKind::integer: kind = 1; break;
            case typing::TypeKind::boolean: kind = 2; break;
            case typing::TypeKind::string: kind = 3; break;
            case typing::TypeKind::uint8: kind = 4; break;
            default: break;
        }
        if (kind == 0) {
            report(DiagnosticId::unsupportedSyntax, expression->span,
                   "'" + name + "' needs a dictionary key type, but '" + model->types().displayName(type) +
                           "' is not one");
            return std::nullopt;
        }
        auto constant = makeInstruction(ir::Opcode::literal, expression->span);
        constant.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        constant.integerValue = kind;
        const auto result = *constant.result;
        emit(std::move(constant));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }
    if (name == "joyeer_intrinsic_place_address" || name == "joyeer_intrinsic_borrow_address") {
        if (expression->arguments.size() != 1) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes a place");
            return std::nullopt;
        }
        const auto place = lowerAddress(expression->arguments[0]->value);
        if (!place.has_value()) return std::nullopt;
        return emitValue(ir::Opcode::pointerToInteger, model->types().intType(), ir::ValueCategory::value,
                         { place->id }, expression->span);
    }
    if (name == "joyeer_intrinsic_stack_block") {
        if (expression->arguments.size() != 1) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes a count");
            return std::nullopt;
        }
        const auto count = lowerExpression(expression->arguments[0]->value);
        if (!count.has_value()) return std::nullopt;
        auto instruction = makeInstruction(ir::Opcode::stackBlock, expression->span);
        instruction.integerValue = static_cast<int64_t>(type);
        instruction.operands = { count->id };
        instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }
    if (name == "joyeer_intrinsic_slot_address") {
        if (expression->arguments.size() != 2) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes a base and an index");
            return std::nullopt;
        }
        std::vector<ir::ValueId> operands;
        for (const auto& argument : expression->arguments) {
            auto value = lowerExpression(argument->value);
            if (!value.has_value()) return std::nullopt;
            value = coerce(*value, model->types().intType(), argument->value->span);
            if (!value.has_value()) return std::nullopt;
            operands.push_back(value->id);
        }
        auto instruction = makeInstruction(ir::Opcode::slotAddress, expression->span);
        instruction.integerValue = static_cast<int64_t>(type);
        instruction.operands = std::move(operands);
        instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }
    if (name == "joyeer_intrinsic_destroy_range" || name == "joyeer_intrinsic_clone_at") {
        if (expression->arguments.size() != 2) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes two Int arguments");
            return std::nullopt;
        }
        std::vector<ir::ValueId> operands;
        for (const auto& argument : expression->arguments) {
            auto value = lowerExpression(argument->value);
            if (!value.has_value()) return std::nullopt;
            value = coerce(*value, model->types().intType(), argument->value->span);
            if (!value.has_value()) return std::nullopt;
            operands.push_back(value->id);
        }
        if (name == "joyeer_intrinsic_destroy_range" && model->isLinear(type)) {
            // A linear element is never destroyed: a nonempty range is a defect (decision 0025).
            const auto zero = emitValue(ir::Opcode::literal, model->types().intType(), ir::ValueCategory::value,
                                        {}, expression->span);
            const auto nonempty = emitValue(ir::ComparePredicate::notEqual, model->types().boolType(),
                                            ir::ValueCategory::value, { operands[1], zero.id }, expression->span);
            auto guard = makeInstruction(ir::Opcode::trapIf, expression->span);
            guard.operands = { nonempty.id };
            guard.text = "linear container dropped while not empty";
            emit(std::move(guard));
            return emitUnit(expression->span);
        }
        if (name == "joyeer_intrinsic_destroy_range" && !requiresDestroy(type)) return emitUnit(expression->span);
        auto instruction = makeInstruction(
                name == "joyeer_intrinsic_clone_at" ? ir::Opcode::cloneAt : ir::Opcode::destroyRange,
                expression->span);
        instruction.integerValue = static_cast<int64_t>(type);
        instruction.operands = std::move(operands);
        emit(std::move(instruction));
        return emitUnit(expression->span);
    }
    if (name == "joyeer_intrinsic_size_of" || name == "joyeer_intrinsic_align_of") {
        auto instruction = makeInstruction(
                name == "joyeer_intrinsic_size_of" ? ir::Opcode::typeSize : ir::Opcode::typeAlignment,
                expression->span);
        instruction.integerValue = static_cast<int64_t>(type);
        instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }
    if (expression->arguments.empty()) {
        report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes an address");
        return std::nullopt;
    }
    auto address = lowerExpression(expression->arguments[0]->value);
    if (!address.has_value()) return std::nullopt;
    address = coerce(*address, model->types().intType(), expression->arguments[0]->value->span);
    if (!address.has_value()) return std::nullopt;
    const auto slot = emitValue(
            ir::Opcode::integerAddress, type, ir::ValueCategory::address, { address->id }, expression->span);
    if (name == "joyeer_intrinsic_move_out") {
        return emitValue(ir::Opcode::take, type, ir::ValueCategory::value, { slot.id }, expression->span);
    }
    if (name == "joyeer_intrinsic_destroy_at") {
        emitDestroyStorage(slot, expression->span);
        return emitUnit(expression->span);
    }
    if (expression->arguments.size() != 2) {
        report(DiagnosticId::unsupportedSyntax, expression->span, "move_in takes an address and a value");
        return std::nullopt;
    }
    const auto& argument = expression->arguments[1];
    std::optional<ir::Value> value;
    if (isAddressable(argument->value)) {
        const auto source = lowerAddress(argument->value);
        if (source.has_value()) {
            value = emitValue(ir::Opcode::take, source->type, ir::ValueCategory::value, { source->id },
                              argument->span);
        }
    } else {
        value = lowerExpression(argument->value);
    }
    if (!value.has_value()) return std::nullopt;
    value = coerce(*value, type, argument->value->span);
    if (!value.has_value()) return std::nullopt;
    if (requiresDestroy(value->type)) {
        value = prepareOwned(*value, argument->span);
        if (!value.has_value()) return std::nullopt;
        if (!acquireOwned(*value, expression->span).has_value()) return std::nullopt;
    }
    emitRawStore(*value, slot, expression->span);
    return emitUnit(expression->span);
}

std::optional<ir::Value> Builder::lowerStringUtf8(
        const syntax::CallExprSyntax::Ptr& expression) {
    if (expression->callee->kind != syntax::Kind::memberExpr ||
        !expression->arguments.empty()) {
        report(
                DiagnosticId::unsupportedSyntax,
                expression->span,
                "String.utf8 requires no arguments");
        return std::nullopt;
    }
    const auto member =
            std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
    const auto string = lowerExpression(member->base);
    const auto type = typeOf(expression);
    if (!string.has_value() || !type.has_value()) return std::nullopt;

    // The library array's `string_utf8` method, called on an inert zero `[UInt8]` like a literal method
    // (decision 0028 stage 3); the String is borrowed.
    const auto method = model->arrayRoleMethod(*type, semantic::LibraryRole::stringUtf8);
    const auto* record = model->types().type(*type);
    const auto instance = method.has_value() && record != nullptr
            ? model->instanceIndex(*method, record->arguments) : std::nullopt;
    if (!instance.has_value() || *instance >= instanceFunctions.size() ||
        instanceFunctions[*instance] == ir::invalidFunctionId) {
        report(DiagnosticId::missingSymbol, expression->span, "String.utf8 has no lowered array method");
        return std::nullopt;
    }
    const auto receiverStorage = emitValue(ir::Opcode::stackAllocate, *type, ir::ValueCategory::address,
                                          {}, expression->span);
    auto zero = makeInstruction(ir::Opcode::zeroInitialize, expression->span);
    zero.operands = { receiverStorage.id };
    emit(std::move(zero));
    const auto receiver = emitValue(ir::Opcode::load, *type, ir::ValueCategory::value,
                                   { receiverStorage.id }, expression->span);
    recordValue(receiver, ValueOwnership::owned);
    const auto ownedReceiver = acquireOwned(receiver, expression->span);
    if (!ownedReceiver.has_value()) return std::nullopt;
    auto call = makeInstruction(ir::Opcode::call, expression->span);
    call.callee = instanceFunctions[*instance];
    call.operands = { ownedReceiver->id, string->id };
    call.result = makeValue(*type, ir::ValueCategory::value);
    const auto result = *call.result;
    emit(std::move(call));
    recordValue(result, ValueOwnership::owned);
    return result;
}

} // namespace joyeer::lowering::detail
