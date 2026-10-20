#include "native_ir.h"

#include "llvm/IR/Intrinsics.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace joyeer::llvmbackend::native {

namespace {
std::vector<const ir::BasicBlock*> emissionOrder(const ir::Function& function) {
    std::unordered_map<ir::BlockId, const ir::BasicBlock*> blocks;
    std::vector<ir::BlockId> roots {function.entry};
    for (const auto& block : function.blocks) {
        blocks.emplace(block.id, &block);
        if (block.id != function.entry) roots.push_back(block.id);
    }
    std::unordered_set<ir::BlockId> visited;
    std::vector<const ir::BasicBlock*> result;
    for (const auto root : roots) {
        if (visited.contains(root)) continue;
        std::vector<std::pair<ir::BlockId, bool>> pending {{root, false}};
        std::vector<const ir::BasicBlock*> postorder;
        while (!pending.empty()) {
            const auto [id, complete] = pending.back();
            pending.pop_back();
            const auto* block = blocks.at(id);
            if (complete) {
                postorder.push_back(block);
                continue;
            }
            if (!visited.insert(id).second) continue;
            pending.emplace_back(id, true);
            auto successors = block->instructions.back().targets;
            for (const auto& arm : block->instructions.back().switchCases) successors.push_back(arm.target);
            for (auto target = successors.rbegin(); target != successors.rend(); ++target) {
                if (!visited.contains(*target)) pending.emplace_back(*target, false);
            }
        }
        result.insert(result.end(), postorder.rbegin(), postorder.rend());
    }
    return result;
}
}

void Builder::emitFunction(const ir::Function& function) {
    currentFunction = &function;
    currentBlock = nullptr;
    activeFunction = functions.at(function.id);
    values.clear();
    operands.clear();
    blocks.clear();
    builder.SetCurrentDebugLocation(nullptr);
    functionDebug(function);
    prologue = llvm::BasicBlock::Create(context(), "entry", activeFunction);
    for (size_t index = 0; index < function.parameters.size(); ++index) {
        const auto& parameter = function.parameters[index];
        auto* argument = activeFunction->getArg(static_cast<unsigned>(index));
        argument->setName("v" + std::to_string(parameter.value.id));
        values.emplace(parameter.value.id, parameter.value);
        operands.emplace(parameter.value.id, argument);
    }
    for (const auto& block : function.blocks) {
        blocks.emplace(block.id, llvm::BasicBlock::Create(context(), "b" + std::to_string(block.id), activeFunction));
        for (const auto& instruction : block.instructions) {
            if (instruction.result.has_value()) values.emplace(instruction.result->id, *instruction.result);
        }
    }
    // Reverse postorder establishes dominating SSA definitions before uses,
    // independently of storage/print order. The verifier checks dominance;
    // iterative traversal also handles loops without recursive host-stack use.
    for (const auto* sourceBlock : emissionOrder(function)) {
        const auto& block = *sourceBlock;
        currentBlock = &block;
        builder.SetInsertPoint(blocks.at(block.id));
        if (block.id == function.entry) {
            for (const auto& binding : function.entryDebugVariableBindings) {
                if (!bindDebug(binding)) {
                    report(DiagnosticId::unsupportedInstruction, {}, "LLVM debug parameter binding failed");
                    return;
                }
            }
        }
        for (const auto& instruction : block.instructions) {
            builder.SetCurrentDebugLocation(location(instruction.debugLocation));
            const auto count = diagnostics.size();
            if (!emitInstruction(instruction)) {
                if (count == diagnostics.size()) {
                    report(DiagnosticId::unsupportedInstruction, instruction.span,
                            std::string("LLVM lowering failed for '") + ir::opcodeName(instruction.opcode) + "'");
                }
                return;
            }
            for (const auto& binding : instruction.debugVariableBindings) {
                if (!bindDebug(binding)) {
                    report(DiagnosticId::unsupportedInstruction, instruction.span, "LLVM debug variable binding failed");
                    return;
                }
            }
        }
    }
    builder.SetInsertPoint(prologue);
    builder.SetCurrentDebugLocation(location(function.debugLocation));
    builder.CreateBr(blocks.at(function.entry));
    currentFunction = nullptr;
    currentBlock = nullptr;
    subprogram = nullptr;
    builder.SetCurrentDebugLocation(nullptr);
}

