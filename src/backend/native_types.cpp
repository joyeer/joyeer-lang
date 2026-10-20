#include "native_ir.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Verifier.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

namespace {
size_t alignTo(size_t size, size_t alignment) {
    return (size + alignment - 1) / alignment * alignment;
}

llvm::Attribute::AttrKind cExtension(const llvm::Triple& target, llvm::Type* type, bool isSigned) {
    if (!type->isIntegerTy() || type->getIntegerBitWidth() >= 32) return llvm::Attribute::None;
    const bool extended = target.isOSDarwin() ||
            (target.getArch() == llvm::Triple::x86_64 &&
             (!target.isOSWindows() || type->isIntegerTy(1)));
    return !extended ? llvm::Attribute::None :
            isSigned ? llvm::Attribute::SExt : llvm::Attribute::ZExt;
}
}

Builder::Builder(ModuleStorage& storage, const ir::Module& source, EmitOptions options)
    : storage(storage), source(source), options(options), builder(storage.context) {
    storage.module = std::make_unique<llvm::Module>(source.sourceName, context());
    module().setSourceFileName(source.sourceName);
    for (const auto& value : source.types) types.emplace(value.id, &value);
    for (const auto& value : source.structures) structures.emplace(value.type, &value);
    for (const auto& value : source.enumerations) enumerations.emplace(value.type, &value);
    for (const auto& value : source.functions) sourceFunctions.emplace(value.id, &value);
    for (const auto& value : source.debugScopes) sourceDebugScopes.emplace(value.id, &value);
    for (const auto& value : source.debugVariables) sourceDebugVariables.emplace(value.id, &value);
}

void Builder::report(DiagnosticId id, SourceSpan span, std::string message) {
    diagnostics.push_back({id, span,
            currentFunction == nullptr ? std::nullopt : std::optional(currentFunction->id),
            currentBlock == nullptr ? std::nullopt : std::optional(currentBlock->id),
            std::move(message)});
}

std::vector<Diagnostic> Builder::build() {
    const auto verification = ir::Verifier().verify(source);
    if (!verification.succeeded()) {
        for (const auto& error : verification.errors) {
            diagnostics.push_back({DiagnosticId::invalidModule, {}, error.function,
                    error.block, std::string(ir::verificationErrorName(error.id)) + ": " + error.message});
        }
        return std::move(diagnostics);
    }
    try {
        const llvm::Triple target(llvm::sys::getDefaultTargetTriple());
        const bool supportedLinux = target.isOSLinux() &&
                (target.getEnvironment() == llvm::Triple::GNU || target.getEnvironment() == llvm::Triple::Musl);
        const bool supportedTarget =
                (target.getArch() == llvm::Triple::x86_64 || target.getArch() == llvm::Triple::aarch64) &&
                (supportedLinux || target.isMacOSX() || target.isWindowsMSVCEnvironment());
        for (const auto& function : source.functions) {
            if (function.nativeBinding.has_value() && !supportedTarget) {
                report(DiagnosticId::unsupportedType, function.nativeBinding->span,
                        "C scalar ABI is not supported for target '" + target.str() + "'");
                return std::move(diagnostics);
            }
        }
        destruction.emplace(source);
        defineTypes();
        if (options.emitLineTables) {
            if (source.sourceFile(0) == nullptr) {
                report(DiagnosticId::invalidModule, {},
                        "LLVM line-table emission requires module source info");
                return std::move(diagnostics);
            }
            initializeDebug();
        }
        // Reserve C linkage names first, so private generated helpers can never
        // take a native symbol and force LLVM to rename the public entry.
        for (const auto& function : source.functions) {
            if (function.nativeBinding.has_value()) declareNativeFunction(function);
        }
        emitOwnership();
        // Declare source functions before emitting their bodies so recursive
        // and forward calls do not depend on source declaration order.
        for (const auto& function : source.functions) {
            if (!function.isExternal) {
                functions.emplace(function.id, declareFunction(function));
            } else if (function.nativeBinding.has_value()) {
                functions.emplace(function.id, declareNativeFunction(function));
            }
        }
        for (const auto& function : source.functions) {
            if (!function.isExternal) emitFunction(function);
            if (!diagnostics.empty()) break;
        }
        if (diagnostics.empty()) {
            for (const auto& function : source.functions) {
                if (function.nativeBinding.has_value() &&
                    function.nativeBinding->kind == NativeBindingKind::cExport) {
                    emitNativeExport(function);
                }
            }
            if (!source.isRuntimeObject) emitEntry();
        }
        if (debug) {
            debug->finalize();
            // Preserve the existing textual debug-intrinsic contract while
            // constructing all metadata through DIBuilder's object API.
            module().convertFromNewDbgValues();
        }
        if (diagnostics.empty()) {
            std::string message;
            llvm::raw_string_ostream stream(message);
            if (llvm::verifyModule(module(), &stream)) {
                report(DiagnosticId::invalidModule, {}, "LLVM verification failed: " + message);
            }
        }
    } catch (const std::exception& error) {
        if (diagnostics.empty()) {
            report(DiagnosticId::unsupportedInstruction, {}, error.what());
        }
    }
    return std::move(diagnostics);
}

