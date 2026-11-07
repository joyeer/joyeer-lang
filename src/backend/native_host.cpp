#include "native_ir.h"
#include "compiler/frontend/intrinsics.h"

#include <algorithm>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

// Spec §19.4.5. Loads and stores are unaligned-safe; the atomic add requires an
// 8-byte-aligned location, which the caller guarantees.
llvm::Value* Builder::emitIntrinsic(const intrinsic::Descriptor& descriptor,
        llvm::ArrayRef<llvm::Value*> arguments) {
    using intrinsic::Operation;
    if (descriptor.operation == Operation::addressOf) {
        return builder.CreatePtrToInt(arguments[0], builder.getInt64Ty(), temporary());
    }
    if (descriptor.operation == Operation::staticSlot) {
        // One zeroed table per program: every object that uses slots defines the
        // same linkonce symbol in a comdat, and the linker keeps a single copy.
        const std::string symbol(intrinsic::staticSlotSymbol);
        auto* tableType = llvm::ArrayType::get(builder.getInt64Ty(), static_cast<uint64_t>(intrinsic::staticSlotCount));
        auto* table = module().getNamedGlobal(symbol);
        if (table == nullptr) {
            table = new llvm::GlobalVariable(module(), tableType, false,
                    llvm::GlobalValue::LinkOnceODRLinkage, llvm::ConstantAggregateZero::get(tableType), symbol);
            table->setAlignment(llvm::Align(8));
            auto* comdat = module().getOrInsertComdat(symbol);
            comdat->setSelectionKind(llvm::Comdat::Any);
            table->setComdat(comdat);
        }
        auto* outside = builder.CreateICmpUGE(arguments[0],
                integer(static_cast<uint64_t>(intrinsic::staticSlotCount)), temporary());
        trap(outside, "static slot index out of range");
        llvm::Value* zero = integer(0);
        auto* slot = builder.CreateInBoundsGEP(tableType, table, {zero, arguments[0]}, temporary());
        return builder.CreatePtrToInt(slot, builder.getInt64Ty(), temporary());
    }
    if (descriptor.operation == Operation::literalAddress) {
        // The argument is a string literal, so its data is a private constant
        // global. Re-emit those bytes with a NUL terminator and return the address.
        // The data pointer is either the constant itself or an extractvalue of the
        // literal's constant {pointer, count} struct, depending on folding.
        llvm::Value* pointerValue = arguments[0];
        if (auto* extraction = llvm::dyn_cast<llvm::ExtractValueInst>(pointerValue)) {
            if (auto* literal = llvm::dyn_cast<llvm::ConstantStruct>(extraction->getAggregateOperand());
                    literal != nullptr && extraction->getNumIndices() == 1 && extraction->getIndices()[0] == 0) {
                pointerValue = literal->getOperand(0);
            }
        }
        auto* global = llvm::dyn_cast<llvm::GlobalVariable>(pointerValue->stripPointerCasts());
        llvm::Constant* initializer = global != nullptr && global->hasInitializer()
                ? global->getInitializer() : nullptr;
        std::string bytes;
        bool literal = false;
        if (const auto* array = llvm::dyn_cast_or_null<llvm::ConstantDataArray>(initializer)) {
            bytes = array->getRawDataValues().str();
            literal = true;
        } else if (initializer != nullptr && llvm::isa<llvm::ConstantAggregateZero>(initializer) &&
                   initializer->getType()->isArrayTy()) {
            // An empty literal, or one made only of NUL bytes.
            bytes.assign(initializer->getType()->getArrayNumElements(), '\0');
            literal = true;
        }
        if (!literal) {
            report(DiagnosticId::unsupportedExternal, {},
                    "joyeer_intrinsic_literal_address requires a string literal argument");
            return nullptr;
        }
        auto* terminated = stringLiteral(bytes, true);
        return builder.CreatePtrToInt(terminated, builder.getInt64Ty(), temporary());
    }
    if (descriptor.operation == Operation::callVoid1 || descriptor.operation == Operation::callVoid2) {
        // The callee is the target's ordinary C function taking addresses.
        auto* function = builder.CreateIntToPtr(arguments[0], pointer(), temporary());
        std::vector<llvm::Type*> parameterTypes;
        std::vector<llvm::Value*> callArguments;
        for (size_t index = 1; index < arguments.size(); ++index) {
            parameterTypes.push_back(pointer());
            callArguments.push_back(builder.CreateIntToPtr(arguments[index], pointer(), temporary()));
        }
        builder.CreateCall(llvm::FunctionType::get(builder.getVoidTy(), parameterTypes, false),
                function, callArguments);
        return nullptr;
    }
    auto* address = builder.CreateIntToPtr(arguments[0], pointer(), temporary());
    switch (descriptor.operation) {
        case Operation::load8:
            return builder.CreateAlignedLoad(builder.getInt8Ty(), address, llvm::MaybeAlign(1), temporary());
        case Operation::load16:
            return builder.CreateAlignedLoad(builder.getInt16Ty(), address, llvm::MaybeAlign(1), temporary());
        case Operation::load32:
            return builder.CreateAlignedLoad(builder.getInt32Ty(), address, llvm::MaybeAlign(1), temporary());
        case Operation::load64:
            return builder.CreateAlignedLoad(builder.getInt64Ty(), address, llvm::MaybeAlign(1), temporary());
        case Operation::store8:
        case Operation::store16:
        case Operation::store32:
        case Operation::store64:
            builder.CreateAlignedStore(arguments[1], address, llvm::MaybeAlign(1));
            return nullptr;
        case Operation::atomicAdd:
            return builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, address, arguments[1],
                    llvm::MaybeAlign(8), llvm::AtomicOrdering::SequentiallyConsistent);
        case Operation::addressOf:
        case Operation::callVoid1:
        case Operation::callVoid2:
        case Operation::literalAddress:
        case Operation::staticSlot:
            break;
    }
    return nullptr;
}

