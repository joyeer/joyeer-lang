#include "native_ir.h"

#include <algorithm>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

namespace {
size_t caseTag(const ir::EnumerationDefinition& definition, std::string_view name) {
    for (size_t index = 0; index < definition.cases.size(); ++index) {
        if (definition.cases[index].name == name) return index;
    }
    throw std::runtime_error("missing concrete enum case '" + std::string(name) + "'");
}
}

bool Builder::matchesHostValue(ir::TypeId id, hostbuiltins::ValueKind kind) {
    const auto& record = type(id);
    using Kind = hostbuiltins::ValueKind;
    switch (kind) {
        case Kind::string: return record.kind == typing::TypeKind::string;
        case Kind::integer: return record.kind == typing::TypeKind::integer;
        case Kind::unit: return unit(id);
        case Kind::strings:
            return record.kind == typing::TypeKind::array && record.arguments.size() == 1 &&
                    type(record.arguments[0]).kind == typing::TypeKind::string;
        case Kind::fileKind: return record.kind == typing::TypeKind::enumeration && record.name == "FileKind";
        case Kind::processStatus: return record.kind == typing::TypeKind::enumeration && record.name == "ProcessStatus";
    }
    return false;
}

std::vector<size_t> Builder::enumTags(ir::TypeId id, const hostbuiltins::Enumeration& shape) {
    std::vector<size_t> tags;
    const auto found = enumerations.find(id);
    if (type(id).kind == typing::TypeKind::enumeration && type(id).name == shape.name &&
            found != enumerations.end() && found->second->cases.size() == shape.cases.size()) {
        for (const auto name : shape.cases) {
            for (size_t index = 0; index < found->second->cases.size(); ++index) {
                const auto& candidate = found->second->cases[index];
                if (candidate.name != name) continue;
                if ((!shape.integerPayload && candidate.payloadTypes.empty()) ||
                        (shape.integerPayload && candidate.payloadTypes.size() == 1 &&
                         type(candidate.payloadTypes[0]).kind == typing::TypeKind::integer)) {
                    tags.push_back(index);
                }
                break;
            }
        }
    }
    if (tags.size() != shape.cases.size()) {
        report(DiagnosticId::unsupportedExternal, {}, "invalid host enumeration shape for '" + std::string(shape.name) + "'");
        throw std::runtime_error("invalid host enumeration");
    }
    return tags;
}

llvm::Value* Builder::hostEnum(ir::TypeId id, llvm::Value* destination,
        llvm::Value* nativeTag, const hostbuiltins::Enumeration& shape) {
    const auto tags = enumTags(id, shape);
    auto* below = builder.CreateICmpULT(nativeTag, integer(static_cast<uint64_t>(shape.firstAbiTag), 32), temporary());
    auto* above = builder.CreateICmpUGT(nativeTag, integer(static_cast<uint64_t>(shape.firstAbiTag) + tags.size() - 1, 32), temporary());
    trap(builder.CreateOr(below, above, temporary()), "invalid native " + std::string(shape.name) + " tag");
    llvm::Value* selected = integer(tags.front(), 32);
    // Source declaration order is independent of the stable runtime ABI tags.
    for (size_t index = 1; index < tags.size(); ++index) {
        selected = builder.CreateSelect(builder.CreateICmpEQ(nativeTag,
                integer(static_cast<uint64_t>(shape.firstAbiTag) + index, 32), temporary()),
                integer(tags[index], 32), selected, temporary());
    }
    builder.CreateStore(llvm::Constant::getNullValue(llvmType(id)), destination);
    builder.CreateStore(selected, field(llvmType(id), destination, 0));
    return destination;
}