llvm::Type* Builder::llvmType(ir::TypeId id) {
    if (const auto found = loweredTypes.find(id); found != loweredTypes.end()) return found->second;
    const auto found = types.find(id);
    if (found == types.end()) {
        report(DiagnosticId::unsupportedType, {}, "unknown Joyeer IR type " + std::to_string(id));
        throw std::runtime_error("unknown Joyeer IR type");
    }
    llvm::Type* result = nullptr;
    std::string name;
    if (typing::isInteger(found->second->kind)) {
        result = builder.getIntNTy(typing::scalarBitWidth(found->second->kind));
    } else if (found->second->kind == typing::TypeKind::float32) {
        result = builder.getFloatTy();
    } else if (found->second->kind == typing::TypeKind::float64) {
        result = builder.getDoubleTy();
    }
    switch (found->second->kind) {
        case typing::TypeKind::voidType: result = llvm::StructType::get(context()); break;
        case typing::TypeKind::never: result = builder.getVoidTy(); break;
        case typing::TypeKind::boolean: result = builder.getInt1Ty(); break;
        case typing::TypeKind::string: name = "joyeer.string"; break;
        case typing::TypeKind::array: name = "joyeer.array"; break;
        case typing::TypeKind::dictionary: name = "joyeer.dictionary"; break;
        case typing::TypeKind::structure:
        case typing::TypeKind::enumeration:
        case typing::TypeKind::optional:
        case typing::TypeKind::result:
            break;
        default: break;
    }
    if (!name.empty()) {
        auto* aggregate = llvm::StructType::getTypeByName(context(), name);
        if (aggregate == nullptr) {
            aggregate = llvm::StructType::create(context(), name);
            std::vector<llvm::Type*> fields {pointer(), builder.getInt64Ty()};
            if (found->second->kind != typing::TypeKind::string) fields.push_back(builder.getInt64Ty());
            aggregate->setBody(fields);
        }
        result = aggregate;
    }
    if (result == nullptr) {
        report(DiagnosticId::unsupportedType, {}, "LLVM lowering is not implemented for type '" +
                found->second->name + "'");
        throw std::runtime_error("unsupported Joyeer IR type");
    }
    loweredTypes.emplace(id, result);
    return result;
}

Layout Builder::layout(ir::TypeId id) {
    if (const auto found = layouts.find(id); found != layouts.end()) return found->second;
    const auto& info = type(id);
    if (!layoutStack.insert(id).second) {
        report(DiagnosticId::unsupportedType, {}, "recursive inline layout for type '" + info.name + "'");
        throw std::runtime_error("recursive inline layout");
    }
    Layout result;
    if (typing::isNumeric(info.kind)) {
        const auto size = typing::scalarBitWidth(info.kind) / 8;
        result = {size, size};
    }
    switch (info.kind) {
        case typing::TypeKind::voidType:
        case typing::TypeKind::never: break;
        case typing::TypeKind::boolean:
        case typing::TypeKind::uint8: result = {1, 1}; break;
        case typing::TypeKind::integer: result = {8, 8}; break;
        case typing::TypeKind::string: result = {16, 8}; break;
        case typing::TypeKind::array:
        case typing::TypeKind::dictionary: result = {24, 8}; break;
        case typing::TypeKind::structure:
            for (const auto& field : structures.at(id)->fields) {
                const auto fieldLayout = layout(field.type);
                result.size = alignTo(result.size, fieldLayout.alignment) + fieldLayout.size;
                result.alignment = std::max(result.alignment, fieldLayout.alignment);
            }
            result.size = alignTo(result.size, result.alignment);
            break;
        case typing::TypeKind::enumeration:
        case typing::TypeKind::optional:
        case typing::TypeKind::result: {
            size_t payloadSize = 0;
            for (const auto& enumCase : enumerations.at(id)->cases) {
                size_t size = 0;
                for (const auto payloadType : enumCase.payloadTypes) {
                    const auto valueLayout = layout(payloadType);
                    size = alignTo(size, valueLayout.alignment) + valueLayout.size;
                }
                payloadSize = std::max(payloadSize, alignTo(size, 8));
            }
            result = {8 + payloadSize, 8};
            break;
        }
        default:
            if (!typing::isNumeric(info.kind)) throw std::runtime_error("unsupported inline layout");
            break;
    }
    layoutStack.erase(id);
    layouts.emplace(id, result);
    return result;
}