void Builder::emitNativeExport(const ir::Function& function) {
    auto* entry = declareNativeFunction(function);
    builder.SetCurrentDebugLocation(nullptr);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context(), "entry", entry));
    std::vector<llvm::Value*> arguments;
    for (auto& argument : entry->args()) arguments.push_back(&argument);
    // The exported C entry is separate from Joyeer's internal calling
    // convention, even when the first scalar slice needs no conversions.
    auto* result = builder.CreateCall(functions.at(function.id), arguments);
    if (function.returnsValue) builder.CreateRet(result);
    else builder.CreateRetVoid();
}

bool Builder::emitInstruction(const ir::Instruction& instruction) {
    const auto argument = [&](size_t index) { return operand(instruction.operands.at(index)); };
    llvm::Value* result = nullptr;
    const auto resultType = [&]() { return llvmType(instruction.result->type); };
    using Opcode = ir::Opcode;
    switch (instruction.opcode) {
        case Opcode::unitConstant: result = llvm::Constant::getNullValue(resultType()); break;
        case Opcode::integerConstant:
        case Opcode::byteConstant:
        case Opcode::booleanConstant:
            result = llvm::ConstantInt::get(resultType(), static_cast<uint64_t>(instruction.integerValue)); break;
        case Opcode::floatingConstant:
            result = llvm::ConstantFP::get(resultType(), instruction.text); break;
        case Opcode::stringConstant: result = stringLiteral(instruction.text); break;
        case Opcode::stackAllocate:
            result = stack(resultType(), "v" + std::to_string(instruction.result->id)); break;
        case Opcode::zeroInitialize: {
            const auto id = values.at(instruction.operands[0]).type;
            if (!unit(id)) builder.CreateStore(llvm::Constant::getNullValue(llvmType(id)), argument(0));
            break;
        }
        case Opcode::load:
            result = unit(instruction.result->type) ? static_cast<llvm::Value*>(llvm::Constant::getNullValue(resultType()))
                    : builder.CreateLoad(resultType(), argument(0)); break;
        case Opcode::store:
            if (!unit(values.at(instruction.operands[0]).type)) builder.CreateStore(argument(0), argument(1));
            break;
        case Opcode::copyValue: {
            const auto id = instruction.result->type;
            result = argument(0);
            if (destruction->requiresDestruction(id)) {
                auto* destination = stack(resultType());
                copy(id, destination, spill(result));
                result = builder.CreateLoad(resultType(), destination);
            }
            break;
        }
        case Opcode::take:
            result = llvm::Constant::getNullValue(resultType());
            if (!unit(instruction.result->type)) {
                result = builder.CreateLoad(resultType(), argument(0));
                builder.CreateStore(llvm::Constant::getNullValue(resultType()), argument(0));
            }
            break;
        case Opcode::destroy:
            builder.CreateCall(destroys.at(values.at(instruction.operands[0]).type), {argument(0)}); break;
        case Opcode::add:
        case Opcode::subtract:
        case Opcode::multiply:
        case Opcode::divide:
        case Opcode::remainder: result = arithmetic(instruction); break;
        case Opcode::less:
        case Opcode::lessEqual:
        case Opcode::greater:
        case Opcode::greaterEqual:
        case Opcode::equal:
        case Opcode::notEqual:
            result = comparison(instruction.opcode, argument(0), argument(1),
                    type(values.at(instruction.operands[0]).type).kind);
            break;
        case Opcode::logicalAnd: result = builder.CreateAnd(argument(0), argument(1)); break;
        case Opcode::call: result = emitCall(instruction); break;
        case Opcode::constructStruct:
            result = llvm::PoisonValue::get(resultType());
            if (instruction.operands.empty()) result = llvm::Constant::getNullValue(resultType());
            for (size_t index = 0; index < instruction.operands.size(); ++index) {
                result = builder.CreateInsertValue(result, argument(index), static_cast<unsigned>(index), temporary());
            }
            break;
        case Opcode::fieldAddress:
        case Opcode::extractField: {
            const auto id = values.at(instruction.operands[0]).type;
            const auto& fields = structures.at(id)->fields;
            const auto found = std::find_if(fields.begin(), fields.end(), [&instruction](const auto& field) {
                return field.symbol == instruction.symbol;
            });
            if (found == fields.end()) return false;
            const auto index = static_cast<unsigned>(std::distance(fields.begin(), found));
            result = instruction.opcode == Opcode::fieldAddress ? field(llvmType(id), argument(0), index)
                    : builder.CreateExtractValue(argument(0), index);
            break;
        }
        case Opcode::constructEnum: {
            const auto id = instruction.result->type;
            const auto& cases = enumerations.at(id)->cases;
            const auto found = std::find_if(cases.begin(), cases.end(), [&instruction](const auto& definition) {
                return definition.symbol == instruction.symbol;
            });
            if (found == cases.end()) return false;
            std::vector<llvm::Value*> payloads;
            for (const auto id : instruction.operands) payloads.push_back(operand(id));
            result = constructEnum(id, static_cast<size_t>(std::distance(cases.begin(), found)), payloads);
            break;
        }
        case Opcode::extractPayload:
            result = extractPayload(values.at(instruction.operands[0]).type, argument(0),
                    *instruction.symbol, static_cast<size_t>(instruction.integerValue)); break;
        case Opcode::branch: builder.CreateBr(blocks.at(instruction.targets[0])); break;
        case Opcode::conditionalBranch:
            builder.CreateCondBr(argument(0), blocks.at(instruction.targets[0]), blocks.at(instruction.targets[1])); break;
        case Opcode::switchPattern: {
            for (const auto& arm : instruction.switchCases) {
                if (arm.pattern.kind == ir::PatternKind::wildcard) {
                    builder.CreateBr(blocks.at(arm.target));
                    return true;
                }
                auto* condition = pattern(arm.pattern, argument(0));
                auto* next = llvm::BasicBlock::Create(context(), "pattern." + temporary(), activeFunction);
                builder.CreateCondBr(condition, blocks.at(arm.target), next);
                builder.SetInsertPoint(next);
            }
            builder.CreateUnreachable();
            break;
        }
        case Opcode::returnValue: builder.CreateRet(argument(0)); break;
        case Opcode::returnVoid: builder.CreateRetVoid(); break;
        case Opcode::unreachable: builder.CreateUnreachable(); break;
        default:
            if (!emitCollection(instruction, result)) return false;
            break;
    }
    if (instruction.result.has_value()) {
        if (result == nullptr || result->getType()->isVoidTy()) return false;
        if (llvm::isa<llvm::Instruction>(result) && !result->hasName()) {
            result->setName("v" + std::to_string(instruction.result->id));
        }
        operands[instruction.result->id] = result;
    }
    return true;
}

