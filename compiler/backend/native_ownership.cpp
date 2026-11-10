#include "native_ir.h"

namespace joyeer::llvmbackend::native {

llvm::Value* Builder::helper(ir::TypeId id, bool clone) {
    if (!destruction->requiresDestruction(id)) return nullPointer();
    return clone ? clones.at(id) : destroys.at(id);
}

void Builder::copy(ir::TypeId id, llvm::Value* destination, llvm::Value* sourceAddress) {
    if (destruction->requiresDestruction(id)) {
        builder.CreateCall(clones.at(id), {destination, sourceAddress});
    } else if (!unit(id)) {
        builder.CreateStore(builder.CreateLoad(llvmType(id), sourceAddress, temporary()), destination);
    }
}

void Builder::emitOwnership() {
    std::unordered_set<ir::TypeId> usedTypes;
    if (source.isRuntimeObject) {
        std::vector<ir::TypeId> pending;
        for (const auto& function : source.functions) {
            // A core function holds no managed value: the String literal that names
            // a message for the literal-address intrinsic is never materialized.
            if (function.isExternal || function.coreContext) continue;
            pending.push_back(function.resultType);
            for (const auto& parameter : function.parameters) pending.push_back(parameter.value.type);
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.result.has_value()) pending.push_back(instruction.result->type);
                }
            }
        }
        // Prelude signatures must not pull managed helpers into a leaf runtime
        // object. Include only used types and their stored component types.
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (!usedTypes.insert(id).second) continue;
            const auto& info = type(id);
            pending.insert(pending.end(), info.arguments.begin(), info.arguments.end());
            if (const auto found = structures.find(id); found != structures.end()) {
                for (const auto& field : found->second->fields) pending.push_back(field.type);
            }
            if (const auto found = enumerations.find(id); found != enumerations.end()) {
                for (const auto& enumCase : found->second->cases) {
                    pending.insert(pending.end(), enumCase.payloadTypes.begin(), enumCase.payloadTypes.end());
                }
            }
        }
    }
    for (const auto& type : source.types) {
        if (source.isRuntimeObject && !usedTypes.contains(type.id)) continue;
        if (!destruction->requiresDestruction(type.id)) continue;
        auto* cloneType = llvm::FunctionType::get(builder.getVoidTy(), {pointer(), pointer()}, false);
        auto* destroyType = llvm::FunctionType::get(builder.getVoidTy(), {pointer()}, false);
        const auto linkage = source.isRuntimeObject
                ? llvm::GlobalValue::InternalLinkage : llvm::GlobalValue::ExternalLinkage;
        clones.emplace(type.id, llvm::Function::Create(cloneType,
                linkage, "joyeer_clone_type_" + std::to_string(type.id), module()));
        destroys.emplace(type.id, llvm::Function::Create(destroyType,
                linkage, "joyeer_destroy_type_" + std::to_string(type.id), module()));
    }
}

// The helper bodies are emitted once every source function is declared, because a destroy
// helper calls the `deinit` of a struct that declares one.
void Builder::emitOwnershipBodies() {
    for (const auto& [id, function] : clones) {
        activeFunction = function;
        prologue = llvm::BasicBlock::Create(context(), "entry", function);
        builder.SetInsertPoint(prologue);
        emitClone(id);
        builder.CreateRetVoid();
    }
    for (const auto& [id, function] : destroys) {
        // A helper that calls a `deinit` carries no debug scope of its own, so inlining the
        // `deinit` into it (and it into a caller) corrupts the inlined debug scopes.
        if (const auto found = structures.find(id);
            found != structures.end() && found->second->deinitializer.has_value()) {
            function->addFnAttr(llvm::Attribute::NoInline);
        }
        activeFunction = function;
        prologue = llvm::BasicBlock::Create(context(), "entry", function);
        builder.SetInsertPoint(prologue);
        emitDestroy(id);
        builder.CreateRetVoid();
    }
    activeFunction = nullptr;
    prologue = nullptr;
}

void Builder::emitClone(ir::TypeId id) {
    auto* destination = activeFunction->getArg(0);
    auto* sourceAddress = activeFunction->getArg(1);
    destination->setName("destination");
    sourceAddress->setName("source");
    const auto kind = type(id).kind;
    if (kind == typing::TypeKind::task || kind == typing::TypeKind::channelSender ||
        kind == typing::TypeKind::channelReceiver) {
        // A task or a channel end has one owner and is never cloned; reaching this is a compiler bug.
        trap(builder.getTrue(), kind == typing::TypeKind::task ? "a task cannot be copied" : "a channel end cannot be copied");
        return;
    }
    if (kind == typing::TypeKind::string || kind == typing::TypeKind::array ||
            kind == typing::TypeKind::dictionary) {
        const auto name = kind == typing::TypeKind::string ? "joyeer_string_clone_abi"

                : kind == typing::TypeKind::array ? "joyeer_array_clone_abi" : "joyeer_dictionary_clone_abi";
        auto* value = builder.CreateLoad(llvmType(id), sourceAddress, "value");
        call(name, builder.getVoidTy(), {destination,
                builder.CreateExtractValue(value, 0, "data"), builder.CreateExtractValue(value, 1, "count")});
        return;
    }
    if (kind == typing::TypeKind::structure) {
        const auto& fields = structures.at(id)->fields;
        for (size_t index = 0; index < fields.size(); ++index) {
            copy(fields[index].type, field(llvmType(id), destination, static_cast<unsigned>(index)),
                    field(llvmType(id), sourceAddress, static_cast<unsigned>(index)));
        }
        return;
    }

    // Copy the tag and inert bytes first, then clone only the active payload.
    // Interpreting inactive payload storage would violate ownership semantics.
    auto* whole = builder.CreateLoad(llvmType(id), sourceAddress, "whole");
    builder.CreateStore(whole, destination);
    auto* exit = llvm::BasicBlock::Create(context(), "exit", activeFunction);
    const auto& cases = enumerations.at(id)->cases;
    auto* dispatch = builder.CreateSwitch(builder.CreateExtractValue(whole, 0, "tag"), exit,
            static_cast<unsigned>(cases.size()));
    for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
        auto* block = llvm::BasicBlock::Create(context(), "case." + std::to_string(caseIndex), activeFunction);
        dispatch->addCase(integer(caseIndex, 32), block);
        builder.SetInsertPoint(block);
        const auto offsets = payloadOffsets(cases[caseIndex]);
        for (size_t index = 0; index < offsets.size(); ++index) {
            const auto payloadType = cases[caseIndex].payloadTypes[index];
            if (!destruction->requiresDestruction(payloadType)) continue;
            copy(payloadType, payload(llvmType(id), destination, offsets[index]),
                    payload(llvmType(id), sourceAddress, offsets[index]));
        }
        builder.CreateBr(exit);
    }
    builder.SetInsertPoint(exit);
}

