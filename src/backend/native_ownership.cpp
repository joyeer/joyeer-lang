#include "native_ir.h"

namespace joyeer::llvmbackend::native {

llvm::Value* Builder::helper(ir::TypeId id, bool clone) {
    if (!ir::requiresDestruction(source, id)) return nullPointer();
    return clone ? clones.at(id) : destroys.at(id);
}

void Builder::copy(ir::TypeId id, llvm::Value* destination, llvm::Value* sourceAddress) {
    if (ir::requiresDestruction(source, id)) {
        builder.CreateCall(clones.at(id), {destination, sourceAddress});
    } else if (!unit(id)) {
        builder.CreateStore(builder.CreateLoad(llvmType(id), sourceAddress, temporary()), destination);
    }
}

void Builder::emitOwnership() {
    for (const auto& type : source.types) {
        if (!ir::requiresDestruction(source, type.id)) continue;
        auto* cloneType = llvm::FunctionType::get(builder.getVoidTy(), {pointer(), pointer()}, false);
        auto* destroyType = llvm::FunctionType::get(builder.getVoidTy(), {pointer()}, false);
        clones.emplace(type.id, llvm::Function::Create(cloneType,
                llvm::GlobalValue::ExternalLinkage, "joyeer_clone_type_" + std::to_string(type.id), module()));
        destroys.emplace(type.id, llvm::Function::Create(destroyType,
                llvm::GlobalValue::ExternalLinkage, "joyeer_destroy_type_" + std::to_string(type.id), module()));
    }
    for (const auto& [id, function] : clones) {
        activeFunction = function;
        prologue = llvm::BasicBlock::Create(context(), "entry", function);
        builder.SetInsertPoint(prologue);
        emitClone(id);
        builder.CreateRetVoid();
    }
    for (const auto& [id, function] : destroys) {
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
            if (!ir::requiresDestruction(source, payloadType)) continue;
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
    if (kind == typing::TypeKind::string || kind == typing::TypeKind::array ||
            kind == typing::TypeKind::dictionary) {
        const auto name = kind == typing::TypeKind::string ? "joyeer_string_destroy_abi"
                : kind == typing::TypeKind::array ? "joyeer_array_destroy_abi" : "joyeer_dictionary_destroy_abi";
        call(name, builder.getVoidTy(), {address});
        return;
    }
    if (kind == typing::TypeKind::structure) {
        const auto& fields = structures.at(id)->fields;
        // Destruction is reverse declaration order, including nested aggregates.
        for (size_t index = fields.size(); index > 0; --index) {
            const auto payloadType = fields[index - 1].type;
            if (!ir::requiresDestruction(source, payloadType)) continue;
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
                if (!ir::requiresDestruction(source, payloadType)) continue;
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