void Builder::trap(llvm::Value* condition, std::string_view message) {
    auto* failure = llvm::BasicBlock::Create(context(), "trap." + temporary(), activeFunction);
    auto* continuation = llvm::BasicBlock::Create(context(), "checked." + temporary(), activeFunction);
    builder.CreateCondBr(condition, failure, continuation);
    builder.SetInsertPoint(failure);
    auto* panic = call("joyeer_panic", builder.getVoidTy(), {stringLiteral(message, true)});
    if (auto* function = panic->getCalledFunction()) {
        function->addFnAttr(llvm::Attribute::Cold);
        function->addFnAttr(llvm::Attribute::NoReturn);
    }
    builder.CreateUnreachable();
    builder.SetInsertPoint(continuation);
}

llvm::Value* Builder::arithmetic(const ir::Instruction& instruction) {
    auto* left = operand(instruction.operands[0]);
    auto* right = operand(instruction.operands[1]);
    const auto kind = type(values.at(instruction.operands[0]).type).kind;
    if (kind == typing::TypeKind::string && instruction.opcode == ir::Opcode::add) {
        auto* address = stack(left->getType());
        call("joyeer_string_concat_abi", builder.getVoidTy(), {address,
                builder.CreateExtractValue(left, 0, temporary()), builder.CreateExtractValue(left, 1, temporary()),
                builder.CreateExtractValue(right, 0, temporary()), builder.CreateExtractValue(right, 1, temporary())});
        return builder.CreateLoad(left->getType(), address);
    }
    if (typing::isFloating(kind)) {
        switch (instruction.opcode) {
            case ir::Opcode::add: return builder.CreateFAdd(left, right);
            case ir::Opcode::subtract: return builder.CreateFSub(left, right);
            case ir::Opcode::multiply: return builder.CreateFMul(left, right);
            case ir::Opcode::divide: return builder.CreateFDiv(left, right);
            default: return nullptr;
        }
    }
    if (!typing::isInteger(kind)) {
        report(DiagnosticId::unsupportedInstruction, instruction.span, "unsupported arithmetic operand type");
        return nullptr;
    }
    const auto bits = typing::scalarBitWidth(kind);
    const bool signedValue = typing::isSignedInteger(kind);
    if (instruction.opcode == ir::Opcode::divide || instruction.opcode == ir::Opcode::remainder) {
        const bool divide = instruction.opcode == ir::Opcode::divide;
        const auto operation = std::string(divide ? "division" : "remainder");
        trap(builder.CreateICmpEQ(right, integer(0, bits), temporary()), "integer " + operation + " by zero");
        if (!signedValue) return divide ? builder.CreateUDiv(left, right) : builder.CreateURem(left, right);
        auto* minimum = builder.CreateICmpEQ(left, integer(uint64_t{1} << (bits - 1), bits), temporary());
        auto* negativeOne = builder.CreateICmpEQ(right, integer(UINT64_MAX, bits), temporary());
        trap(builder.CreateAnd(minimum, negativeOne, temporary()), "integer " + operation + " overflow");
        return divide ? builder.CreateSDiv(left, right) : builder.CreateSRem(left, right);
    }
    if (!signedValue) {
        switch (instruction.opcode) {
            case ir::Opcode::add: return builder.CreateAdd(left, right);
            case ir::Opcode::subtract: return builder.CreateSub(left, right);
            case ir::Opcode::multiply: return builder.CreateMul(left, right);
            default: return nullptr;
        }
    }
    llvm::Intrinsic::ID intrinsic = llvm::Intrinsic::not_intrinsic;
    std::string message;
    switch (instruction.opcode) {
        case ir::Opcode::add: intrinsic = llvm::Intrinsic::sadd_with_overflow; message = "addition"; break;
        case ir::Opcode::subtract: intrinsic = llvm::Intrinsic::ssub_with_overflow; message = "subtraction"; break;
        case ir::Opcode::multiply: intrinsic = llvm::Intrinsic::smul_with_overflow; message = "multiplication"; break;
        default: return nullptr;
    }
    auto* checked = builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&module(), intrinsic, {left->getType()}),
            {left, right}, temporary());
    trap(builder.CreateExtractValue(checked, 1, temporary()), "integer " + message + " overflow");
    return builder.CreateExtractValue(checked, 0);
}

