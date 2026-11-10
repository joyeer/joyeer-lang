#include "native_ir.h"

#include <algorithm>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

namespace {
size_t alignTo(size_t size, size_t alignment) {
    return (size + alignment - 1) / alignment * alignment;
}
}

// The thread record is JOYEER_THREAD_BYTES (runtime.h); a test keeps the two equal.
constexpr size_t taskRecordBytes = 128;

TaskLayout Builder::taskLayout(ir::TypeId input, ir::TypeId result) {
    const auto resultLayout = unit(result) ? Layout {} : layout(result);
    const auto inputLayout = unit(input) ? Layout {} : layout(input);
    TaskLayout out;
    out.resultOffset = alignTo(taskRecordBytes + 8, resultLayout.alignment);
    out.inputOffset = alignTo(out.resultOffset + resultLayout.size, inputLayout.alignment);
    out.size = alignTo(out.inputOffset + inputLayout.size, 16);
    return out;
}

// The thread entry for one task target: it loads the moved input from the block, calls the target as
// a direct call would, and stores the result. The runtime calls it with the block as its argument.
llvm::Function* Builder::taskThunk(const ir::Function& callee) {
    if (const auto found = taskThunks.find(callee.id); found != taskThunks.end()) return found->second;
    auto* thunkType = llvm::FunctionType::get(builder.getVoidTy(), {pointer()}, false);
    auto* thunk = llvm::Function::Create(thunkType, llvm::GlobalValue::InternalLinkage,
            "joyeer_task_thunk_" + std::to_string(callee.id), module());
    // No debug scope of its own: inlining it into a caller with line tables would corrupt them.
    thunk->addFnAttr(llvm::Attribute::NoInline);
    taskThunks.emplace(callee.id, thunk);

    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    builder.SetCurrentDebugLocation(llvm::DebugLoc());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context(), "entry", thunk));
    const auto input = callee.parameters.at(0).value.type;
    const auto info = taskLayout(input, callee.resultType);
    auto* block = thunk->getArg(0);
    block->setName("block");
    auto* argument = builder.CreateLoad(llvmType(input), bytes(block, integer(info.inputOffset)), "input");
    auto* invocation = builder.CreateCall(functions.at(callee.id), {argument});
    if (callee.returnsValue && !unit(callee.resultType)) {
        builder.CreateStore(invocation, bytes(block, integer(info.resultOffset)));
    }
    builder.CreateRetVoid();
    return thunk;
}

int Builder::keyKind(ir::TypeId id) {
    switch (type(id).kind) {
        case typing::TypeKind::integer: return 1;
        case typing::TypeKind::boolean: return 2;
        case typing::TypeKind::string: return 3;
        case typing::TypeKind::uint8: return 4;
        default:
            report(DiagnosticId::unsupportedType, {}, "dictionary keys are not yet supported for type '" + type(id).name + "'");
            throw std::runtime_error("unsupported dictionary key");
    }
}

llvm::Value* Builder::dictionaryFind(ir::TypeId id, llvm::Value* dictionary, llvm::Value* key) {
    const auto keyType = type(id).arguments.at(0);
    return call("joyeer_dictionary_find_abi", pointer(),
            {builder.CreateExtractValue(dictionary, 0, temporary()),
             builder.CreateExtractValue(dictionary, 1, temporary()), spill(key),
             integer(layout(keyType).size), integer(static_cast<uint64_t>(keyKind(keyType)), 32)});
}

