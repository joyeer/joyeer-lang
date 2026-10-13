#include "native_ir.h"

#include <algorithm>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

namespace {
size_t alignTo(size_t size, size_t alignment) {
    return (size + alignment - 1) / alignment * alignment;
}
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
                result = call("joyeer_string_byte_at_abi", builder.getInt8Ty(),
                        {builder.CreateExtractValue(base, 0, temporary()),
                         builder.CreateExtractValue(base, 1, temporary()), index});
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