llvm::Value* Builder::comparison(ir::Opcode opcode, llvm::Value* left, llvm::Value* right,
        typing::TypeKind kind) {
    using Opcode = ir::Opcode;
    if (left->getType()->isFloatingPointTy()) {
        llvm::CmpInst::Predicate predicate = llvm::CmpInst::FCMP_OEQ;
        switch (opcode) {
            case Opcode::less: predicate = llvm::CmpInst::FCMP_OLT; break;
            case Opcode::lessEqual: predicate = llvm::CmpInst::FCMP_OLE; break;
            case Opcode::greater: predicate = llvm::CmpInst::FCMP_OGT; break;
            case Opcode::greaterEqual: predicate = llvm::CmpInst::FCMP_OGE; break;
            case Opcode::notEqual: predicate = llvm::CmpInst::FCMP_UNE; break;
            default: break;
        }
        return builder.CreateFCmp(predicate, left, right);
    }
    if (auto* aggregate = llvm::dyn_cast<llvm::StructType>(left->getType());
            aggregate != nullptr && aggregate->getName() == "joyeer.string") {
        std::vector<llvm::Value*> arguments {builder.CreateExtractValue(left, 0, temporary()),
                builder.CreateExtractValue(left, 1, temporary()), builder.CreateExtractValue(right, 0, temporary()),
                builder.CreateExtractValue(right, 1, temporary())};
        if (opcode == Opcode::equal || opcode == Opcode::notEqual) {
            auto* equal = call("joyeer_string_equal_abi", builder.getInt1Ty(), arguments);
            return opcode == Opcode::equal ? equal : builder.CreateNot(equal);
        }
        left = call("joyeer_string_compare_abi", builder.getInt64Ty(), arguments);
        right = integer(0);
        kind = typing::TypeKind::integer;
    }
    const bool signedValue = typing::isSignedInteger(kind);
    llvm::CmpInst::Predicate predicate = llvm::CmpInst::ICMP_EQ;
    switch (opcode) {
        case Opcode::less: predicate = signedValue ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT; break;
        case Opcode::lessEqual: predicate = signedValue ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE; break;
        case Opcode::greater: predicate = signedValue ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT; break;
        case Opcode::greaterEqual: predicate = signedValue ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE; break;
        case Opcode::notEqual: predicate = llvm::CmpInst::ICMP_NE; break;
        default: break;
    }
    return builder.CreateICmp(predicate, left, right);
}

