// The closed literal packing protocols (decisions 0026, 0027). A collection literal in user
// code is the one place a non-runtime function may form raw slots: it fills `stack_block`
// storage slot by slot and transfers it, once, to the collection's literal factory. These
// checks admit exactly that shape and record the admitted storage, slots and places in
// `literalBlocks`, `literalSlots`, `literalPlaces` and `literalEntryFields`, which the
// instruction checks then accept without runtime authorization.

#include "internal.h"

#include <algorithm>

namespace joyeer::ir::verifier {

// The blocks reached from `from` along successor (or predecessor) edges; `from` itself is included only
// when a cycle returns to it.
std::unordered_set<BlockId> FunctionVerifier::reachedFrom(BlockId from, bool forward) {
    const auto& edges = forward ? successors : predecessors;
    std::unordered_set<BlockId> reached;
    std::vector<BlockId> work { from };
    while (!work.empty()) {
        ++literalPathWork;
        const auto current = work.back();
        work.pop_back();
        const auto next = edges.find(current);
        if (next == edges.end()) continue;
        for (const auto block : next->second) {
            ++literalPathWork;
            if (reached.insert(block).second) work.push_back(block);
        }
    }
    return reached;
}

// A value stored into a literal slot is evaluated before the storage: earlier in its block, in a block that
// dominates it, or a parameter.
bool FunctionVerifier::evaluatedBefore(ValueId value, BlockId blockId, size_t storageIndex) const {
    const auto definition = valueDefinitions.find(value);
    return definition == valueDefinitions.end() ||
            (definition->second.first == blockId && definition->second.second < storageIndex) ||
            (definition->second.first != blockId && dominance.dominates(definition->second.first, blockId));
}

// The closed packing protocol of a dictionary literal (decision 0027, Decision 3, rules 1-6), checked rule by
// rule; returns the first rule that fails, or nothing, and admits the storage, slots, places and field
// addresses of a valid literal.
std::string FunctionVerifier::dictionaryLiteralFailure(const BasicBlock& block, size_t index,
                                                       const LiteralUse& transfer,
                                                       const std::vector<LiteralUse>& slots,
                                                       size_t transfers, bool foreignUse) {
    const auto& storage = block.instructions[index];
    const auto element = static_cast<TypeId>(storage.integerValue);
    const auto& call = *transfer.instruction;
    const auto* factory = functions.at(*call.callee);
    const auto result = types.find(factory->resultType);
    const auto entry = structures.find(element);
    const auto entryType = types.find(element);
    // Rule 2: the element is the entry struct over the dictionary's key and value.
    if (entry == structures.end() || entry->second->role != StructureRole::dictEntry ||
        entryType == types.end() || result == types.end() || entry->second->fields.size() != 2 ||
        entryType->second->arguments != result->second->arguments) {
        return "rule 2: the block's element must be the dictionary's entry struct over its key and value";
    }
    // Rule 6, first: the block is transferred exactly once and otherwise used only by its slots.
    if (transfers != 1 || foreignUse) {
        return "rule 6: the block must be transferred exactly once, to the literal method, and otherwise "
               "used only by its slots";
    }
    const bool sameBlockTransfer = transfer.block == block.id;
    if (!sameBlockTransfer || transfer.index <= index) {
        return "rule 6: the literal method must follow the packing in its block, on an inert zero receiver";
    }
    // Rule 1: the literal method receives the zero receiver, the block and its count.
    if (call.operands.size() != 3 || call.operands[1] != storage.result->id ||
        call.operands[2] != storage.operands[0]) {
        return "rule 1: the literal method must receive a receiver, the block and its count";
    }
    const auto* count = definingInstruction(storage.operands[0]);
    if (!isIntegerLiteral(count) || count->integerValue < 0) {
        return "rule 5: the entry count must be a non-negative constant";
    }
    const auto& keyField = entry->second->fields[0];
    const auto& valueField = entry->second->fields[1];
    const bool unitValue = types.contains(valueField.type) &&
            types.at(valueField.type)->kind == typing::TypeKind::voidType;
    // A stored value is used once in the packing window, so one owned value never fills two slots. Its other
    // uses are only on paths that never reach the packing and that the packing never reaches before the value
    // is evaluated again, such as its cleanup on an early exit of a later entry. Paths are followed from the
    // packing block, forward and backward, stopping at the value's defining block; a cycle that returns to
    // the packing without evaluating the value again would transfer it twice.
    //
    // A barrier only removes paths, so the walks without one bound every walk with one: a value whose other
    // uses lie off them, in a packing block on no cycle, needs no walk of its own. A block is reached from the
    // packing (or reaches it) without passing the defining block exactly when it is reached at all and the
    // defining block does not dominate it in the graph rooted at the packing (or in the reverse graph). So
    // each literal walks and builds those trees at most once per direction, never once per stored value.
    std::optional<std::unordered_set<BlockId>> reachedFromPacking;
    std::optional<std::unordered_set<BlockId>> reachingPacking;
    std::optional<DominanceAnalysis> fromPacking;
    std::optional<DominanceAnalysis> toPacking;
    const auto treeOf = [&](std::optional<DominanceAnalysis>& tree, bool forward) -> const DominanceAnalysis& {
        if (!tree.has_value()) {
            tree.emplace(block.id, forward ? successors : predecessors);
            literalPathWork += tree->work();
        }
        return *tree;
    };
    const auto usedOnceInWindow = [&](ValueId value) {
        const auto definition = valueDefinitions.find(value);
        const auto defined = definition == valueDefinitions.end()
                ? std::optional<BlockId>() : std::optional<BlockId>(definition->second.first);
        size_t inWindow = 0;
        std::vector<BlockId> elsewhere;
        for (const auto& use : literalUses[value]) {
            if (use.block == block.id && use.index > index && use.index < transfer.index) {
                ++inWindow;
            } else if (use.block == block.id || use.block == defined) {
                return false;
            } else {
                elsewhere.push_back(use.block);
            }
        }
        if (inWindow != 1) return false;
        if (defined == block.id && elsewhere.empty()) return true;
        if (!reachedFromPacking.has_value()) reachedFromPacking = reachedFrom(block.id, true);
        // Whether `other`, not the packing block, is reached from the packing (forward) or reaches it along a
        // path that avoids the defining block. A use in the packing block was rejected above; with no
        // definition, or one in the packing block itself, nothing else is a barrier. For the packing block
        // itself the answer is plain reachability, which is right only for the cycle check's self loop.
        const bool barrier = defined.has_value() && *defined != block.id;
        const auto avoids = [&](BlockId other, bool forward) {
            if (!forward && !reachingPacking.has_value()) {
                reachingPacking = reachedFrom(block.id, false);
            }
            const auto& reached = forward ? *reachedFromPacking : *reachingPacking;
            if (!reached.contains(other)) return false;
            return !barrier || !treeOf(forward ? fromPacking : toPacking, forward).dominates(*defined, other);
        };
        // Only a packing block on a cycle is reached again from itself, through one of its predecessors.
        if (defined != block.id && reachedFromPacking->contains(block.id)) {
            const auto into = predecessors.find(block.id);
            if (into != predecessors.end() &&
                std::any_of(into->second.begin(), into->second.end(), [&](BlockId predecessor) {
                    return avoids(predecessor, true);
                })) {
                return false;
            }
        }
        return std::none_of(elsewhere.begin(), elsewhere.end(), [&](BlockId use) {
            return avoids(use, true) || avoids(use, false);
        });
    };
    std::unordered_set<int64_t> keys;
    std::unordered_set<int64_t> storedValues;
    std::vector<ValueId> admittedSlots;
    std::vector<ValueId> admittedPlaces;
    std::vector<ValueId> admittedFields;
    for (const auto& use : slots) {
        // Rule 3: a bounded constant slot of the block, with exactly one entry place.
        const auto& slot = *use.instruction;
        const auto* offset = slot.operands.size() == 2 ? definingInstruction(slot.operands[1]) : nullptr;
        const bool inRange = isIntegerLiteral(offset) &&
                offset->integerValue >= 0 && offset->integerValue < count->integerValue;
        if (!slot.result.has_value() || slot.integerValue != storage.integerValue || use.block != block.id ||
            use.index <= index || !inRange || literalUses[slot.result->id].size() != 1) {
            return "rule 3: each slot must be a bounded constant slot of the block with one entry place";
        }
        const auto address = literalUses[slot.result->id].front();
        const auto& place = *address.instruction;
        if (address.operand != 0 || place.opcode != Opcode::integerAddress || place.operands.size() != 1 ||
            !place.result.has_value() || place.result->type != element ||
            place.result->category != ValueCategory::address || address.block != block.id ||
            address.index <= use.index) {
            return "rule 3: each slot must be a bounded constant slot of the block with one entry place";
        }
        std::unordered_set<semantic::SymbolId> projected;
        for (const auto& projection : literalUses[place.result->id]) {
            const auto& field = *projection.instruction;
            const bool isKey = field.symbol == keyField.symbol;
            const bool isValue = field.symbol == valueField.symbol;
            // Rule 3: the place is only projected, once to each of the key and the value, in the window.
            if (projection.operand != 0 || field.opcode != Opcode::fieldAddress || !field.result.has_value() ||
                !field.symbol.has_value() || (!isKey && !isValue) || !projected.insert(*field.symbol).second ||
                projection.block != block.id || projection.index <= address.index ||
                (sameBlockTransfer && projection.index >= transfer.index)) {
                return "rule 3: an entry place may only be projected once to each of its key and value "
                       "fields, in the packing window";
            }
            // Rule 5: a Void value has no storage, so it is neither projected nor stored.
            if (isValue && unitValue) return "rule 5: a Void value is neither projected nor stored";
            // Rule 4: one store, of a value of exactly the field's type evaluated before the storage.
            const auto fieldType = isKey ? keyField.type : valueField.type;
            const auto& stores = literalUses[field.result->id];
            bool stored = stores.size() == 1 && field.result->type == fieldType &&
                    field.result->category == ValueCategory::address;
            for (const auto& store : stores) {
                const auto& instruction = *store.instruction;
                const auto value = instruction.operands.size() == 2 ? values.find(instruction.operands[0])
                                                                    : values.end();
                stored = stored && store.operand == 1 && instruction.opcode == Opcode::store &&
                        value != values.end() && value->second.type == fieldType &&
                        value->second.category == ValueCategory::value && store.block == block.id &&
                        store.index > projection.index && sameBlockTransfer && store.index < transfer.index &&
                        evaluatedBefore(value->first, block.id, index) &&
                        usedOnceInWindow(value->first);
            }
            if (!stored) {
                return "rule 4: each field address must have exactly one use, a store in its block before "
                       "the transfer of a value of the field's type evaluated before the storage, used once in "
                       "the packing and otherwise only on paths that never reach it";
            }
            (isKey ? keys : storedValues).insert(offset->integerValue);
            admittedFields.push_back(field.result->id);
        }
        admittedSlots.push_back(slot.result->id);
        admittedPlaces.push_back(place.result->id);
    }
    // Rule 5: every entry has its key and, unless the value is Void, its value.
    const auto entries = static_cast<size_t>(count->integerValue);
    if (slots.size() != entries || keys.size() != entries || storedValues.size() != (unitValue ? 0 : entries)) {
        return "rule 5: the key must be stored once for every entry, and the value too unless it is Void";
    }
    // Rule 6: the transfer follows the packing in the block, on an inert zero receiver, and the window holds
    // only packing operations: no load, no escaping address, nothing fallible.
    const auto* receiver = definingInstruction(call.operands[0]);
    const auto* zeroPlace = receiver != nullptr && receiver->opcode == Opcode::load &&
            receiver->operands.size() == 1 ? definingInstruction(receiver->operands[0]) : nullptr;
    bool inert = sameBlockTransfer && transfer.index > index && zeroPlace != nullptr &&
            zeroPlace->opcode == Opcode::stackAllocate && zeroPlace->result.has_value() &&
            zeroPlace->result->type == factory->resultType;
    bool zeroed = false;
    if (inert) {
        for (const auto& use : literalUses[zeroPlace->result->id]) {
            if (use.instruction->opcode == Opcode::zeroInitialize && use.operand == 0 &&
                use.block == block.id && use.index < index && !zeroed) {
                zeroed = true;
            } else if (use.instruction != receiver || use.operand != 0 || use.block != block.id ||
                       use.index >= index || !zeroed) {
                inert = false;
            }
        }
    }
    if (!inert || !zeroed) {
        return "rule 6: the literal method must follow the packing in its block, on an inert zero receiver";
    }
    for (size_t packed = index + 1; packed < transfer.index; ++packed) {
        switch (block.instructions[packed].opcode) {
            case Opcode::literal:
                if (!isIntegerLiteral(&block.instructions[packed])) {
                    return "rule 6: the packing window holds only constants, slots, entry places, field "
                           "addresses and stores";
                }
                break;
            case Opcode::slotAddress:
            case Opcode::integerAddress:
            case Opcode::fieldAddress:
            case Opcode::store:
                break;
            default:
                return "rule 6: the packing window holds only constants, slots, entry places, field "
                       "addresses and stores";
        }
    }
    literalBlocks.emplace(storage.result->id, element);
    for (const auto slot : admittedSlots) literalSlots.emplace(slot, element);
    for (const auto place : admittedPlaces) literalPlaces.emplace(place, element);
    literalEntryFields.insert(admittedFields.begin(), admittedFields.end());
    return {};
}

// Every `stack_block` that user code forms, or that runtime code transfers to a literal factory,
// follows the array protocol, or the dictionary protocol when its factory builds a dictionary.
// User functions may only form raw slots through this closed, straight-line initialize-and-transfer
// protocol. Ordinary source uses of the layout intrinsics still need runtime authorization.
void FunctionVerifier::verifyLiteralStorage() {
    for (const auto& block : function.blocks) {
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& storage = block.instructions[index];
            if (storage.opcode != Opcode::stackBlock || !storage.result.has_value() ||
                storage.operands.size() != 1) continue;
            if (function.runtimeContext && std::none_of(literalUses[storage.result->id].begin(),
                    literalUses[storage.result->id].end(), [&](const auto& use) {
                        return use.operand == 1 && use.instruction->opcode == Opcode::call &&
                                use.instruction->callee.has_value() &&
                                literalFactories.contains(*use.instruction->callee);
                    })) {
                continue;
            }
            const auto* count = definingInstruction(storage.operands[0]);
            const auto element = static_cast<TypeId>(storage.integerValue);
            bool valid = isIntegerLiteral(count) && count->integerValue >= 0 && types.contains(element);
            const LiteralUse* transfer = nullptr;
            std::vector<LiteralUse> slots;
            size_t transfers = 0;
            bool foreignUse = false;
            for (const auto& use : literalUses[storage.result->id]) {
                if (use.instruction->opcode == Opcode::call && use.operand == 1 &&
                    use.instruction->callee.has_value() && literalFactories.contains(*use.instruction->callee)) {
                    if (transfer != nullptr) valid = false;
                    transfer = &use;
                    ++transfers;
                } else if (use.instruction->opcode == Opcode::slotAddress && use.operand == 0) {
                    slots.push_back(use);
                } else {
                    valid = false;
                    foreignUse = true;
                }
            }
            const auto* factory = transfer != nullptr ? functions.at(*transfer->instruction->callee) : nullptr;
            const auto handle = factory != nullptr ? structures.find(factory->resultType) : structures.end();
            if (handle != structures.end() && handle->second->role == StructureRole::dict) {
                const auto failure = dictionaryLiteralFailure(block, index, *transfer, slots, transfers, foreignUse);
                if (!failure.empty()) {
                    report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                           "dictionary literal " + failure);
                }
                continue;
            }
            valid = valid && transfer != nullptr;
            if (transfer != nullptr) {
                const auto& call = *transfer->instruction;
                const auto result = types.find(factory->resultType);
                valid = valid && transfer->block == block.id && transfer->index > index &&
                        call.operands.size() == 3 && call.operands[2] == storage.operands[0] &&
                        result != types.end() && result->second->arguments.size() == 1 &&
                        result->second->arguments[0] == element;
                const auto* receiver = call.operands.empty() ? nullptr : definingInstruction(call.operands[0]);
                const auto* place = receiver != nullptr && receiver->opcode == Opcode::load &&
                        receiver->operands.size() == 1 ? definingInstruction(receiver->operands[0]) : nullptr;
                valid = valid && place != nullptr && place->opcode == Opcode::stackAllocate &&
                        place->result.has_value() && place->result->type == factory->resultType;
                bool zeroed = false;
                if (place != nullptr && place->result.has_value()) {
                    for (const auto& use : literalUses[place->result->id]) {
                        if (use.instruction->opcode == Opcode::zeroInitialize && use.operand == 0 &&
                            use.block == block.id && use.index < index) {
                            if (zeroed) valid = false;
                            zeroed = true;
                        } else if (use.instruction != receiver || use.operand != 0 ||
                                   use.block != block.id || use.index >= index || !zeroed) {
                            valid = false;
                        }
                    }
                }
                valid = valid && zeroed;
                for (size_t packed = index + 1; packed < transfer->index; ++packed) {
                    switch (block.instructions[packed].opcode) {
                        case Opcode::literal:
                            valid = valid && isIntegerLiteral(&block.instructions[packed]);
                            break;
                        case Opcode::slotAddress:
                        case Opcode::integerAddress:
                        case Opcode::store:
                            break;
                        default:
                            valid = false;
                            break;
                    }
                }
            }
            std::unordered_set<int64_t> initialized;
            std::vector<ValueId> admittedSlots;
            std::vector<ValueId> admittedPlaces;
            for (const auto& use : slots) {
                const auto& slot = *use.instruction;
                const auto* offset = slot.operands.size() == 2 ? definingInstruction(slot.operands[1]) : nullptr;
                const bool inRange = isIntegerLiteral(offset) &&
                        count != nullptr && offset->integerValue >= 0 && offset->integerValue < count->integerValue;
                valid = valid && slot.result.has_value() && slot.integerValue == storage.integerValue &&
                        use.block == block.id && use.index > index && inRange;
                if (!slot.result.has_value()) continue;
                const auto& addresses = literalUses[slot.result->id];
                valid = valid && addresses.size() == 1;
                for (const auto& address : addresses) {
                    const auto& place = *address.instruction;
                    valid = valid && address.operand == 0 && place.opcode == Opcode::integerAddress &&
                            place.operands.size() == 1 && place.result.has_value() &&
                            place.result->type == element && place.result->category == ValueCategory::address &&
                            address.block == block.id && address.index > use.index;
                    if (!place.result.has_value()) continue;
                    const auto& stores = literalUses[place.result->id];
                    valid = valid && stores.size() == 1;
                    for (const auto& store : stores) {
                        const auto& instruction = *store.instruction;
                        const auto value = instruction.operands.size() == 2
                                ? values.find(instruction.operands[0]) : values.end();
                        const auto definition = value == values.end() ? valueDefinitions.end()
                                : valueDefinitions.find(value->first);
                        const bool evaluatedFirst = value != values.end() &&
                                (definition == valueDefinitions.end() ||
                                 (definition->second.first == block.id && definition->second.second < index) ||
                                 (definition->second.first != block.id &&
                                  dominance.dominates(definition->second.first, block.id)));
                        valid = valid && store.operand == 1 && instruction.opcode == Opcode::store &&
                                value != values.end() && value->second.type == element &&
                                value->second.category == ValueCategory::value &&
                                store.block == block.id && store.index > address.index &&
                                transfer != nullptr && store.index < transfer->index && evaluatedFirst &&
                                inRange && initialized.insert(offset->integerValue).second;
                    }
                    admittedPlaces.push_back(place.result->id);
                }
                admittedSlots.push_back(slot.result->id);
            }
            valid = valid && count != nullptr && types.contains(element) &&
                    (types.at(element)->kind == typing::TypeKind::voidType
                            ? slots.empty()
                            : initialized.size() == static_cast<uint64_t>(count->integerValue));
            if (!valid) {
                report(VerificationErrorId::invalidInstruction, functionId, block.id, index,
                       "array literal storage must initialize each bounded slot exactly once, then transfer "
                       "only to its matching literal factory with an inert-zero receiver");
                continue;
            }
            literalBlocks.emplace(storage.result->id, element);
            for (const auto slot : admittedSlots) literalSlots.emplace(slot, element);
            for (const auto place : admittedPlaces) literalPlaces.emplace(place, element);
        }
    }
}

} // namespace joyeer::ir::verifier