llvm::Value* Builder::emitHostCall(const ir::Function& function,
        llvm::ArrayRef<llvm::Value*> arguments, const ir::Instruction& instruction) {
    const auto* descriptor = hostbuiltins::findFunction(function.name);
    const auto& result = type(function.resultType);
    const auto invalidSignature = [&]() -> llvm::Value* {
        report(DiagnosticId::unsupportedExternal, instruction.span,
                "invalid signature for host function '" + function.name + "'");
        return nullptr;
    };
    if (descriptor == nullptr || !instruction.result.has_value() ||
            arguments.size() != descriptor->parameters.size() || result.kind != typing::TypeKind::result ||
            result.arguments.size() != 2 || !matchesHostValue(result.arguments[0], descriptor->result) ||
            !enumerations.contains(function.resultType)) return invalidSignature();
    const auto successId = result.arguments[0];
    const auto errorId = result.arguments[1];
    const auto& cases = *enumerations.at(function.resultType);
    const auto okTag = caseTag(cases, "Ok");
    const auto errorTag = caseTag(cases, "Err");
    if (cases.cases[okTag].payloadTypes != std::vector<ir::TypeId>{successId} ||
            cases.cases[errorTag].payloadTypes != std::vector<ir::TypeId>{errorId}) {
        report(DiagnosticId::unsupportedExternal, instruction.span, "host function requires concrete Result cases");
        return nullptr;
    }
    const auto* errorShape = hostbuiltins::findEnumeration(descriptor->error);
    enumTags(errorId, *errorShape);
    const bool process = descriptor->result == hostbuiltins::ValueKind::processStatus;
    const bool enumSuccess = process || descriptor->result == hostbuiltins::ValueKind::fileKind;
    const auto* successShape = enumSuccess
            ? hostbuiltins::findEnumeration(process ? "ProcessStatus" : "FileKind") : nullptr;
    if (successShape != nullptr) enumTags(successId, *successShape);
    std::vector<llvm::Value*> abiArguments;
    llvm::Value* output = nullptr;
    llvm::Value* status = nullptr;
    if (descriptor->result != hostbuiltins::ValueKind::unit) {
        auto* outputType = enumSuccess ? builder.getInt32Ty() : llvmType(successId);
        output = stack(outputType);
        builder.CreateStore(llvm::Constant::getNullValue(outputType), output);
        abiArguments.push_back(output);
    }
    if (process) {
        status = stack(builder.getInt64Ty());
        builder.CreateStore(integer(0), status);
        abiArguments.push_back(status);
    }
    auto* error = stack(builder.getInt64Ty());
    builder.CreateStore(integer(0), error);
    abiArguments.push_back(error);
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (!matchesHostValue(values.at(instruction.operands[index]).type, descriptor->parameters[index].kind)) {
            report(DiagnosticId::unsupportedExternal, instruction.span,
                    "invalid argument to host function '" + function.name + "'");
            return nullptr;
        }
        if (descriptor->parameters[index].kind == hostbuiltins::ValueKind::integer) {
            abiArguments.push_back(arguments[index]);
        } else {
            // Flatten language handles into borrowed data/count pairs. The
            // runtime writes owned results into caller-provided stack storage.
            abiArguments.push_back(builder.CreateExtractValue(arguments[index], 0, temporary()));
            abiArguments.push_back(builder.CreateExtractValue(arguments[index], 1, temporary()));
        }
    }
    auto* nativeTag = call(descriptor->runtimeName, builder.getInt32Ty(), abiArguments);
    auto* destination = stack(llvmType(function.resultType));
    builder.CreateStore(llvm::Constant::getNullValue(llvmType(function.resultType)), destination);
    auto* tagAddress = field(llvmType(function.resultType), destination, 0);
    auto* resultPayload = payload(llvmType(function.resultType), destination);
    auto* ok = llvm::BasicBlock::Create(context(), "host.ok." + temporary(), activeFunction);
    auto* failed = llvm::BasicBlock::Create(context(), "host.error." + temporary(), activeFunction);
    auto* done = llvm::BasicBlock::Create(context(), "host.done." + temporary(), activeFunction);
    builder.CreateCondBr(builder.CreateICmpEQ(nativeTag, integer(0, 32), temporary()), ok, failed);
    builder.SetInsertPoint(ok);
    builder.CreateStore(integer(okTag, 32), tagAddress);
    if (enumSuccess) {
        hostEnum(successId, resultPayload, builder.CreateLoad(builder.getInt32Ty(), output, temporary()), *successShape);
        if (process) {
            builder.CreateStore(builder.CreateLoad(builder.getInt64Ty(), status, temporary()),
                    payload(llvmType(successId), resultPayload));
        }
    } else if (!unit(successId)) {
        builder.CreateStore(builder.CreateLoad(llvmType(successId), output, temporary()), resultPayload);
    }
    builder.CreateBr(done);
    builder.SetInsertPoint(failed);
    builder.CreateStore(integer(errorTag, 32), tagAddress);
    hostEnum(errorId, resultPayload, nativeTag, *errorShape);
    builder.CreateStore(builder.CreateLoad(builder.getInt64Ty(), error, temporary()),
            payload(llvmType(errorId), resultPayload));
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    return builder.CreateLoad(llvmType(function.resultType), destination);
}

