// Library arrays and dictionaries (decisions 0025, 0026, 0027): literals built in a stack block
// and passed to the role's literal method, element reads and addresses through the role's
// addressor, and `&d[k] = v` through `dict_set`. Also String byte subscripts.

#include "internal.h"

namespace joyeer::lowering::detail {

// `&d[k] = v` on a library dictionary (decision 0027): insert or replace through `dict_set`. The base's own
// index operands, then the key, then the value are evaluated (spec 4.4.3); a base key is owned when a later
// operand may disturb the binding it was read from. Only then is the base addressed, so the right side cannot
// leave that address dangling, and a missing outer key traps after the value was evaluated. A trap in the right
// side inserts nothing.
std::optional<ir::Value> Builder::lowerDictionarySet(
        const syntax::AssignmentExprSyntax::Ptr& expression,
        const syntax::SubscriptExprSyntax::Ptr& subscript,
        typing::TypeId dictionaryType) {
    const auto* record = model->types().type(dictionaryType);
    const auto method = model->collectionRoleMethod(dictionaryType, semantic::LibraryRole::dictSet);
    const auto instance = method.has_value() && record != nullptr
            ? model->instanceIndex(*method, record->arguments) : std::nullopt;
    if (!instance.has_value() || *instance >= instanceFunctions.size() ||
        instanceFunctions[*instance] == ir::invalidFunctionId) {
        report(DiagnosticId::missingSymbol, expression->span, "the dictionary insertion has no lowered instance");
        return std::nullopt;
    }
    const auto keyType = record->arguments[0];
    const auto valueType = record->arguments[1];
    const bool ownKeys = !targetIndexesCannotDisturb(subscript) || !cannotDisturb(expression->value);
    auto base = prepareAddress(subscript->base, ownKeys);
    if (!base.has_value() || currentBlockTerminated()) return std::nullopt;
    auto key = prepareOperand(subscript->index, keyType);
    if (!key.has_value() || currentBlockTerminated()) return std::nullopt;
    auto value = prepareOperand(expression->value, valueType);
    if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto dictionary = (*base)();
    if (!dictionary.has_value()) return std::nullopt;
    auto call = makeInstruction(ir::Opcode::call, expression->span);
    call.callee = instanceFunctions[*instance];
    call.operands = { dictionary->id };
    if (!transferOperands(call, { *key, *value }, expression->span)) return std::nullopt;
    emit(std::move(call));
    return emitUnit(expression->span);
}

// Whether `type` is subscripted through a role addressor: a library array or dictionary.
bool Builder::isRoleSubscriptBase(typing::TypeId type) const {
    return model->isArrayRoleType(type) || model->types().isDictionary(type);
}

// `a[i]` on a library array (decision 0025) and `d[k]` on a library dictionary (decision 0027): the addressor of
// the receiver's instance returns the element's address as an `Int`, which only this lowering turns into a typed
// place. A dictionary key is passed raw as the borrowed argument; an owned key temporary keeps its own cleanup.
std::optional<ir::Value> Builder::emitRoleElementAddress(
        typing::TypeId arrayType, const ir::Value& baseAddress, const ir::Value& index,
        typing::TypeId elementType, SourceSpan span) {
    const auto method = model->collectionRoleMethod(arrayType, model->types().isDictionary(arrayType)
            ? semantic::LibraryRole::dictElement : semantic::LibraryRole::arrayElement);
    const auto* record = model->types().type(arrayType);
    const auto instance = method.has_value() && record != nullptr
            ? model->instanceIndex(*method, record->arguments) : std::nullopt;
    if (!instance.has_value() || *instance >= instanceFunctions.size() ||
        instanceFunctions[*instance] == ir::invalidFunctionId) {
        report(DiagnosticId::missingSymbol, span, "the element addressor has no lowered instance");
        return std::nullopt;
    }
    auto call = makeInstruction(ir::Opcode::call, span);
    call.callee = instanceFunctions[*instance];
    // A borrowing receiver is passed as a value that aliases the array's storage.
    auto receiver = baseAddress;
    if (module->functions[*call.callee].parameters.at(0).value.category == ir::ValueCategory::value) {
        receiver = emitValue(ir::Opcode::load, arrayType, ir::ValueCategory::value, { baseAddress.id }, span);
    }
    call.operands = { receiver.id, index.id };
    call.result = makeValue(model->types().intType(), ir::ValueCategory::value);
    const auto address = *call.result;
    emit(std::move(call));
    recordValue(address, ValueOwnership::trivial);
    if (elementType == model->types().neverType()) {
        emit(makeInstruction(ir::Opcode::unreachable, span));
        return std::nullopt;
    }
    return emitValue(ir::Opcode::integerAddress, elementType, ir::ValueCategory::address, { address.id }, span);
}

std::optional<ir::Value> Builder::lowerSubscript(
        const syntax::SubscriptExprSyntax::Ptr& expression) {
    if (const auto baseType = typeOf(expression->base); baseType.has_value() && isRoleSubscriptBase(*baseType)) {
        std::optional<ir::Value> baseAddress;
        if (isAddressable(expression->base) && !cannotDisturb(expression->index)) {
            // The index may reallocate the base's storage or reassign one of its keys: the base's indexes are
            // evaluated (and owned), then this index, and only then is the base addressed.
            const auto deferred = prepareAddress(expression->base, true);
            if (!deferred.has_value() || currentBlockTerminated()) return std::nullopt;
            const auto index = lowerExpression(expression->index);
            const auto type = typeOf(expression);
            if (!index.has_value() || !type.has_value() || currentBlockTerminated()) return std::nullopt;
            baseAddress = (*deferred)();
            if (!baseAddress.has_value()) return std::nullopt;
            const auto place = emitRoleElementAddress(*baseType, *baseAddress, *index, *type, expression->span);
            if (!place.has_value()) return std::nullopt;
            return emitValue(ir::Opcode::load, *type, ir::ValueCategory::value, { place->id }, expression->span);
        }
        if (isAddressable(expression->base)) {
            baseAddress = lowerAddress(expression->base);
        } else {
            // A temporary receiver is spilled so the addressor can borrow it; the value keeps its own cleanup.
            const auto value = lowerExpression(expression->base);
            if (!value.has_value()) return std::nullopt;
            baseAddress = emitValue(ir::Opcode::stackAllocate, *baseType, ir::ValueCategory::address, {},
                                    expression->base->span);
            emitRawStore(*value, *baseAddress, expression->base->span);
        }
        const auto index = lowerExpression(expression->index);
        const auto type = typeOf(expression);
        if (!baseAddress.has_value() || !index.has_value() || !type.has_value()) return std::nullopt;
        const auto place = emitRoleElementAddress(*baseType, *baseAddress, *index, *type, expression->span);
        if (!place.has_value()) return std::nullopt;
        return emitValue(ir::Opcode::load, *type, ir::ValueCategory::value, { place->id }, expression->span);
    }
    std::optional<ir::Value> base;
    std::optional<ir::Value> index;
    if (isAddressable(expression->base) && !cannotDisturb(expression->index)) {
        // A String byte read whose index may reassign the string: the base's own indexes, then this index, and
        // only then is the string loaded (spec 4.4.3's order), so the read never sees a freed buffer.
        const auto deferred = prepareAddress(expression->base, true);
        const auto baseType = typeOf(expression->base);
        if (!deferred.has_value() || !baseType.has_value() || currentBlockTerminated()) return std::nullopt;
        index = lowerExpression(expression->index);
        if (!index.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto address = (*deferred)();
        if (!address.has_value()) return std::nullopt;
        base = emitValue(ir::Opcode::load, *baseType, ir::ValueCategory::value, { address->id },
                         expression->base->span);
    } else {
        base = lowerExpression(expression->base);
        index = lowerExpression(expression->index);
    }
    const auto type = typeOf(expression);
    if (!base.has_value() || !index.has_value() || !type.has_value()) {
        return std::nullopt;
    }
    return emitStringRole(semantic::LibraryRole::stringByte, { *base, *index }, *type, expression->span);
}

std::optional<ir::Value> Builder::lowerArray(const syntax::ArrayExprSyntax::Ptr& expression) {
    const auto type = typeOf(expression);
    if (!type.has_value()) return std::nullopt;
    const auto* arrayType = model->types().type(*type);
    if (arrayType == nullptr || !model->isArrayRoleType(*type) ||
        arrayType->arguments.size() != 1) {
        report(DiagnosticId::missingType, expression->span, "array literal has no array type");
        return std::nullopt;
    }

    const auto elementType = arrayType->arguments[0];
    const auto arguments = arrayType->arguments;
    const auto literal = model->arrayRoleMethod(*type, semantic::LibraryRole::arrayLiteral);
    const auto instance = literal.has_value() ? model->instanceIndex(*literal, arguments) : std::nullopt;
    if (!instance.has_value() || *instance >= instanceFunctions.size() ||
        instanceFunctions[*instance] == ir::invalidFunctionId) {
        report(DiagnosticId::missingSymbol, expression->span, "the array literal method has no lowered instance");
        return std::nullopt;
    }
    std::vector<ir::Value> elements;
    for (const auto& element : expression->elements) {
        const auto value = prepareOperand(element, elementType);
        if (!value.has_value()) return std::nullopt;
        elements.push_back(*value);
    }
    if (currentBlockTerminated()) return std::nullopt;
    const auto integer = [&](int64_t number, SourceSpan span) {
        auto constant = makeInstruction(ir::Opcode::literal, span);
        constant.integerValue = number;
        constant.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto value = *constant.result;
        emit(std::move(constant));
        recordValue(value, ValueOwnership::trivial);
        return value;
    };
    const auto count = integer(static_cast<int64_t>(elements.size()), expression->span);
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

    auto storage = makeInstruction(ir::Opcode::stackBlock, expression->span);
    storage.integerValue = static_cast<int64_t>(elementType);
    storage.operands = { count.id };
    storage.result = makeValue(model->types().intType(), ir::ValueCategory::value);
    const auto block = *storage.result;
    emit(std::move(storage));
    recordValue(block, ValueOwnership::trivial);
    // No element can fail after ownership starts moving into the untracked stack block.
    for (size_t index = 0; index < elements.size(); ++index) {
        const auto value = acquireOwned(elements[index], expression->elements[index]->span);
        if (!value.has_value()) return std::nullopt;
        if (elementType == model->types().voidType()) continue;
        const auto offset = integer(static_cast<int64_t>(index), expression->elements[index]->span);
        auto slot = makeInstruction(ir::Opcode::slotAddress, expression->elements[index]->span);
        slot.integerValue = static_cast<int64_t>(elementType);
        slot.operands = { block.id, offset.id };
        slot.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto address = *slot.result;
        emit(std::move(slot));
        recordValue(address, ValueOwnership::trivial);
        const auto place = emitValue(ir::Opcode::integerAddress, elementType, ir::ValueCategory::address,
                                     { address.id }, expression->elements[index]->span);
        emitRawStore(*value, place, expression->elements[index]->span);
    }
    auto instruction = makeInstruction(ir::Opcode::call, expression->span);
    instruction.callee = instanceFunctions[*instance];
    instruction.operands = { ownedReceiver->id, block.id, count.id };
    instruction.result = makeValue(*type, ir::ValueCategory::value);
    const auto result = *instruction.result;
    emit(std::move(instruction));
    recordValue(result, ValueOwnership::owned);
    return result;
}

std::optional<ir::Value> Builder::lowerDictionary(
        const syntax::DictionaryExprSyntax::Ptr& expression) {
    const auto type = typeOf(expression);
    if (!type.has_value()) return std::nullopt;
    const auto* dictionaryType = model->types().type(*type);
    const auto entryType = model->dictEntryType(*type);
    const auto fields = model->dictEntryFields();
    if (dictionaryType == nullptr || !model->types().isDictionary(*type) || !entryType.has_value() ||
        !fields.has_value()) {
        report(DiagnosticId::missingType, expression->span, "dictionary literal has no dictionary type");
        return std::nullopt;
    }
    const auto arguments = dictionaryType->arguments;
    const auto keyType = arguments[0];
    const auto valueType = arguments[1];
    const auto literal = model->collectionRoleMethod(*type, semantic::LibraryRole::dictLiteral);
    const auto instance = literal.has_value() ? model->instanceIndex(*literal, arguments) : std::nullopt;
    if (!instance.has_value() || *instance >= instanceFunctions.size() ||
        instanceFunctions[*instance] == ir::invalidFunctionId) {
        report(DiagnosticId::missingSymbol, expression->span,
               "the dictionary literal method has no lowered instance");
        return std::nullopt;
    }
    // Every key and value is evaluated first, left to right, entry by entry (decision 0027, rule 4).
    std::vector<std::pair<ir::Value, ir::Value>> entries;
    for (const auto& entry : expression->entries) {
        const auto key = prepareOperand(entry->key, keyType);
        if (!key.has_value()) return std::nullopt;
        const auto value = prepareOperand(entry->value, valueType);
        if (!value.has_value()) return std::nullopt;
        entries.emplace_back(*key, *value);
    }
    if (currentBlockTerminated()) return std::nullopt;
    const auto integer = [&](int64_t number, SourceSpan span) {
        auto constant = makeInstruction(ir::Opcode::literal, span);
        constant.integerValue = number;
        constant.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto value = *constant.result;
        emit(std::move(constant));
        recordValue(value, ValueOwnership::trivial);
        return value;
    };
    const auto count = integer(static_cast<int64_t>(entries.size()), expression->span);
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

    auto storage = makeInstruction(ir::Opcode::stackBlock, expression->span);
    storage.integerValue = static_cast<int64_t>(*entryType);
    storage.operands = { count.id };
    storage.result = makeValue(model->types().intType(), ir::ValueCategory::value);
    const auto block = *storage.result;
    emit(std::move(storage));
    recordValue(block, ValueOwnership::trivial);
    // Each entry's key, and its value unless `V` is Void, are stored through the entry's fields; nothing can fail
    // after ownership starts moving into the untracked stack block.
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = expression->entries[index];
        const auto key = acquireOwned(entries[index].first, entry->key->span);
        const auto value = acquireOwned(entries[index].second, entry->value->span);
        if (!key.has_value() || !value.has_value()) return std::nullopt;
        const auto offset = integer(static_cast<int64_t>(index), entry->span);
        auto slot = makeInstruction(ir::Opcode::slotAddress, entry->span);
        slot.integerValue = static_cast<int64_t>(*entryType);
        slot.operands = { block.id, offset.id };
        slot.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto address = *slot.result;
        emit(std::move(slot));
        recordValue(address, ValueOwnership::trivial);
        const auto place = emitValue(ir::Opcode::integerAddress, *entryType, ir::ValueCategory::address,
                                     { address.id }, entry->span);
        const auto keyPlace = emitValue(ir::Opcode::fieldAddress, keyType, ir::ValueCategory::address,
                                        { place.id }, entry->key->span, (*fields)[0]);
        emitRawStore(*key, keyPlace, entry->key->span);
        if (valueType == model->types().voidType()) continue;
        const auto valuePlace = emitValue(ir::Opcode::fieldAddress, valueType, ir::ValueCategory::address,
                                          { place.id }, entry->value->span, (*fields)[1]);
        emitRawStore(*value, valuePlace, entry->value->span);
    }
    auto instruction = makeInstruction(ir::Opcode::call, expression->span);
    instruction.callee = instanceFunctions[*instance];
    instruction.operands = { ownedReceiver->id, block.id, count.id };
    instruction.result = makeValue(*type, ir::ValueCategory::value);
    const auto result = *instruction.result;
    emit(std::move(instruction));
    recordValue(result, ValueOwnership::owned);
    return result;
}

} // namespace joyeer::lowering::detail
