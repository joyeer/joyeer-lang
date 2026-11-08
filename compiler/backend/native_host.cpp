#include "native_ir.h"
#include "compiler/frontend/intrinsics.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "llvm/IR/InlineAsm.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace joyeer::llvmbackend::native {

// Spec §19.4.5. Loads and stores are unaligned-safe; the atomic add requires an
// 8-byte-aligned location, which the caller guarantees.
llvm::Value* Builder::emitIntrinsic(const intrinsic::Descriptor& descriptor,
        llvm::ArrayRef<llvm::Value*> arguments) {
    using intrinsic::Operation;
    if (descriptor.operation == Operation::addressOf) {
        return builder.CreatePtrToInt(arguments[0], builder.getInt64Ty(), temporary());
    }
    if (descriptor.operation == Operation::memoryCopy || descriptor.operation == Operation::memoryMove ||
        descriptor.operation == Operation::memoryFill) {
        // LLVM's own memory intrinsics: small constant sizes become inline moves,
        // the rest a call to the platform's memcpy, memmove or memset.
        auto* destination = builder.CreateIntToPtr(arguments[0], pointer(), temporary());
        if (descriptor.operation == Operation::memoryFill) {
            builder.CreateMemSet(destination, arguments[1], arguments[2], llvm::MaybeAlign(1));
            return nullptr;
        }
        auto* source = builder.CreateIntToPtr(arguments[1], pointer(), temporary());
        if (descriptor.operation == Operation::memoryCopy) {
            builder.CreateMemCpy(destination, llvm::MaybeAlign(1), source, llvm::MaybeAlign(1), arguments[2]);
        } else {
            builder.CreateMemMove(destination, llvm::MaybeAlign(1), source, llvm::MaybeAlign(1), arguments[2]);
        }
        return nullptr;
    }
    if (descriptor.operation == Operation::doubleBits || descriptor.operation == Operation::floatBits) {
        return builder.CreateBitCast(arguments[0],
                descriptor.operation == Operation::doubleBits ? builder.getInt64Ty() : builder.getInt32Ty(), temporary());
    }
    if (descriptor.operation == Operation::targetArchitecture) {
        const llvm::Triple target(llvm::sys::getDefaultTargetTriple());
        return integer(target.getArch() == llvm::Triple::x86_64 ? 1 : target.getArch() == llvm::Triple::aarch64 ? 2 : 0);
    }
    if (descriptor.operation == Operation::systemCall) {
        // Only the Linux kernel promises a stable system call interface, and only a Linux
        // program can use this (the runtime selects it with `@platform(linux)`), but the IR is
        // produced for any target so that a Linux unit can be validated on another host. The
        // call number and the six arguments go in the architecture's registers; the kernel
        // result comes back in the first. Constraint 0 ties the number to the result
        // register (rax) or the first argument to it (x0), which the call overwrites.
        const llvm::Triple target(llvm::sys::getDefaultTargetTriple());
        const bool x86 = target.getArch() == llvm::Triple::x86_64;
        if (!x86 && target.getArch() != llvm::Triple::aarch64) {
            report(DiagnosticId::unsupportedExternal, {},
                    "joyeer_intrinsic_syscall requires an x86-64 or AArch64 target");
            return integer(0);
        }
        auto* integerType = builder.getInt64Ty();
        auto* assemblyType = llvm::FunctionType::get(integerType, std::vector<llvm::Type*>(7, integerType), false);
        auto* assembly = llvm::InlineAsm::get(assemblyType, x86 ? "syscall" : "svc #0",
                x86 ? "={rax},0,{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory},~{dirflag},~{fpsr},~{flags}"
                    : "={x0},{x8},0,{x1},{x2},{x3},{x4},{x5},~{memory}",
                true);
        return builder.CreateCall(assemblyType, assembly, arguments, temporary());
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
    if (descriptor.operation == Operation::literalAddress || descriptor.operation == Operation::symbolAddress) {
        // The argument is a string literal, so its data is a private constant
        // global. A literal address re-emits those bytes with a NUL terminator; a
        // symbol address reads them as the name of a function or data symbol.
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
                    std::string(descriptor.symbol) + " requires a string literal argument");
            return nullptr;
        }
        if (descriptor.operation == Operation::symbolAddress) {
            // A function defined in this module is taken directly; anything else is
            // an external symbol resolved by the linker.
            if (auto* function = module().getFunction(bytes)) {
                return builder.CreatePtrToInt(function, builder.getInt64Ty(), temporary());
            }
            return builder.CreatePtrToInt(declareDataSymbol(bytes), builder.getInt64Ty(), temporary());
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
        case Operation::symbolAddress:
        case Operation::staticSlot:
        case Operation::memoryCopy:
        case Operation::memoryMove:
        case Operation::memoryFill:
        case Operation::doubleBits:
        case Operation::floatBits:
        case Operation::systemCall:
        case Operation::targetArchitecture:
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
    if (callee.isExternal && callee.nativeBinding.has_value() && callee.nativeBinding->dataSymbol) {
        // The value of a data-symbol binding is the symbol's address; nothing is called.
        return builder.CreatePtrToInt(declareDataSymbol(callee.nativeBinding->symbol),
                builder.getInt64Ty(), temporary());
    }
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