std::vector<size_t> Builder::payloadOffsets(const ir::EnumCaseDefinition& definition) {
    std::vector<size_t> offsets;
    size_t offset = 0;
    for (const auto id : definition.payloadTypes) {
        const auto valueLayout = layout(id);
        offset = alignTo(offset, valueLayout.alignment);
        offsets.push_back(offset);
        offset += valueLayout.size;
    }
    return offsets;
}

void Builder::defineTypes() {
    // Opaque declarations break definition-order dependencies without allowing
    // recursive inline layouts, which are still rejected by layout().
    std::map<ir::TypeId, llvm::StructType*> aggregates;
    for (const auto& definition : source.structures) {
        aggregates.emplace(definition.type, llvm::StructType::create(context(),
                "joyeer.struct." + std::to_string(definition.type)));
    }
    for (const auto& definition : source.enumerations) {
        aggregates.emplace(definition.type, llvm::StructType::create(context(),
                "joyeer.enum." + std::to_string(definition.type)));
    }
    for (const auto& [id, aggregate] : aggregates) loweredTypes.emplace(id, aggregate);
    for (const auto& [id, aggregate] : aggregates) {
        if (const auto found = structures.find(id); found != structures.end()) {
            std::vector<llvm::Type*> fields;
            for (const auto& field : found->second->fields) fields.push_back(llvmType(field.type));
            aggregate->setBody(fields);
            layout(id);
        } else {
            const auto valueLayout = layout(id);
            aggregate->setBody({builder.getInt32Ty(),
                    llvm::ArrayType::get(builder.getInt64Ty(), (valueLayout.size - 8) / 8)});
        }
    }
}

llvm::Function* Builder::declareFunction(const ir::Function& function) {
    std::vector<llvm::Type*> parameters;
    for (const auto& parameter : function.parameters) {
        parameters.push_back(parameter.value.category == ir::ValueCategory::address
                ? pointer() : llvmType(parameter.value.type));
    }
    auto* result = unit(function.resultType) ? builder.getVoidTy() : llvmType(function.resultType);
    return llvm::Function::Create(llvm::FunctionType::get(result, parameters, false),
            source.isRuntimeObject || function.nativeBinding.has_value()
                    ? llvm::GlobalValue::InternalLinkage : llvm::GlobalValue::ExternalLinkage,
            "joyeer_fn_" + std::to_string(function.id), module());
}

llvm::Function* Builder::declareNativeFunction(const ir::Function& function) {
    std::vector<llvm::Type*> parameters;
    std::vector<typing::TypeKind> kinds;
    for (const auto& parameter : function.parameters) {
        parameters.push_back(llvmType(parameter.value.type));
        kinds.push_back(type(parameter.value.type).kind);
    }
    auto* result = unit(function.resultType) ? builder.getVoidTy() : llvmType(function.resultType);
    return declareCFunction(function.nativeBinding->symbol, result, parameters,
            kinds, type(function.resultType).kind);
}