llvm::Value* Builder::constructEnum(ir::TypeId id, size_t tag, llvm::ArrayRef<llvm::Value*> payloads) {
    auto* address = stack(llvmType(id));
    builder.CreateStore(llvm::Constant::getNullValue(llvmType(id)), address);
    builder.CreateStore(integer(tag, 32), field(llvmType(id), address, 0));
    const auto& definition = enumerations.at(id)->cases.at(tag);
    const auto offsets = payloadOffsets(definition);
    for (size_t index = 0; index < payloads.size(); ++index) {
        if (!unit(definition.payloadTypes.at(index))) {
            builder.CreateStore(payloads[index], payload(llvmType(id), address, offsets.at(index)));
        }
    }
    return builder.CreateLoad(llvmType(id), address);
}

llvm::Value* Builder::extractPayload(ir::TypeId id, llvm::Value* value,
        semantic::SymbolId symbol, size_t index) {
    const auto& cases = enumerations.at(id)->cases;
    const auto found = std::find_if(cases.begin(), cases.end(), [symbol](const auto& definition) {
        return definition.symbol == symbol;
    });
    if (found == cases.end()) throw std::runtime_error("invalid enum payload case");
    const auto payloadType = found->payloadTypes.at(index);
    if (unit(payloadType)) return llvm::Constant::getNullValue(llvmType(payloadType));
    return builder.CreateLoad(llvmType(payloadType),
            payload(llvmType(id), spill(value), payloadOffsets(*found).at(index)), temporary());
}

llvm::Value* Builder::pattern(const ir::Pattern& tested, llvm::Value* value) {
    if (tested.kind == ir::PatternKind::wildcard) return integer(1, 1);
    if (tested.kind == ir::PatternKind::stringLiteral) {
        return comparison(ir::Opcode::equal, value, stringLiteral(tested.text));
    }
    if (tested.kind == ir::PatternKind::floatingLiteral) {
        return builder.CreateFCmpOEQ(value, llvm::ConstantFP::get(value->getType(), tested.text));
    }
    if (tested.kind != ir::PatternKind::enumCase) {
        return builder.CreateICmpEQ(value, llvm::ConstantInt::get(value->getType(),
                static_cast<uint64_t>(tested.integerValue)), temporary());
    }
    const auto& cases = enumerations.at(tested.type)->cases;
    const auto found = std::find_if(cases.begin(), cases.end(), [&tested](const auto& definition) {
        return definition.symbol == tested.symbol;
    });
    if (found == cases.end()) throw std::runtime_error("invalid enum pattern case");
    auto* tagMatches = builder.CreateICmpEQ(builder.CreateExtractValue(value, 0, temporary()),
            integer(static_cast<uint64_t>(std::distance(cases.begin(), found)), 32), temporary());
    std::vector<size_t> indices;
    for (size_t index = 0; index < tested.payloads.size(); ++index) {
        if (tested.payloads[index].kind != ir::PatternKind::wildcard) indices.push_back(index);
    }
    if (indices.empty()) return tagMatches;

    // Short-circuit before touching payloads: inactive storage is not a value.
    // The PHI joins only boolean matches; Joyeer values remain explicit SSA.
    auto* done = llvm::BasicBlock::Create(context(), "pattern.done." + temporary(), activeFunction);
    auto* testPayload = llvm::BasicBlock::Create(context(), "pattern.payload." + temporary(), activeFunction);
    auto* tagBlock = builder.GetInsertBlock();
    builder.CreateCondBr(tagMatches, testPayload, done);
    std::vector<llvm::BasicBlock*> failed {tagBlock};
    builder.SetInsertPoint(testPayload);
    llvm::Value* matches = nullptr;
    for (size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
        const auto index = indices[ordinal];
        matches = pattern(tested.payloads[index], extractPayload(tested.type, value, *tested.symbol, index));
        if (ordinal + 1 < indices.size()) {
            auto* next = llvm::BasicBlock::Create(context(), "pattern.payload." + temporary(), activeFunction);
            failed.push_back(builder.GetInsertBlock());
            builder.CreateCondBr(matches, next, done);
            builder.SetInsertPoint(next);
        }
    }
    auto* finalBlock = builder.GetInsertBlock();
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    auto* phi = builder.CreatePHI(builder.getInt1Ty(), static_cast<unsigned>(failed.size() + 1), temporary());
    for (auto* block : failed) phi->addIncoming(integer(0, 1), block);
    phi->addIncoming(matches, finalBlock);
    return phi;
}

