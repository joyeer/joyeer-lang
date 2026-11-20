#include "native_ir.h"

#include <stdexcept>

namespace joyeer::llvmbackend::native {

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
    if (!isArrayRole(id)) throw std::runtime_error("unsupported constant type");

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
            {llvm::ConstantExpr::getPtrToInt(first, builder.getInt64Ty()), integer(count), integer(count)});
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
        case Opcode::constantValue:
            result = constantArray(source.constants.at(static_cast<size_t>(instruction.integerValue)));
            return true;
        default: return false;
    }
}

} // namespace joyeer::llvmbackend::native