llvm::Function* Builder::declareCFunction(std::string_view name, llvm::Type* result,
        llvm::ArrayRef<llvm::Type*> parameters,
        llvm::ArrayRef<typing::TypeKind> parameterKinds, typing::TypeKind resultKind) {
    const llvm::StringRef symbol(name.data(), name.size());
    auto* signature = llvm::FunctionType::get(result, parameters, false);
    const llvm::Triple target(llvm::sys::getDefaultTargetTriple());
    llvm::AttributeList attributes;
    const auto returnExtension = cExtension(target, result, typing::isSignedInteger(resultKind));
    if (returnExtension != llvm::Attribute::None) {
        attributes = attributes.addRetAttribute(context(), returnExtension);
    }
    for (unsigned index = 0; index < parameters.size(); ++index) {
        const auto extension = cExtension(target, parameters[index],
                !parameterKinds.empty() && typing::isSignedInteger(parameterKinds[index]));
        if (extension != llvm::Attribute::None) {
            attributes = attributes.addParamAttribute(context(), index, extension);
        }
    }
    if (auto* existing = module().getFunction(symbol)) {
        bool compatible = existing->getAttributes().getRetAttrs() == attributes.getRetAttrs();
        for (unsigned index = 0; index < parameters.size(); ++index) {
            compatible &= existing->getAttributes().getParamAttrs(index) == attributes.getParamAttrs(index);
        }
        if (existing->getFunctionType() != signature ||
                existing->getCallingConv() != llvm::CallingConv::C || !compatible) {
            const auto binding = std::find_if(source.functions.begin(), source.functions.end(),
                    [name](const auto& function) {
                        return function.nativeBinding.has_value() && function.nativeBinding->symbol == name;
                    });
            const auto span = binding != source.functions.end() ? binding->nativeBinding->span
                    : currentFunction != nullptr && currentFunction->debugLocation.has_value()
                            ? currentFunction->debugLocation->span : SourceSpan {};
            const auto message = "conflicting C signatures for native symbol '" + std::string(name) + "'";
            report(DiagnosticId::conflictingNativeSymbol, span, message);
            throw std::runtime_error(message);
        }
        return existing;
    }
    auto* entry = llvm::Function::Create(
            signature, llvm::GlobalValue::ExternalLinkage, symbol, module());
    entry->setCallingConv(llvm::CallingConv::C);
    entry->setAttributes(attributes);
    return entry;
}

llvm::CallInst* Builder::call(std::string_view name, llvm::Type* result,
        llvm::ArrayRef<llvm::Value*> arguments) {
    std::vector<llvm::Type*> parameters;
    for (const auto* argument : arguments) parameters.push_back(argument->getType());
    auto* function = declareCFunction(name, result, parameters);
    auto* invocation = builder.CreateCall(function, arguments,
            result->isVoidTy() ? "" : temporary());
    invocation->setAttributes(function->getAttributes());
    return invocation;
}

llvm::Value* Builder::stack(llvm::Type* type, const llvm::Twine& name) {
    // All allocas execute once, even if the Joyeer entry block has back edges.
    // Stores and debug declarations remain at their original execution sites.
    llvm::IRBuilder<llvm::NoFolder> allocationBuilder(prologue, prologue->begin());
    auto* allocation = allocationBuilder.CreateAlloca(type, nullptr,
            name.isTriviallyEmpty() ? llvm::Twine(temporary()) : name);
    allocation->setDebugLoc(builder.getCurrentDebugLocation());
    return allocation;
}

llvm::Value* Builder::spill(llvm::Value* value) {
    auto* address = stack(value->getType());
    builder.CreateStore(value, address);
    return address;
}

llvm::Value* Builder::field(llvm::Type* type, llvm::Value* address, unsigned index) {
    return builder.CreateInBoundsGEP(type, address,
            {integer(0, 32), integer(index, 32)}, temporary());
}

llvm::Value* Builder::bytes(llvm::Value* address, llvm::Value* offset) {
    return builder.CreateInBoundsGEP(builder.getInt8Ty(), address, offset, temporary());
}

llvm::Value* Builder::payload(llvm::Type* type, llvm::Value* address, size_t offset) {
    auto* base = builder.CreateInBoundsGEP(type, address,
            {integer(0, 32), integer(1, 32), integer(0, 32)}, temporary());
    return offset == 0 ? base : bytes(base, integer(offset));
}

llvm::Value* Builder::stringLiteral(std::string_view text, bool terminate) {
    if (terminate) {
        const auto found = panicMessages.find(std::string(text));
        if (found != panicMessages.end()) return found->second;
    }
    auto* data = llvm::ConstantDataArray::getString(context(),
            llvm::StringRef(text.data(), text.size()), terminate);
    auto* global = new llvm::GlobalVariable(module(), data->getType(), true,
            llvm::GlobalValue::PrivateLinkage, data,
            std::string(terminate ? ".joyeer.panic." : ".joyeer.string.") + std::to_string(nextString++));
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(1));
    if (terminate) {
        panicMessages.emplace(std::string(text), global);
        return global;
    }
    auto* stringType = llvm::StructType::getTypeByName(context(), "joyeer.string");
    if (stringType == nullptr) {
        stringType = llvm::StructType::create(context(), {pointer(), builder.getInt64Ty()}, "joyeer.string");
    }
    return llvm::ConstantStruct::get(stringType, {global, integer(text.size())});
}

} // namespace joyeer::llvmbackend::native