// Reads one value of the given Joyeer type from the compiler's constant
// encoding and returns it as an LLVM constant. Arrays become read-only globals
// laid out like runtime arrays: an `ArrayHeader` immediately before the
// elements, so cloning an array constant works through the normal runtime path.
llvm::Constant* Builder::decodeConstant(ir::TypeId id, const std::string& data, size_t& offset) {
    const auto read = [&](unsigned bytes) {
        if (offset + bytes > data.size()) throw std::runtime_error("truncated constant data");
        uint64_t value = 0;
        for (unsigned index = 0; index < bytes; ++index) {
            value |= static_cast<uint64_t>(static_cast<unsigned char>(data[offset + index])) << (8 * index);
        }
        offset += bytes;
        return value;
    };
    const auto kind = type(id).kind;
    if (kind == typing::TypeKind::boolean) return llvm::ConstantInt::get(llvmType(id), read(1));
    if (kind == typing::TypeKind::float32) {
        return llvm::ConstantFP::get(context(),
                llvm::APFloat(llvm::APFloat::IEEEsingle(), llvm::APInt(32, read(4))));
    }
    if (kind == typing::TypeKind::float64) {
        return llvm::ConstantFP::get(context(),
                llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, read(8))));
    }
    if (typing::isInteger(kind)) {
        return llvm::ConstantInt::get(llvmType(id), read(typing::scalarBitWidth(kind) / 8));
    }
    if (kind == typing::TypeKind::string) {
        const auto length = read(8);
        if (offset + length > data.size()) throw std::runtime_error("truncated constant data");
        const std::string_view text(data.data() + offset, length);
        offset += length;
        return llvm::cast<llvm::Constant>(stringLiteral(text));
    }
    if (kind != typing::TypeKind::array) throw std::runtime_error("unsupported constant type");

    const auto elementId = type(id).arguments.at(0);
    const auto count = read(8);
    std::vector<llvm::Constant*> elements;
    for (uint64_t index = 0; index < count; ++index) {
        elements.push_back(decodeConstant(elementId, data, offset));
    }
    auto* i64 = builder.getInt64Ty();
    auto* headerType = llvm::StructType::get(context(), {i64, pointer(), pointer()});
    auto* header = llvm::ConstantStruct::get(headerType, {
            llvm::ConstantInt::get(i64, layout(elementId).size),
            llvm::cast<llvm::Constant>(helper(elementId, true)),
            llvm::cast<llvm::Constant>(helper(elementId, false))});
    auto* elementsType = llvm::ArrayType::get(llvmType(elementId), count);
    auto* storageType = llvm::StructType::get(context(), {headerType, elementsType});
    auto* storage = llvm::ConstantStruct::get(storageType,
            {header, llvm::ConstantArray::get(elementsType, elements)});
    auto* global = new llvm::GlobalVariable(module(), storageType, true,
            llvm::GlobalValue::PrivateLinkage, storage, ".joyeer.constant." + std::to_string(nextString++));
    auto* first = llvm::ConstantExpr::getGetElementPtr(storageType, global,
            llvm::ArrayRef<llvm::Constant*> {integer(0, 32), integer(1, 32), integer(0, 32)},
            llvm::GEPNoWrapFlags::inBounds());
    return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvmType(id)),
            {first, integer(count), integer(count)});
}

llvm::Constant* Builder::constantArray(const ir::Constant& constant) {
    if (const auto found = constantArrays.find(constant.id); found != constantArrays.end()) {
        return found->second;
    }
    size_t offset = 0;
    auto* value = decodeConstant(constant.type, constant.data, offset);
    constantArrays.emplace(constant.id, value);
    return value;
}