llvm::Value* Builder::emitCall(const ir::Instruction& instruction) {
    const auto found = std::find_if(source.functions.begin(), source.functions.end(), [&instruction](const auto& function) {
        return function.id == instruction.callee;
    });
    if (found == source.functions.end()) return nullptr;
    const auto& callee = *found;
    std::vector<llvm::Value*> arguments;
    for (const auto id : instruction.operands) arguments.push_back(operand(id));
    if (!callee.isExternal) return builder.CreateCall(functions.at(callee.id), arguments);
    if (hostbuiltins::findFunction(callee.name) != nullptr) return emitHostCall(callee, arguments, instruction);
    if (callee.name == "print" && arguments.size() == 1) {
        const auto& argumentType = type(values.at(instruction.operands[0]).type);
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
    if (callee.name == "readFile" && arguments.size() == 1) {
        const auto invalid = [&]() -> llvm::Value* {
            report(DiagnosticId::unsupportedExternal, instruction.span, "readFile result must be Result<String, IOError>");
            return nullptr;
        };
        if (!arguments[0]->getType()->isStructTy() || !enumerations.contains(callee.resultType)) return invalid();
        const auto& cases = *enumerations.at(callee.resultType);
        const auto okTag = caseTag(cases, "Ok");
        const auto errorTag = caseTag(cases, "Err");
        if (cases.cases[okTag].payloadTypes.size() != 1 || cases.cases[errorTag].payloadTypes.size() != 1) return invalid();
        const auto stringId = cases.cases[okTag].payloadTypes[0];
        const auto errorId = cases.cases[errorTag].payloadTypes[0];
        if (type(stringId).kind != typing::TypeKind::string || !enumerations.contains(errorId)) return invalid();
        const auto& errors = *enumerations.at(errorId);
        const std::array<std::string_view, 4> names {"NotFound", "PermissionDenied", "InvalidPath", "Other"};
        std::vector<size_t> tags;
        for (const auto name : names) {
            const auto tag = caseTag(errors, name);
            if (errors.cases[tag].payloadTypes.size() != 1 ||
                    type(errors.cases[tag].payloadTypes[0]).kind != typing::TypeKind::integer) return invalid();
            tags.push_back(tag);
        }
        auto* stringAddress = stack(llvmType(stringId));
        auto* codeAddress = stack(builder.getInt64Ty());
        builder.CreateStore(llvm::Constant::getNullValue(llvmType(stringId)), stringAddress);
        builder.CreateStore(integer(0), codeAddress);
        auto* errorKind = call("joyeer_read_file_abi", builder.getInt32Ty(),
                {stringAddress, codeAddress, builder.CreateExtractValue(arguments[0], 0, temporary()),
                 builder.CreateExtractValue(arguments[0], 1, temporary())});
        auto* destination = stack(llvmType(callee.resultType));
        builder.CreateStore(llvm::Constant::getNullValue(llvmType(callee.resultType)), destination);
        auto* tagAddress = field(llvmType(callee.resultType), destination, 0);
        auto* resultPayload = payload(llvmType(callee.resultType), destination);
        auto* ok = llvm::BasicBlock::Create(context(), "readfile.ok." + temporary(), activeFunction);
        auto* failed = llvm::BasicBlock::Create(context(), "readfile.error." + temporary(), activeFunction);
        auto* done = llvm::BasicBlock::Create(context(), "readfile.done." + temporary(), activeFunction);
        builder.CreateCondBr(builder.CreateICmpEQ(errorKind, integer(JOYEER_IO_ERROR_NONE, 32), temporary()), ok, failed);
        builder.SetInsertPoint(ok);
        builder.CreateStore(integer(okTag, 32), tagAddress);
        builder.CreateStore(builder.CreateLoad(llvmType(stringId), stringAddress, temporary()), resultPayload);
        builder.CreateBr(done);
        builder.SetInsertPoint(failed);
        builder.CreateStore(integer(errorTag, 32), tagAddress);
        llvm::Value* selected = integer(tags[3], 32);
        for (size_t index = 0; index < 3; ++index) {
            selected = builder.CreateSelect(builder.CreateICmpEQ(errorKind, integer(index + 1, 32), temporary()),
                    integer(tags[index], 32), selected, temporary());
        }
        builder.CreateStore(llvm::Constant::getNullValue(llvmType(errorId)), resultPayload);
        builder.CreateStore(selected, field(llvmType(errorId), resultPayload, 0));
        builder.CreateStore(builder.CreateLoad(builder.getInt64Ty(), codeAddress, temporary()),
                payload(llvmType(errorId), resultPayload));
        builder.CreateBr(done);
        builder.SetInsertPoint(done);
        return builder.CreateLoad(llvmType(callee.resultType), destination);
    }
    report(DiagnosticId::unsupportedExternal, instruction.span, "unsupported external function '" + callee.name + "'");
    return nullptr;
}

} // namespace joyeer::llvmbackend::native