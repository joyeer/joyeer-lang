#include "native_ir.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

namespace {
size_t alignTo(size_t size, size_t alignment) {
    return (size + alignment - 1) / alignment * alignment;
}
}

Builder::Builder(ModuleStorage& storage, const ir::Module& source, EmitOptions options)
    : storage(storage), source(source), options(options), builder(storage.context) {
    storage.module = std::make_unique<llvm::Module>(source.sourceName, context());
    module().setSourceFileName(source.sourceName);
    for (const auto& value : source.types) types.emplace(value.id, &value);
    for (const auto& value : source.structures) structures.emplace(value.type, &value);
    for (const auto& value : source.enumerations) enumerations.emplace(value.type, &value);
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
    switch (found->second->kind) {
        case typing::TypeKind::voidType: result = llvm::StructType::get(context()); break;
        case typing::TypeKind::never: result = builder.getVoidTy(); break;
        case typing::TypeKind::integer: result = builder.getInt64Ty(); break;
        case typing::TypeKind::boolean: result = builder.getInt1Ty(); break;
        case typing::TypeKind::uint8: result = builder.getInt8Ty(); break;
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
        default: throw std::runtime_error("unsupported inline layout");
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
    for (const auto& parameter : function.parameters) {
        parameters.push_back(llvmType(parameter.value.type));
    }
    auto* result = unit(function.resultType) ? builder.getVoidTy() : llvmType(function.resultType);
    auto* signature = llvm::FunctionType::get(result, parameters, false);
    const auto& symbol = function.nativeBinding->symbol;
    if (auto* existing = module().getFunction(symbol)) {
        if (existing->getFunctionType() != signature) {
            throw std::runtime_error("conflicting native symbol '" + symbol + "'");
        }
        return existing;
    }
    auto* entry = llvm::Function::Create(
            signature, llvm::GlobalValue::ExternalLinkage, symbol, module());
    entry->setCallingConv(llvm::CallingConv::C);
    return entry;
}

llvm::FunctionCallee Builder::runtime(std::string_view name, llvm::Type* result,
        llvm::ArrayRef<llvm::Type*> parameters) {
    return module().getOrInsertFunction(llvm::StringRef(name.data(), name.size()),
            llvm::FunctionType::get(result, parameters, false));
}

llvm::CallInst* Builder::call(std::string_view name, llvm::Type* result,
        llvm::ArrayRef<llvm::Value*> arguments) {
    std::vector<llvm::Type*> parameters;
    for (const auto* argument : arguments) parameters.push_back(argument->getType());
    return builder.CreateCall(runtime(name, result, parameters), arguments,
            result->isVoidTy() ? "" : temporary());
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