bool Builder::emitCollection(const ir::Instruction& instruction, llvm::Value*& result) {
    const auto argument = [&](size_t index) { return operand(instruction.operands.at(index)); };
    const auto resultType = [&]() { return llvmType(instruction.result->type); };
    using Opcode = ir::Opcode;
    switch (instruction.opcode) {
        case Opcode::constructArray: {
            const auto elementId = type(instruction.result->type).arguments.at(0);
            auto* elementType = llvmType(elementId);
            llvm::Value* data = nullPointer();
            const auto count = instruction.operands.size();
            if (count != 0 && !unit(elementId)) {
                auto* arrayType = llvm::ArrayType::get(elementType, count);
                data = stack(arrayType);
                for (size_t index = 0; index < count; ++index) {
                    auto* address = builder.CreateInBoundsGEP(arrayType, data,
                            {integer(0, 32), integer(index)}, temporary());
                    builder.CreateStore(argument(index), address);
                }
            }
            auto* address = stack(resultType());
            // Runtime storage owns cloned elements, not the temporary array on
            // our stack. Type-specific callbacks preserve nested value semantics.
            call("joyeer_array_create_owned_abi", builder.getVoidTy(),
                    {address, data, integer(count), integer(layout(elementId).size),
                     helper(elementId, true), helper(elementId, false)});
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::constantValue:
            result = constantArray(source.constants.at(static_cast<size_t>(instruction.integerValue)));
            return true;
        case Opcode::stringUtf8: {
            auto* string = argument(0);
            auto* address = stack(resultType());
            call("joyeer_array_create_owned_abi", builder.getVoidTy(),
                    {address, builder.CreateExtractValue(string, 0, temporary()),
                     builder.CreateExtractValue(string, 1, temporary()), integer(1), nullPointer(), nullPointer()});
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::arrayAppend: {
            const auto elementId = values.at(instruction.operands[1]).type;
            call("joyeer_array_append_owned_abi", builder.getVoidTy(),
                    {argument(0), unit(elementId) ? nullPointer() : spill(argument(1))});
            return true;
        }
        case Opcode::taskSpawn: {
            const auto& callee = *sourceFunctions.at(*instruction.callee);
            const auto input = callee.parameters.at(0).value.type;
            const auto info = taskLayout(input, callee.resultType);
            auto* block = call("joyeer_alloc", pointer(), {integer(info.size)});
            // The runtime requires the thread record zeroed, and a recycled block is not.
            builder.CreateMemSet(block, builder.getInt8(0), taskRecordBytes, llvm::MaybeAlign(8));
            builder.CreateStore(integer(info.size), bytes(block, integer(taskRecordBytes)));
            builder.CreateStore(argument(0), bytes(block, integer(info.inputOffset)));
            auto* started = call("joyeer_thread_start_abi", builder.getInt64Ty(),
                    {block, taskThunk(callee), block});
            // -1073741824 is JOYEER_THREAD_LIMIT; any other negative code is the system's refusal.
            trap(builder.CreateICmpEQ(started, builder.getInt64(static_cast<uint64_t>(-1073741824LL)),
                         temporary()), "too many tasks");
            trap(builder.CreateICmpSLT(started, integer(0), temporary()), "cannot start task");
            result = block;
            return true;
        }
        case Opcode::taskJoin: {
            const auto resultId = instruction.result->type;
            auto* slot = argument(0);
            auto* block = builder.CreateLoad(pointer(), slot, temporary());
            auto* joined = call("joyeer_thread_join_abi", builder.getInt64Ty(), {block});
            trap(builder.CreateICmpSLT(joined, integer(0), temporary()), "cannot join task");
            const auto resultLayout = unit(resultId) ? Layout {} : layout(resultId);
            const auto resultOffset = alignTo(taskRecordBytes + 8, resultLayout.alignment);
            llvm::Value* value = unit(resultId)
                    ? static_cast<llvm::Value*>(llvm::Constant::getNullValue(llvmType(resultId)))
                    : builder.CreateLoad(llvmType(resultId), bytes(block, integer(resultOffset)), temporary());
            auto* size = builder.CreateLoad(builder.getInt64Ty(), bytes(block, integer(taskRecordBytes)), temporary());
            call("joyeer_free", builder.getVoidTy(), {block, size});
            builder.CreateStore(nullPointer(), slot);
            result = value;
            return true;
        }
        case Opcode::channelCreate: {
            const auto element = type(values.at(instruction.operands[1]).type).arguments.at(0);
            const auto elementLayout = unit(element) ? Layout {0, 1} : layout(element);
            call("joyeer_channel_create_abi", builder.getInt64Ty(),
                    {argument(0), integer(elementLayout.size), integer(elementLayout.alignment),
                     helper(element, false), argument(1), argument(2)});
            return true;
        }
        case Opcode::channelSend: {
            const auto element = type(values.at(instruction.operands[0]).type).arguments.at(0);
            auto* end = builder.CreateLoad(pointer(), argument(0), temporary());
            auto* valueAddress = unit(element) ? nullPointer() : spill(argument(1));
            auto* sent = call("joyeer_channel_send_abi", builder.getInt64Ty(), {end, valueAddress});
            const auto resultId = instruction.result->type;
            const auto& cases = enumerations.at(resultId)->cases;
            size_t okTag = cases.size();
            size_t errTag = cases.size();
            for (size_t index = 0; index < cases.size(); ++index) {
                if (cases[index].name == "Ok") okTag = index;
                if (cases[index].name == "Err") errTag = index;
            }
            if (okTag == cases.size() || errTag == cases.size()) return false;
            auto* address = stack(resultType());
            builder.CreateStore(llvm::Constant::getNullValue(resultType()), address);
            auto* delivered = builder.CreateICmpNE(sent, integer(0), temporary());
            builder.CreateStore(builder.CreateSelect(delivered, integer(okTag, 32), integer(errTag, 32), temporary()),
                    field(resultType(), address, 0));
            if (destruction->requiresDestruction(element)) {
                // The channel did not take the value (the receiver is closed), so it is destroyed here.
                auto* unsent = llvm::BasicBlock::Create(context(), "channel.unsent." + temporary(), activeFunction);
                auto* done = llvm::BasicBlock::Create(context(), "channel.sent." + temporary(), activeFunction);
                builder.CreateCondBr(delivered, done, unsent);
                builder.SetInsertPoint(unsent);
                builder.CreateCall(destroys.at(element), {valueAddress});
                builder.CreateBr(done);
                builder.SetInsertPoint(done);
            }
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::channelReceive: {
            const auto element = type(values.at(instruction.operands[0]).type).arguments.at(0);
            const auto resultId = instruction.result->type;
            const auto& cases = enumerations.at(resultId)->cases;
            size_t okTag = cases.size();
            size_t errTag = cases.size();
            for (size_t index = 0; index < cases.size(); ++index) {
                if (cases[index].name == "Ok") okTag = index;
                if (cases[index].name == "Err") errTag = index;
            }
            if (okTag == cases.size() || errTag == cases.size()) return false;
            auto* address = stack(resultType());
            builder.CreateStore(llvm::Constant::getNullValue(resultType()), address);
            auto* end = builder.CreateLoad(pointer(), argument(0), temporary());
            // The runtime moves the value's bytes straight into the `Ok` payload.
            auto* received = call("joyeer_channel_receive_abi", builder.getInt64Ty(),
                    {end, unit(element) ? nullPointer() : payload(resultType(), address)});
            auto* delivered = builder.CreateICmpNE(received, integer(0), temporary());
            builder.CreateStore(builder.CreateSelect(delivered, integer(okTag, 32), integer(errTag, 32), temporary()),
                    field(resultType(), address, 0));
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::channelClose: {
            auto* slot = argument(0);
            auto* end = builder.CreateLoad(pointer(), slot, temporary());
            call("joyeer_channel_close_abi", builder.getVoidTy(), {end});
            builder.CreateStore(nullPointer(), slot);
            return true;
        }
        case Opcode::arrayConsumeEmpty:
            // Traps when the array is not empty; otherwise frees its storage and zeroes the handle,
            // so the scope-end destroy of the moved-from binding does nothing.
            call("joyeer_array_consume_empty_abi", builder.getVoidTy(), {argument(0)});
            return true;
        case Opcode::arrayRemoveLast: {
            const auto id = instruction.result->type;
            const auto& cases = enumerations.at(id)->cases;
            size_t someTag = cases.size();
            size_t noneTag = cases.size();
            for (size_t index = 0; index < cases.size(); ++index) {
                if (cases[index].name == "Some") someTag = index;
                if (cases[index].name == "None") noneTag = index;
            }
            if (someTag == cases.size() || noneTag == cases.size()) return false;
            const auto arrayId = values.at(instruction.operands[0]).type;
            const auto elementId = type(arrayId).arguments.at(0);
            auto* address = stack(resultType());
            builder.CreateStore(llvm::Constant::getNullValue(resultType()), address);
            auto* tagAddress = field(resultType(), address, 0);
            builder.CreateStore(integer(noneTag, 32), tagAddress);
            // The runtime moves the element's bytes straight into the payload, so ownership
            // passes to the `Some` without a clone; the tag says whether there was one.
            auto* removed = call("joyeer_array_remove_last_abi", builder.getInt64Ty(),
                    {argument(0), unit(elementId) ? nullPointer() : payload(resultType(), address)});
            auto* some = llvm::BasicBlock::Create(context(), "array.remove.some." + temporary(), activeFunction);
            auto* done = llvm::BasicBlock::Create(context(), "array.remove.done." + temporary(), activeFunction);
            builder.CreateCondBr(builder.CreateICmpNE(removed, integer(0), temporary()), some, done);
            builder.SetInsertPoint(some);
            builder.CreateStore(integer(someTag, 32), tagAddress);
            builder.CreateBr(done);
            builder.SetInsertPoint(done);
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::constructDictionary: {
            const auto& arguments = type(instruction.result->type).arguments;
            const auto keyId = arguments.at(0);
            const auto valueId = arguments.at(1);
            auto* entryType = llvm::StructType::get(context(), {llvmType(keyId), llvmType(valueId)});
            const auto keyLayout = layout(keyId);
            const auto valueLayout = layout(valueId);
            const auto valueOffset = alignTo(keyLayout.size, valueLayout.alignment);
            const auto entrySize = alignTo(valueOffset + valueLayout.size,
                    std::max(keyLayout.alignment, valueLayout.alignment));
            const auto count = instruction.operands.size() / 2;
            llvm::Value* data = nullPointer();
            if (count != 0) {
                auto* arrayType = llvm::ArrayType::get(entryType, count);
                data = stack(arrayType);
                for (size_t index = 0; index < count; ++index) {
                    auto* address = builder.CreateInBoundsGEP(arrayType, data,
                            {integer(0, 32), integer(index)}, temporary());
                    builder.CreateStore(argument(index * 2), field(entryType, address, 0));
                    if (!unit(valueId)) builder.CreateStore(argument(index * 2 + 1), field(entryType, address, 1));
                }
            }
            auto* address = stack(resultType());
            call("joyeer_dictionary_create_owned_abi", builder.getVoidTy(),
                    {address, data, integer(count), integer(keyLayout.size), integer(valueLayout.size),
                     integer(entrySize), integer(valueOffset), integer(static_cast<uint64_t>(keyKind(keyId)), 32),
                     helper(keyId, true), helper(keyId, false), helper(valueId, true), helper(valueId, false)});
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::dictionarySet: {
            const auto valueId = values.at(instruction.operands[2]).type;
            call("joyeer_dictionary_set_owned_abi", builder.getVoidTy(),
                    {argument(0), spill(argument(1)), unit(valueId) ? nullPointer() : spill(argument(2))});
            return true;
        }
        case Opcode::dictionaryGet: {
            const auto id = instruction.result->type;
            const auto& cases = enumerations.at(id)->cases;
            size_t someTag = cases.size();
            size_t noneTag = cases.size();
            for (size_t index = 0; index < cases.size(); ++index) {
                if (cases[index].name == "Some") someTag = index;
                if (cases[index].name == "None") noneTag = index;
            }
            if (someTag == cases.size() || noneTag == cases.size()) return false;
            const auto dictionaryId = values.at(instruction.operands[0]).type;
            const auto storedId = type(dictionaryId).arguments.at(1);
            auto* found = dictionaryFind(dictionaryId, argument(0), argument(1));
            auto* address = stack(resultType());
            builder.CreateStore(llvm::Constant::getNullValue(resultType()), address);
            auto* tagAddress = field(resultType(), address, 0);
            builder.CreateStore(integer(noneTag, 32), tagAddress);
            auto* some = llvm::BasicBlock::Create(context(), "dictionary.get.some." + temporary(), activeFunction);
            auto* done = llvm::BasicBlock::Create(context(), "dictionary.get.done." + temporary(), activeFunction);
            builder.CreateCondBr(builder.CreateICmpNE(found, nullPointer(), temporary()), some, done);
            builder.SetInsertPoint(some);
            builder.CreateStore(integer(someTag, 32), tagAddress);
            if (!unit(storedId)) copy(storedId, payload(resultType(), address), found);
            builder.CreateBr(done);
            builder.SetInsertPoint(done);
            result = builder.CreateLoad(resultType(), address);
            return true;
        }
        case Opcode::count: result = builder.CreateExtractValue(argument(0), 1); return true;
        case Opcode::subscript:
        case Opcode::subscriptAddress: {
            const bool addressResult = instruction.opcode == Opcode::subscriptAddress;
            const auto baseId = values.at(instruction.operands[0]).type;
            const auto& baseType = type(baseId);
            auto* base = argument(0);
            auto* index = argument(1);
            if (baseType.kind == typing::TypeKind::string && !addressResult) {
                // Byte access is inline: a checked index, then one load. The
                // runtime has no function for it.
                auto* data = builder.CreateExtractValue(base, 0, temporary());
                auto* count = builder.CreateExtractValue(base, 1, temporary());
                auto* negative = builder.CreateICmpSLT(index, integer(0), temporary());
                auto* pastEnd = builder.CreateICmpSGE(index, count, temporary());
                trap(builder.CreateOr(negative, pastEnd, temporary()), "string index out of bounds");
                result = builder.CreateLoad(builder.getInt8Ty(),
                        builder.CreateGEP(builder.getInt8Ty(), data, index, temporary()), temporary());
                return true;
            }
            if (addressResult) base = builder.CreateLoad(llvmType(baseId), base, temporary());
            auto* data = builder.CreateExtractValue(base, 0, temporary());
            auto* count = builder.CreateExtractValue(base, 1, temporary());
            llvm::Value* address = nullptr;
            if (baseType.kind == typing::TypeKind::array) {
                auto* nullData = builder.CreateICmpEQ(data, nullPointer(), temporary());
                auto* negative = builder.CreateICmpSLT(index, integer(0), temporary());
                auto* pastEnd = builder.CreateICmpSGE(index, count, temporary());
                trap(builder.CreateOr(nullData, builder.CreateOr(negative, pastEnd, temporary()), temporary()),
                        "array index out of bounds");
                if (!addressResult && unit(instruction.result->type)) {
                    result = llvm::Constant::getNullValue(resultType());
                    return true;
                }
                // Allocation/growth validates byte counts. A checked element
                // index can use typed GEP without another runtime header lookup.
                address = builder.CreateGEP(resultType(), data, index);
            } else if (baseType.kind == typing::TypeKind::dictionary) {
                const auto keyId = baseType.arguments.at(0);
                address = call("joyeer_dictionary_at_abi", pointer(),
                        {data, count, spill(index), integer(layout(keyId).size),
                         integer(static_cast<uint64_t>(keyKind(keyId)), 32)});
            } else {
                report(DiagnosticId::unsupportedInstruction, instruction.span,
                        "subscript is not implemented for type '" + baseType.name + "'");
                return false;
            }
            result = address;
            if (!addressResult) {
                result = unit(instruction.result->type)
                        ? static_cast<llvm::Value*>(llvm::Constant::getNullValue(resultType()))
                        : builder.CreateLoad(resultType(), address);
            }
            return true;
        }
        default: return false;
    }
}

} // namespace joyeer::llvmbackend::native