void Builder::emitDestroy(ir::TypeId id) {
    auto* address = activeFunction->getArg(0);
    address->setName("value");
    const auto kind = type(id).kind;
    if (kind == typing::TypeKind::channelSender || kind == typing::TypeKind::channelReceiver) {
        // Dropping an end closes it; a null end (never initialized, closed, or moved from) is nothing.
        auto* end = builder.CreateLoad(pointer(), address, "end");
        call("joyeer_channel_close_abi", builder.getVoidTy(), {end});
        builder.CreateStore(nullPointer(), address);
        return;
    }
    if (kind == typing::TypeKind::task) {
        // `join` zeroes the handle and a moved-from handle is zero, so a live one here was dropped
        // without being joined: the linear check should have refused the program (defence in depth).
        auto* block = builder.CreateLoad(pointer(), address, "task");
        trap(builder.CreateIsNotNull(block), "task dropped without join");
        return;
    }
    if (kind == typing::TypeKind::string || kind == typing::TypeKind::array ||
            kind == typing::TypeKind::dictionary) {
        const auto name = kind == typing::TypeKind::string ? "joyeer_string_destroy_abi"

                : kind == typing::TypeKind::array ? "joyeer_array_destroy_abi" : "joyeer_dictionary_destroy_abi";
        call(name, builder.getVoidTy(), {address});
        return;
    }
    if (kind == typing::TypeKind::structure) {
        const auto& definition = *structures.at(id);
        const auto& fields = definition.fields;
        if (definition.deinitializer.has_value()) {
            // The `deinit` runs first, and only for a live value: the hidden last field is
            // cleared by `take`, by zero-initialization and at the end of this helper.
            auto* live = builder.CreateLoad(llvmType(fields.back().type),
                    field(llvmType(id), address, static_cast<unsigned>(fields.size() - 1)), "live");
            auto* run = llvm::BasicBlock::Create(context(), "deinit", activeFunction);
            auto* members = llvm::BasicBlock::Create(context(), "members", activeFunction);
            builder.CreateCondBr(builder.CreateIsNotNull(live), run, members);
            builder.SetInsertPoint(run);
            // Clear the flag first: a `deinit` that overwrites `self` would otherwise destroy the
            // old value, find it still live, and run itself again without end.
            builder.CreateStore(llvm::Constant::getNullValue(llvmType(fields.back().type)),
                    field(llvmType(id), address, static_cast<unsigned>(fields.size() - 1)));
            builder.CreateCall(functions.at(*definition.deinitializer), {address});
            builder.CreateBr(members);
            builder.SetInsertPoint(members);
        }
        // Destruction is reverse declaration order, including nested aggregates.
        for (size_t index = fields.size(); index > 0; --index) {
            const auto payloadType = fields[index - 1].type;
            if (!destruction->requiresDestruction(payloadType)) continue;
            builder.CreateCall(destroys.at(payloadType),
                    {field(llvmType(id), address, static_cast<unsigned>(index - 1))});
        }
    } else {
        auto* whole = builder.CreateLoad(llvmType(id), address, "whole");
        auto* exit = llvm::BasicBlock::Create(context(), "exit", activeFunction);
        const auto& cases = enumerations.at(id)->cases;
        auto* dispatch = builder.CreateSwitch(builder.CreateExtractValue(whole, 0, "tag"), exit,
                static_cast<unsigned>(cases.size()));
        for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            auto* block = llvm::BasicBlock::Create(context(), "case." + std::to_string(caseIndex), activeFunction);
            dispatch->addCase(integer(caseIndex, 32), block);
            builder.SetInsertPoint(block);
            const auto offsets = payloadOffsets(cases[caseIndex]);
            for (size_t index = offsets.size(); index > 0; --index) {
                const auto payloadType = cases[caseIndex].payloadTypes[index - 1];
                if (!destruction->requiresDestruction(payloadType)) continue;
                builder.CreateCall(destroys.at(payloadType),
                        {payload(llvmType(id), address, offsets[index - 1])});
            }
            builder.CreateBr(exit);
        }
        builder.SetInsertPoint(exit);
    }
    builder.CreateStore(llvm::Constant::getNullValue(llvmType(id)), address);
}

} // namespace joyeer::llvmbackend::native