llvm::Value* Builder::emitCall(const ir::Instruction& instruction) {
    if (!instruction.callee.has_value()) return nullptr;
    const auto found = sourceFunctions.find(*instruction.callee);
    if (found == sourceFunctions.end()) return nullptr;
    const auto& callee = *found->second;
    std::vector<llvm::Value*> arguments;
    for (const auto id : instruction.operands) arguments.push_back(operand(id));
    if (callee.isExternal && callee.nativeBinding.has_value()) {
        std::vector<llvm::Value*> nativeArguments;
        for (size_t index = 0; index < arguments.size(); ++index) {
            const auto& parameter = callee.parameters[index];
            const auto kind = type(parameter.value.type).kind;
            if (callee.nativeBinding->passesPointer(index)) {
                // Borrowed bytes arrive as a value; an inout buffer as the address of its handle.
                auto* handle = parameter.value.category == ir::ValueCategory::address
                        ? builder.CreateLoad(llvmType(parameter.value.type), arguments[index], temporary())
                        : arguments[index];
                nativeArguments.push_back(builder.CreateExtractValue(handle, 0, temporary()));
            } else if (parameter.isInitializing) {
                if (kind == typing::TypeKind::string) {
                    builder.CreateStore(llvm::Constant::getNullValue(llvmType(parameter.value.type)), arguments[index]);
                } else {
                    const auto element = type(type(parameter.value.type).arguments.at(0)).kind;
                    call(element == typing::TypeKind::uint8
                                    ? "joyeer_native_bytes_init" : "joyeer_native_strings_init",
                            builder.getVoidTy(), {arguments[index]});
                }
                nativeArguments.push_back(arguments[index]);
            } else if (kind == typing::TypeKind::string || kind == typing::TypeKind::array) {
                nativeArguments.push_back(builder.CreateExtractValue(arguments[index], 0, temporary()));
                nativeArguments.push_back(builder.CreateExtractValue(arguments[index], 1, temporary()));
            } else {
                nativeArguments.push_back(arguments[index]);
            }
        }
        if (const auto* descriptor = intrinsic::find(callee.nativeBinding->symbol)) {
            return emitIntrinsic(*descriptor, nativeArguments);
        }
        auto* function = functions.at(callee.id);
        auto* invocation = builder.CreateCall(function, nativeArguments);
        invocation->setAttributes(function->getAttributes());
        return invocation;
    }
    if (!callee.isExternal || callee.nativeBinding.has_value()) {
        auto* function = functions.at(callee.id);
        auto* invocation = builder.CreateCall(function, arguments);
        invocation->setAttributes(function->getAttributes());
        return invocation;
    }
    if (callee.name == "print" && arguments.size() == 1) {
        const auto& argumentType = type(values.at(instruction.operands[0]).type);
        if (typing::isSignedInteger(argumentType.kind)) {
            return call("joyeer_print_int", builder.getVoidTy(),
                    {builder.CreateSExtOrTrunc(arguments[0], builder.getInt64Ty())});
        }
        if (typing::isUnsignedInteger(argumentType.kind) && argumentType.kind != typing::TypeKind::uint8) {
            return call("joyeer_print_uint", builder.getVoidTy(),
                    {builder.CreateZExtOrTrunc(arguments[0], builder.getInt64Ty())});
        }
        if (typing::isFloating(argumentType.kind)) {
            return call(argumentType.kind == typing::TypeKind::float32
                            ? "joyeer_print_float" : "joyeer_print_double",
                    builder.getVoidTy(), arguments);
        }
        std::string_view name;
        switch (argumentType.kind) {
            case typing::TypeKind::integer: name = "joyeer_print_int"; break;
            case typing::TypeKind::boolean: name = "joyeer_print_bool"; break;
            case typing::TypeKind::uint8: name = "joyeer_print_byte"; break;
            case typing::TypeKind::string:
                return call("joyeer_print_string_abi", builder.getVoidTy(),
                        {builder.CreateExtractValue(arguments[0], 0, temporary()),
                         builder.CreateExtractValue(arguments[0], 1, temporary())});
            default:
                report(DiagnosticId::unsupportedExternal, instruction.span,
                        "print is not implemented for type '" + argumentType.name + "'");
                return nullptr;
        }
        return call(name, builder.getVoidTy(), arguments);
    }
    if ((callee.name == "byteToInt" || callee.name == "byteToString") && arguments.size() == 1) {
        if (!arguments[0]->getType()->isIntegerTy(8)) {
            report(DiagnosticId::unsupportedExternal, instruction.span, callee.name + " requires a UInt8 argument");
            return nullptr;
        }
        if (callee.name == "byteToInt") return builder.CreateZExt(arguments[0], builder.getInt64Ty());
        auto* destination = stack(llvmType(callee.resultType));
        call("joyeer_byte_to_string_abi", builder.getVoidTy(), {destination, arguments[0]});
        return builder.CreateLoad(llvmType(callee.resultType), destination);
    }
    report(DiagnosticId::unsupportedExternal, instruction.span, "unsupported external function '" + callee.name + "'");
    return nullptr;
}

} // namespace joyeer::llvmbackend::native