void Builder::emitEntry() {
    const ir::Function* entry = nullptr;
    for (const auto& function : source.functions) {
        if (function.isExternal || !function.isRootModule || function.name != "main") continue;
        if (entry != nullptr) {
            diagnostics.push_back({DiagnosticId::invalidEntryPoint,
                    function.debugLocation.has_value() ? function.debugLocation->span : SourceSpan{},
                    function.id, std::nullopt, "root module contains multiple 'main' entry functions"});
            return;
        }
        entry = &function;
    }
    if (entry == nullptr) return;
    const bool legacy = entry->parameters.empty() && unit(entry->resultType);
    bool arguments = false;
    if (entry->parameters.size() == 1 && type(entry->resultType).kind == typing::TypeKind::integer) {
        const auto& parameter = entry->parameters.front();
        const auto& parameterType = type(parameter.value.type);
        arguments = parameter.label == "args" && !parameter.isMutable && !parameter.isConsuming &&
                parameter.value.category == ir::ValueCategory::value &&
                parameterType.kind == typing::TypeKind::array && parameterType.arguments.size() == 1 &&
                type(parameterType.arguments[0]).kind == typing::TypeKind::string;
    }
    if (!legacy && !arguments) {
        diagnostics.push_back({DiagnosticId::invalidEntryPoint,
                entry->debugLocation.has_value() ? entry->debugLocation->span : SourceSpan{}, entry->id,
                std::nullopt, "entry function must have signature 'func main()' or 'func main(args: [String]): Int'"});
        return;
    }
    for (const auto* symbol : {"joyeer_main", "joyeer_main_uses_arguments"}) {
        if (module().getNamedValue(symbol) != nullptr) {
            diagnostics.push_back({DiagnosticId::invalidEntryPoint,
                    entry->debugLocation.has_value() ? entry->debugLocation->span : SourceSpan{},
                    entry->id, std::nullopt,
                    "native binding conflicts with toolchain entry symbol '" + std::string(symbol) + "'"});
            return;
        }
    }
    // The native C runtime calls these stable adapters, not the source main's
    // language-specific signature. Ownership of argv remains with the runtime.
    auto* usesArguments = llvm::Function::Create(llvm::FunctionType::get(builder.getInt32Ty(), false),
            llvm::GlobalValue::ExternalLinkage, "joyeer_main_uses_arguments", module());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context(), "entry", usesArguments));
    builder.CreateRet(integer(arguments ? 1 : 0, 32));
    auto* wrapper = llvm::Function::Create(llvm::FunctionType::get(builder.getInt64Ty(), {pointer()}, false),
            llvm::GlobalValue::ExternalLinkage, "joyeer_main", module());
    wrapper->getArg(0)->setName("args");
    builder.SetInsertPoint(llvm::BasicBlock::Create(context(), "entry", wrapper));
    if (arguments) {
        auto* value = builder.CreateLoad(llvmType(entry->parameters[0].value.type), wrapper->getArg(0), "arguments");
        builder.CreateRet(builder.CreateCall(functions.at(entry->id), {value}, "status"));
    } else {
        builder.CreateCall(functions.at(entry->id));
        builder.CreateRet(integer(0));
    }
    hasEntryPoint = true;
}

} // namespace joyeer::llvmbackend::native