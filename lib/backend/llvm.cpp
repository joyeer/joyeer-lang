#include "joyeer/backend/llvm.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace joyeer::llvmbackend {

namespace {

std::string escapeQuoted(std::string_view text) {
    std::ostringstream out;
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 0x20 && byte <= 0x7e && character != '"' && character != '\\') {
            out << character;
        } else {
            out << '\\' << std::uppercase << std::hex << std::setw(2)
                << std::setfill('0') << static_cast<unsigned int>(byte) << std::dec;
        }
    }
    return out.str();
}

std::string valueName(ir::ValueId value) {
    return "%v" + std::to_string(value);
}

std::string blockName(ir::BlockId block) {
    return "b" + std::to_string(block);
}

std::string functionName(const ir::Function& function) {
    return "@joyeer_fn_" + std::to_string(function.id);
}

size_t alignTo(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

struct Layout {
    size_t size = 0;
    size_t alignment = 1;
};

class Builder {
public:
    Result build(const ir::Module& source) {
        module = &source;
        const auto verification = ir::Verifier().verify(source);
        if (!verification.succeeded()) {
            for (const auto& error : verification.errors) {
                report(
                        DiagnosticId::invalidModule,
                        {},
                        error.function,
                        error.block,
                        std::string(ir::verificationErrorName(error.id)) +
                                ": " + error.message);
            }
            return Result { {}, std::move(diagnostics) };
        }

        for (const auto& type : source.types) types.emplace(type.id, &type);
        for (const auto& structure : source.structures) {
            structures.emplace(structure.type, &structure);
            for (size_t index = 0; index < structure.fields.size(); ++index) {
                fields.emplace(
                        structure.fields[index].symbol,
                        std::make_pair(&structure, index));
            }
        }
        for (const auto& enumeration : source.enumerations) {
            enumerations.emplace(enumeration.type, &enumeration);
            for (size_t index = 0; index < enumeration.cases.size(); ++index) {
                cases.emplace(
                        enumeration.cases[index].symbol,
                        std::make_pair(&enumeration, index));
            }
        }
        for (const auto& function : source.functions) {
            functions.emplace(function.id, &function);
        }
        emitAggregateTypeDefinitions();
        emitOwnershipHelpers();
        for (const auto& function : source.functions) {
            if (!function.isExternal) emitFunction(function);
        }
        emitEntryPoint();
        if (!diagnostics.empty()) return Result { {}, std::move(diagnostics) };

        std::ostringstream out;
        out << "; Joyeer LLVM backend\n"
            << "source_filename = \"" << escapeQuoted(source.sourceName) << "\"\n";
        if (usesString) out << "%joyeer.string = type { ptr, i64 }\n";
        if (usesArray) out << "%joyeer.array = type { ptr, i64, i64 }\n";
        if (usesDictionary) out << "%joyeer.dictionary = type { ptr, i64, i64 }\n";
        for (const auto& definition : typeDefinitions) out << definition << '\n';
        if (usesString || usesArray || usesDictionary || !typeDefinitions.empty() ||
            !stringGlobals.empty() || !runtimeDeclarations.empty()) {
            out << '\n';
        }
        for (const auto& global : stringGlobals) out << global << '\n';
        if (!stringGlobals.empty() && !runtimeDeclarations.empty()) out << '\n';

        std::vector<std::string> declarations(
                runtimeDeclarations.begin(),
                runtimeDeclarations.end());
        std::sort(declarations.begin(), declarations.end());
        for (const auto& declaration : declarations) out << declaration << '\n';
        if (!declarations.empty() && !functionBodies.empty()) out << '\n';
        for (size_t index = 0; index < functionBodies.size(); ++index) {
            out << functionBodies[index];
            if (index + 1 < functionBodies.size()) out << '\n';
        }
        return Result { out.str(), {}, hasEntryPoint };
    }

private:
    const ir::Module* module = nullptr;
    const ir::Function* currentFunction = nullptr;
    const ir::BasicBlock* currentBlock = nullptr;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<ir::TypeId, const ir::TypeName*> types;
    std::unordered_map<ir::FunctionId, const ir::Function*> functions;
    std::unordered_map<ir::TypeId, const ir::StructureDefinition*> structures;
    std::unordered_map<ir::TypeId, const ir::EnumerationDefinition*> enumerations;
    std::unordered_map<semantic::SymbolId,
                       std::pair<const ir::StructureDefinition*, size_t>> fields;
    std::unordered_map<semantic::SymbolId,
                       std::pair<const ir::EnumerationDefinition*, size_t>> cases;
    std::unordered_map<ir::TypeId, Layout> layouts;
    std::unordered_set<ir::TypeId> layoutsInProgress;
    std::unordered_map<ir::ValueId, ir::Value> values;
    std::unordered_map<ir::ValueId, std::string> operands;
    std::unordered_set<std::string> runtimeDeclarations;
    std::vector<std::string> stringGlobals;
    std::vector<std::string> typeDefinitions;
    std::vector<std::string> functionBodies;
    size_t nextString = 0;
    size_t nextTemporary = 0;
    bool usesString = false;
    bool usesArray = false;
    bool usesDictionary = false;
    bool hasEntryPoint = false;

    void report(
            DiagnosticId id,
            SourceSpan span,
            std::optional<ir::FunctionId> function,
            std::optional<ir::BlockId> block,
            std::string message) {
        diagnostics.push_back(Diagnostic {
            id,
            span,
            function,
            block,
            std::move(message),
        });
    }

    void reportHere(DiagnosticId id, SourceSpan span, std::string message) {
        report(
                id,
                span,
                currentFunction == nullptr
                        ? std::optional<ir::FunctionId>()
                        : std::optional<ir::FunctionId>(currentFunction->id),
                currentBlock == nullptr
                        ? std::optional<ir::BlockId>()
                        : std::optional<ir::BlockId>(currentBlock->id),
                std::move(message));
    }

    std::optional<Layout> layoutFor(ir::TypeId id) {
        const auto cached = layouts.find(id);
        if (cached != layouts.end()) return cached->second;
        const auto* value = type(id);
        if (value == nullptr) return std::nullopt;
        if (!layoutsInProgress.insert(id).second) {
            reportHere(
                    DiagnosticId::unsupportedType,
                    {},
                    "recursive inline layout for type '" + value->name + "'");
            return std::nullopt;
        }

        std::optional<Layout> result;
        switch (value->kind) {
            case typing::TypeKind::voidType:
            case typing::TypeKind::never:
                result = Layout { 0, 1 };
                break;
            case typing::TypeKind::boolean:
            case typing::TypeKind::uint8:
                result = Layout { 1, 1 };
                break;
            case typing::TypeKind::integer:
                result = Layout { 8, 8 };
                break;
            case typing::TypeKind::string:
                usesString = true;
                result = Layout { 16, 8 };
                break;
            case typing::TypeKind::array:
                usesArray = true;
                result = Layout { 24, 8 };
                break;
            case typing::TypeKind::dictionary:
                usesDictionary = true;
                result = Layout { 24, 8 };
                break;
            case typing::TypeKind::structure: {
                const auto found = structures.find(id);
                if (found == structures.end()) break;
                size_t size = 0;
                size_t alignment = 1;
                for (const auto& field : found->second->fields) {
                    const auto fieldLayout = layoutFor(field.type);
                    if (!fieldLayout.has_value()) {
                        result.reset();
                        break;
                    }
                    size = alignTo(size, fieldLayout->alignment) + fieldLayout->size;
                    alignment = std::max(alignment, fieldLayout->alignment);
                    result = Layout { alignTo(size, alignment), alignment };
                }
                if (found->second->fields.empty()) result = Layout { 0, 1 };
                break;
            }
            case typing::TypeKind::enumeration:
            case typing::TypeKind::optional:
            case typing::TypeKind::result: {
                const auto found = enumerations.find(id);
                if (found == enumerations.end()) break;
                size_t payloadSize = 0;
                bool valid = true;
                for (const auto& enumCase : found->second->cases) {
                    size_t caseSize = 0;
                    for (const auto payloadType : enumCase.payloadTypes) {
                        const auto payloadLayout = layoutFor(payloadType);
                        if (!payloadLayout.has_value()) {
                            valid = false;
                            break;
                        }
                        caseSize = alignTo(caseSize, payloadLayout->alignment) +
                                payloadLayout->size;
                    }
                    if (!valid) break;
                    payloadSize = std::max(payloadSize, alignTo(caseSize, 8));
                }
                if (valid) result = Layout { 8 + payloadSize, 8 };
                break;
            }
            default:
                break;
        }

        layoutsInProgress.erase(id);
        if (result.has_value()) layouts.emplace(id, *result);
        return result;
    }

    std::optional<std::vector<size_t>> payloadOffsets(
            const ir::EnumCaseDefinition& enumCase) {
        std::vector<size_t> result;
        size_t offset = 0;
        for (const auto payloadType : enumCase.payloadTypes) {
            const auto payloadLayout = layoutFor(payloadType);
            if (!payloadLayout.has_value()) return std::nullopt;
            offset = alignTo(offset, payloadLayout->alignment);
            result.push_back(offset);
            offset += payloadLayout->size;
        }
        return result;
    }

    void emitAggregateTypeDefinitions() {
        std::vector<ir::TypeId> structureTypes;
        for (const auto& [id, structure] : structures) {
            static_cast<void>(structure);
            structureTypes.push_back(id);
        }
        std::sort(structureTypes.begin(), structureTypes.end());
        for (const auto id : structureTypes) {
            const auto* structure = structures.at(id);
            std::ostringstream definition;
            definition << "%joyeer.struct." << id << " = type { ";
            for (size_t index = 0; index < structure->fields.size(); ++index) {
                if (index != 0) definition << ", ";
                const auto fieldType = llvmType(structure->fields[index].type);
                if (!fieldType.has_value()) return;
                definition << *fieldType;
            }
            definition << " }";
            typeDefinitions.push_back(definition.str());
            layoutFor(id);
        }

        std::vector<ir::TypeId> enumerationTypes;
        for (const auto& [id, enumeration] : enumerations) {
            static_cast<void>(enumeration);
            enumerationTypes.push_back(id);
        }
        std::sort(enumerationTypes.begin(), enumerationTypes.end());
        for (const auto id : enumerationTypes) {
            const auto valueLayout = layoutFor(id);
            if (!valueLayout.has_value()) return;
            const auto payloadWords = valueLayout->size <= 8
                    ? 0
                    : (valueLayout->size - 8) / 8;
            typeDefinitions.push_back(
                    "%joyeer.enum." + std::to_string(id) +
                    " = type { i32, [" + std::to_string(payloadWords) +
                    " x i64] }");
        }
    }

    std::string cloneHelper(ir::TypeId type) const {
        return "@joyeer_clone_type_" + std::to_string(type);
    }

    std::string destroyHelper(ir::TypeId type) const {
        return "@joyeer_destroy_type_" + std::to_string(type);
    }

    void emitOwnershipHelpers() {
        std::vector<ir::TypeId> ownedTypes;
        for (const auto& type : module->types) {
            if (ir::requiresDestruction(*module, type.id)) ownedTypes.push_back(type.id);
        }
        std::sort(ownedTypes.begin(), ownedTypes.end());
        for (const auto type : ownedTypes) emitCloneHelper(type);
        for (const auto type : ownedTypes) emitDestroyHelper(type);
    }

    void emitCloneHelper(ir::TypeId id) {
        const auto* valueType = type(id);
        const auto typeText = llvmType(id);
        if (valueType == nullptr || !typeText.has_value()) return;
        std::ostringstream out;
        out << "define void " << cloneHelper(id) << "(ptr %destination, ptr %source) {\n"
            << "entry:\n";
        switch (valueType->kind) {
            case typing::TypeKind::string:
                runtimeDeclarations.insert(
                        "declare void @joyeer_string_clone_abi(ptr, ptr, i64)");
                out << "  %value = load %joyeer.string, ptr %source\n"
                    << "  %data = extractvalue %joyeer.string %value, 0\n"
                    << "  %count = extractvalue %joyeer.string %value, 1\n"
                    << "  call void @joyeer_string_clone_abi(ptr %destination, ptr %data, i64 %count)\n";
                break;
            case typing::TypeKind::array:
                runtimeDeclarations.insert(
                        "declare void @joyeer_array_clone_abi(ptr, ptr, i64)");
                out << "  %value = load %joyeer.array, ptr %source\n"
                    << "  %data = extractvalue %joyeer.array %value, 0\n"
                    << "  %count = extractvalue %joyeer.array %value, 1\n"
                    << "  call void @joyeer_array_clone_abi(ptr %destination, ptr %data, i64 %count)\n";
                break;
            case typing::TypeKind::dictionary:
                runtimeDeclarations.insert(
                        "declare void @joyeer_dictionary_clone_abi(ptr, ptr, i64)");
                out << "  %value = load %joyeer.dictionary, ptr %source\n"
                    << "  %data = extractvalue %joyeer.dictionary %value, 0\n"
                    << "  %count = extractvalue %joyeer.dictionary %value, 1\n"
                    << "  call void @joyeer_dictionary_clone_abi(ptr %destination, ptr %data, i64 %count)\n";
                break;
            case typing::TypeKind::structure:
                emitStructClone(out, id, *typeText);
                break;
            case typing::TypeKind::enumeration:
            case typing::TypeKind::optional:
            case typing::TypeKind::result:
                emitEnumClone(out, id, *typeText);
                break;
            default:
                return;
        }
        out << "  ret void\n}\n";
        functionBodies.push_back(out.str());
    }

    void emitStructClone(std::ostringstream& out, ir::TypeId id, const std::string& typeText) {
        const auto* structure = structures.at(id);
        for (size_t index = 0; index < structure->fields.size(); ++index) {
            const auto& field = structure->fields[index];
            const auto destination = "%destination.field." + std::to_string(index);
            const auto source = "%source.field." + std::to_string(index);
            out << "  " << destination << " = getelementptr inbounds " << typeText
                << ", ptr %destination, i32 0, i32 " << index << "\n"
                << "  " << source << " = getelementptr inbounds " << typeText
                << ", ptr %source, i32 0, i32 " << index << "\n";
            if (ir::requiresDestruction(*module, field.type)) {
                out << "  call void " << cloneHelper(field.type) << "(ptr "
                    << destination << ", ptr " << source << ")\n";
            } else {
                const auto fieldType = llvmType(field.type);
                if (!fieldType.has_value()) return;
                const auto loaded = "%field.value." + std::to_string(index);
                out << "  " << loaded << " = load " << *fieldType << ", ptr "
                    << source << "\n"
                    << "  store " << *fieldType << ' ' << loaded << ", ptr "
                    << destination << "\n";
            }
        }
    }

    std::string emitEnumPayloadAddress(
            std::ostringstream& out,
            const std::string& owner,
            const std::string& prefix,
            const std::string& typeText,
            size_t offset) {
        const auto base = '%' + prefix + ".payload.base";
        out << "  " << base << " = getelementptr inbounds " << typeText << ", ptr "
            << owner << ", i32 0, i32 1, i32 0\n";
        if (offset == 0) return base;
        const auto address = '%' + prefix + ".payload";
        out << "  " << address << " = getelementptr inbounds i8, ptr " << base
            << ", i64 " << offset << "\n";
        return address;
    }

    void emitEnumClone(std::ostringstream& out, ir::TypeId id, const std::string& typeText) {
        const auto* enumeration = enumerations.at(id);
        out << "  %whole = load " << typeText << ", ptr %source\n"
            << "  store " << typeText << " %whole, ptr %destination\n"
            << "  %tag = extractvalue " << typeText << " %whole, 0\n"
            << "  switch i32 %tag, label %exit [";
        for (size_t index = 0; index < enumeration->cases.size(); ++index) {
            out << " i32 " << index << ", label %case." << index;
        }
        out << " ]\n";
        for (size_t caseIndex = 0; caseIndex < enumeration->cases.size(); ++caseIndex) {
            const auto& enumCase = enumeration->cases[caseIndex];
            const auto offsets = payloadOffsets(enumCase);
            if (!offsets.has_value()) return;
            out << "case." << caseIndex << ":\n";
            for (size_t payload = 0; payload < enumCase.payloadTypes.size(); ++payload) {
                const auto payloadType = enumCase.payloadTypes[payload];
                if (!ir::requiresDestruction(*module, payloadType)) continue;
                const auto source = emitEnumPayloadAddress(
                        out,
                        "%source",
                        "source." + std::to_string(caseIndex) + '.' + std::to_string(payload),
                        typeText,
                        (*offsets)[payload]);
                const auto destination = emitEnumPayloadAddress(
                        out,
                        "%destination",
                        "destination." + std::to_string(caseIndex) + '.' + std::to_string(payload),
                        typeText,
                        (*offsets)[payload]);
                out << "  call void " << cloneHelper(payloadType) << "(ptr "
                    << destination << ", ptr " << source << ")\n";
            }
            out << "  br label %exit\n";
        }
        out << "exit:\n";
    }

    void emitDestroyHelper(ir::TypeId id) {
        const auto* valueType = type(id);
        const auto typeText = llvmType(id);
        if (valueType == nullptr || !typeText.has_value()) return;
        std::ostringstream out;
        out << "define void " << destroyHelper(id) << "(ptr %value) {\n"
            << "entry:\n";
        switch (valueType->kind) {
            case typing::TypeKind::string:
                runtimeDeclarations.insert("declare void @joyeer_string_destroy_abi(ptr)");
                out << "  call void @joyeer_string_destroy_abi(ptr %value)\n";
                break;
            case typing::TypeKind::array:
                runtimeDeclarations.insert("declare void @joyeer_array_destroy_abi(ptr)");
                out << "  call void @joyeer_array_destroy_abi(ptr %value)\n";
                break;
            case typing::TypeKind::dictionary:
                runtimeDeclarations.insert("declare void @joyeer_dictionary_destroy_abi(ptr)");
                out << "  call void @joyeer_dictionary_destroy_abi(ptr %value)\n";
                break;
            case typing::TypeKind::structure:
                emitStructDestroy(out, id, *typeText);
                break;
            case typing::TypeKind::enumeration:
            case typing::TypeKind::optional:
            case typing::TypeKind::result:
                emitEnumDestroy(out, id, *typeText);
                break;
            default:
                return;
        }
        out << "  ret void\n}\n";
        functionBodies.push_back(out.str());
    }

    void emitStructDestroy(
            std::ostringstream& out,
            ir::TypeId id,
            const std::string& typeText) {
        const auto* structure = structures.at(id);
        for (size_t index = structure->fields.size(); index > 0; --index) {
            const auto& field = structure->fields[index - 1];
            if (!ir::requiresDestruction(*module, field.type)) continue;
            const auto address = "%field." + std::to_string(index - 1);
            out << "  " << address << " = getelementptr inbounds " << typeText
                << ", ptr %value, i32 0, i32 " << index - 1 << "\n"
                << "  call void " << destroyHelper(field.type) << "(ptr "
                << address << ")\n";
        }
        out << "  store " << typeText << " zeroinitializer, ptr %value\n";
    }

    void emitEnumDestroy(
            std::ostringstream& out,
            ir::TypeId id,
            const std::string& typeText) {
        const auto* enumeration = enumerations.at(id);
        out << "  %whole = load " << typeText << ", ptr %value\n"
            << "  %tag = extractvalue " << typeText << " %whole, 0\n"
            << "  switch i32 %tag, label %exit [";
        for (size_t index = 0; index < enumeration->cases.size(); ++index) {
            out << " i32 " << index << ", label %case." << index;
        }
        out << " ]\n";
        for (size_t caseIndex = 0; caseIndex < enumeration->cases.size(); ++caseIndex) {
            const auto& enumCase = enumeration->cases[caseIndex];
            const auto offsets = payloadOffsets(enumCase);
            if (!offsets.has_value()) return;
            out << "case." << caseIndex << ":\n";
            for (size_t payload = enumCase.payloadTypes.size(); payload > 0; --payload) {
                const auto payloadType = enumCase.payloadTypes[payload - 1];
                if (!ir::requiresDestruction(*module, payloadType)) continue;
                const auto address = emitEnumPayloadAddress(
                        out,
                        "%value",
                        "value." + std::to_string(caseIndex) + '.' + std::to_string(payload - 1),
                        typeText,
                        (*offsets)[payload - 1]);
                out << "  call void " << destroyHelper(payloadType) << "(ptr "
                    << address << ")\n";
            }
            out << "  br label %exit\n";
        }
        out << "exit:\n"
            << "  store " << typeText << " zeroinitializer, ptr %value\n";
    }

    std::optional<std::string> llvmType(ir::TypeId id, SourceSpan span = {}) {
        const auto found = types.find(id);
        if (found == types.end()) {
            reportHere(
                    DiagnosticId::unsupportedType,
                    span,
                    "unknown Joyeer IR type " + std::to_string(id));
            return std::nullopt;
        }
        switch (found->second->kind) {
            case typing::TypeKind::voidType:
            case typing::TypeKind::never:
                return "void";
            case typing::TypeKind::integer:
                return "i64";
            case typing::TypeKind::boolean:
                return "i1";
            case typing::TypeKind::uint8:
                return "i8";
            case typing::TypeKind::string:
                usesString = true;
                return "%joyeer.string";
            case typing::TypeKind::array:
                usesArray = true;
                return "%joyeer.array";
            case typing::TypeKind::dictionary:
                usesDictionary = true;
                return "%joyeer.dictionary";
            case typing::TypeKind::structure:
                if (structures.contains(id)) {
                    return "%joyeer.struct." + std::to_string(id);
                }
                break;
            case typing::TypeKind::enumeration:
            case typing::TypeKind::optional:
            case typing::TypeKind::result:
                if (enumerations.contains(id)) {
                    return "%joyeer.enum." + std::to_string(id);
                }
                break;
            default:
                break;
        }
        reportHere(
                DiagnosticId::unsupportedType,
                span,
                "LLVM lowering is not implemented for type '" +
                        found->second->name + "'");
        return std::nullopt;
    }

    void emitEntryPoint() {
        const auto found = std::find_if(
                module->functions.begin(),
                module->functions.end(),
                [](const auto& function) {
                    return !function.isExternal && function.name == "main";
                });
        if (found == module->functions.end()) return;
        const auto* resultType = type(found->resultType);
        if (!found->parameters.empty() || resultType == nullptr ||
            resultType->kind != typing::TypeKind::voidType) {
            report(
                    DiagnosticId::invalidEntryPoint,
                    {},
                    found->id,
                    std::nullopt,
                    "entry function must have signature 'func main()'");
            return;
        }
        functionBodies.push_back(
                "define void @joyeer_main() {\n"
                "entry:\n"
                "  call void " + functionName(*found) + "()\n"
                "  ret void\n"
                "}\n");
        hasEntryPoint = true;
    }

    const ir::TypeName* type(ir::TypeId id) const {
        const auto found = types.find(id);
        return found == types.end() ? nullptr : found->second;
    }

    const ir::Value* value(ir::ValueId id) const {
        const auto found = values.find(id);
        return found == values.end() ? nullptr : &found->second;
    }

    std::optional<std::string> operand(ir::ValueId id) const {
        const auto found = operands.find(id);
        if (found == operands.end()) return std::nullopt;
        return found->second;
    }

    void emitFunction(const ir::Function& function) {
        currentFunction = &function;
        currentBlock = nullptr;
        values.clear();
        operands.clear();

        const auto resultType = llvmType(function.resultType);
        if (!resultType.has_value()) return;
        std::vector<std::string> parameterTypes;
        for (const auto& parameter : function.parameters) {
            const auto typeText = parameter.value.category == ir::ValueCategory::address
                    ? std::optional<std::string>("ptr")
                    : llvmType(parameter.value.type, parameter.span);
            if (!typeText.has_value()) return;
            parameterTypes.push_back(*typeText);
            values.emplace(parameter.value.id, parameter.value);
            operands.emplace(parameter.value.id, valueName(parameter.value.id));
        }
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!instruction.result.has_value()) continue;
                values.emplace(instruction.result->id, *instruction.result);
                operands.emplace(
                        instruction.result->id,
                        valueName(instruction.result->id));
            }
        }

        std::ostringstream out;
        out << "define " << *resultType << ' ' << functionName(function) << '(';
        for (size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0) out << ", ";
            out << parameterTypes[index] << ' '
                << valueName(function.parameters[index].value.id);
        }
        out << ") {\n";
        for (const auto& block : function.blocks) {
            currentBlock = &block;
            out << blockName(block.id) << ":\n";
            for (const auto& instruction : block.instructions) {
                if (!emitInstruction(out, instruction)) return;
            }
        }
        out << "}\n";
        functionBodies.push_back(out.str());
    }

    bool emitInstruction(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto unsupported = [&]() {
            reportHere(
                    DiagnosticId::unsupportedInstruction,
                    instruction.span,
                    std::string("LLVM lowering is not implemented for '") +
                            ir::opcodeName(instruction.opcode) + "'");
            return false;
        };
        const auto requiredOperand = [&](size_t index) -> std::optional<std::string> {
            if (index >= instruction.operands.size()) return std::nullopt;
            return operand(instruction.operands[index]);
        };

        switch (instruction.opcode) {
            case ir::Opcode::integerConstant:
            case ir::Opcode::byteConstant:
                operands[instruction.result->id] = std::to_string(instruction.integerValue);
                return true;
            case ir::Opcode::booleanConstant:
                operands[instruction.result->id] =
                        instruction.integerValue == 0 ? "false" : "true";
                return true;
            case ir::Opcode::stringConstant:
                return emitStringConstant(instruction);
            case ir::Opcode::stackAllocate: {
                const auto typeText = llvmType(instruction.result->type, instruction.span);
                if (!typeText.has_value()) return false;
                out << "  " << valueName(instruction.result->id)
                    << " = alloca " << *typeText << "\n";
                return true;
            }
            case ir::Opcode::zeroInitialize: {
                const auto address = requiredOperand(0);
                const auto* storage = value(instruction.operands[0]);
                const auto typeText = storage == nullptr
                        ? std::optional<std::string>()
                    : llvmType(storage->type, instruction.span);
                if (!address.has_value() || !typeText.has_value()) return false;
                out << "  store " << *typeText << " zeroinitializer, ptr "
                    << *address << "\n";
                return true;
            }
            case ir::Opcode::load: {
                const auto address = requiredOperand(0);
                const auto typeText = llvmType(instruction.result->type, instruction.span);
                if (!address.has_value() || !typeText.has_value()) return false;
                out << "  " << valueName(instruction.result->id)
                    << " = load " << *typeText << ", ptr " << *address << "\n";
                return true;
            }
            case ir::Opcode::store:
                return emitStore(out, instruction);
            case ir::Opcode::copyValue:
                return emitCopy(out, instruction);
            case ir::Opcode::take:
                return emitTake(out, instruction);
            case ir::Opcode::destroy:
                return emitDestroy(out, instruction);
            case ir::Opcode::add:
            case ir::Opcode::subtract:
            case ir::Opcode::multiply:
                return emitArithmetic(out, instruction);
            case ir::Opcode::less:
            case ir::Opcode::lessEqual:
            case ir::Opcode::greater:
            case ir::Opcode::greaterEqual:
            case ir::Opcode::equal:
            case ir::Opcode::notEqual:
                return emitComparison(out, instruction);
            case ir::Opcode::logicalAnd: {
                const auto left = requiredOperand(0);
                const auto right = requiredOperand(1);
                if (!left.has_value() || !right.has_value()) return false;
                out << "  " << valueName(instruction.result->id)
                    << " = and i1 " << *left << ", " << *right << "\n";
                return true;
            }
            case ir::Opcode::call:
                return emitCall(out, instruction);
            case ir::Opcode::constructStruct:
                return emitConstructStruct(out, instruction);
            case ir::Opcode::constructArray:
                return emitConstructArray(out, instruction);
            case ir::Opcode::arrayAppend:
                return emitArrayAppend(out, instruction);
            case ir::Opcode::constructDictionary:
                return emitConstructDictionary(out, instruction);
            case ir::Opcode::dictionarySet:
                return emitDictionarySet(out, instruction);
            case ir::Opcode::fieldAddress:
                return emitFieldAddress(out, instruction);
            case ir::Opcode::extractField:
                return emitExtractField(out, instruction);
            case ir::Opcode::constructEnum:
                return emitConstructEnum(out, instruction);
            case ir::Opcode::extractPayload:
                return emitExtractPayload(out, instruction);
            case ir::Opcode::count:
                return emitCount(out, instruction);
            case ir::Opcode::subscript:
                return emitSubscript(out, instruction, false);
            case ir::Opcode::subscriptAddress:
                return emitSubscript(out, instruction, true);
            case ir::Opcode::branch:
                out << "  br label %" << blockName(instruction.targets[0]) << "\n";
                return true;
            case ir::Opcode::conditionalBranch: {
                const auto condition = requiredOperand(0);
                if (!condition.has_value()) return false;
                out << "  br i1 " << *condition << ", label %"
                    << blockName(instruction.targets[0]) << ", label %"
                    << blockName(instruction.targets[1]) << "\n";
                return true;
            }
            case ir::Opcode::switchPattern:
                return emitSwitchPattern(out, instruction);
            case ir::Opcode::returnValue:
                return emitReturn(out, instruction);
            case ir::Opcode::returnVoid:
                out << "  ret void\n";
                return true;
            case ir::Opcode::unreachable:
                out << "  unreachable\n";
                return true;
            default:
                return unsupported();
        }
    }

    bool emitStringConstant(const ir::Instruction& instruction) {
        const auto globalName = "@.joyeer.string." + std::to_string(nextString++);
        const auto length = instruction.text.size();
        std::ostringstream global;
        global << globalName << " = private unnamed_addr constant [" << length
               << " x i8] c\"" << escapeQuoted(instruction.text) << "\", align 1";
        stringGlobals.push_back(global.str());
        usesString = true;
        operands[instruction.result->id] =
            "{ ptr " + globalName + ", i64 " +
                std::to_string(length) + " }";
        return true;
    }

    std::string temporary() {
        return "%tmp" + std::to_string(nextTemporary++);
    }

    struct HandleParts {
        std::string data;
        std::string count;
    };

    std::optional<HandleParts> emitHandleParts(
            std::ostringstream& out,
            const std::string& llvmType,
            const std::string& value) {
        const auto data = temporary();
        const auto count = temporary();
        out << "  " << data << " = extractvalue " << llvmType << ' ' << value
            << ", 0\n"
            << "  " << count << " = extractvalue " << llvmType << ' ' << value
            << ", 1\n";
        return HandleParts { data, count };
    }

    bool emitConstructStruct(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        const auto found = structures.find(instruction.result->type);
        if (found == structures.end()) return false;
        const auto typeText = llvmType(instruction.result->type, instruction.span);
        if (!typeText.has_value()) return false;
        if (instruction.operands.empty()) {
            operands[instruction.result->id] = "zeroinitializer";
            return true;
        }

        std::string aggregate = "poison";
        for (size_t index = 0; index < instruction.operands.size(); ++index) {
            const auto fieldValue = operand(instruction.operands[index]);
            const auto fieldType = llvmType(found->second->fields[index].type, instruction.span);
            if (!fieldValue.has_value() || !fieldType.has_value()) return false;
            const auto destination = index + 1 == instruction.operands.size()
                    ? valueName(instruction.result->id)
                    : temporary();
            out << "  " << destination << " = insertvalue " << *typeText << ' '
                << aggregate << ", " << *fieldType << ' ' << *fieldValue
                << ", " << index << "\n";
            aggregate = destination;
        }
        return true;
    }

    bool emitConstructArray(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        const auto* arrayType = type(instruction.result->type);
        if (arrayType == nullptr || arrayType->kind != typing::TypeKind::array ||
            arrayType->arguments.size() != 1) {
            return false;
        }
        const auto elementType = llvmType(arrayType->arguments[0], instruction.span);
        const auto elementLayout = layoutFor(arrayType->arguments[0]);
        if (!elementType.has_value() || !elementLayout.has_value()) return false;
        usesArray = true;
        runtimeDeclarations.insert(
            "declare void @joyeer_array_create_owned_abi(ptr, ptr, i64, i64, ptr, ptr)");

        std::string data = "null";
        if (!instruction.operands.empty()) {
            data = temporary();
            out << "  " << data << " = alloca [" << instruction.operands.size()
                << " x " << *elementType << "]\n";
            for (size_t index = 0; index < instruction.operands.size(); ++index) {
                const auto element = operand(instruction.operands[index]);
                if (!element.has_value()) return false;
                const auto address = temporary();
                out << "  " << address << " = getelementptr inbounds ["
                    << instruction.operands.size() << " x " << *elementType
                    << "], ptr " << data << ", i32 0, i64 " << index << "\n"
                    << "  store " << *elementType << ' ' << *element
                    << ", ptr " << address << "\n";
            }
        }
        const auto resultAddress = temporary();
        const auto ownsElements = ir::requiresDestruction(*module, arrayType->arguments[0]);
        const auto clone = ownsElements
            ? "ptr " + cloneHelper(arrayType->arguments[0])
            : std::string("ptr null");
        const auto destroy = ownsElements
            ? "ptr " + destroyHelper(arrayType->arguments[0])
            : std::string("ptr null");
        out << "  " << resultAddress << " = alloca %joyeer.array\n"
            << "  call void @joyeer_array_create_owned_abi(ptr " << resultAddress
            << ", ptr " << data << ", i64 " << instruction.operands.size()
            << ", i64 " << elementLayout->size << ", " << clone << ", "
            << destroy << ")\n"
            << "  " << valueName(instruction.result->id)
            << " = load %joyeer.array, ptr " << resultAddress << "\n";
        return true;
    }

    bool emitArrayAppend(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        if (instruction.operands.size() != 2) return false;
        const auto array = operand(instruction.operands[0]);
        const auto element = operand(instruction.operands[1]);
        const auto* elementValue = value(instruction.operands[1]);
        const auto elementType = elementValue == nullptr
                ? std::optional<std::string>()
                : llvmType(elementValue->type, instruction.span);
        if (!array.has_value() || !element.has_value() ||
            !elementType.has_value()) {
            return false;
        }

        const auto elementAddress = temporary();
        out << "  " << elementAddress << " = alloca " << *elementType << "\n"
            << "  store " << *elementType << ' ' << *element
            << ", ptr " << elementAddress << "\n";
        runtimeDeclarations.insert(
                "declare void @joyeer_array_append_owned_abi(ptr, ptr)");
        out << "  call void @joyeer_array_append_owned_abi(ptr " << *array
            << ", ptr " << elementAddress << ")\n";
        return true;
    }

    bool emitConstructDictionary(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        const auto* dictionaryType = type(instruction.result->type);
        if (dictionaryType == nullptr ||
            dictionaryType->kind != typing::TypeKind::dictionary ||
            dictionaryType->arguments.size() != 2 ||
            instruction.operands.size() % 2 != 0) {
            return false;
        }
        const auto keyType = llvmType(dictionaryType->arguments[0], instruction.span);
        const auto valueType = llvmType(dictionaryType->arguments[1], instruction.span);
        const auto keyLayout = layoutFor(dictionaryType->arguments[0]);
        const auto valueLayout = layoutFor(dictionaryType->arguments[1]);
        if (!keyType.has_value() || !valueType.has_value() ||
            !keyLayout.has_value() || !valueLayout.has_value()) {
            return false;
        }
        usesDictionary = true;
        runtimeDeclarations.insert(
            "declare void @joyeer_dictionary_create_owned_abi(ptr, ptr, i64, i64, i64, i64, i64, i32, ptr, ptr, ptr, ptr)");

        const auto count = instruction.operands.size() / 2;
        const auto entryType = "{ " + *keyType + ", " + *valueType + " }";
        const auto valueOffset = alignTo(keyLayout->size, valueLayout->alignment);
        const auto entrySize = alignTo(
            valueOffset + valueLayout->size,
            std::max(keyLayout->alignment, valueLayout->alignment));
        const auto keyKind = runtimeKeyKind(dictionaryType->arguments[0]);
        if (!keyKind.has_value()) return false;
        std::string data = "null";
        if (count != 0) {
            data = temporary();
            out << "  " << data << " = alloca [" << count << " x "
                << entryType << "]\n";
            for (size_t index = 0; index < count; ++index) {
                const auto key = operand(instruction.operands[index * 2]);
                const auto storedValue = operand(instruction.operands[index * 2 + 1]);
                if (!key.has_value() || !storedValue.has_value()) return false;
                const auto entryAddress = temporary();
                const auto keyAddress = temporary();
                const auto valueAddress = temporary();
                out << "  " << entryAddress << " = getelementptr inbounds ["
                    << count << " x " << entryType << "], ptr " << data
                    << ", i32 0, i64 " << index << "\n"
                    << "  " << keyAddress << " = getelementptr inbounds " << entryType
                    << ", ptr " << entryAddress << ", i32 0, i32 0\n"
                    << "  store " << *keyType << ' ' << *key << ", ptr "
                    << keyAddress << "\n"
                    << "  " << valueAddress << " = getelementptr inbounds " << entryType
                    << ", ptr " << entryAddress << ", i32 0, i32 1\n"
                    << "  store " << *valueType << ' ' << *storedValue << ", ptr "
                    << valueAddress << "\n";
            }
        }
        const auto resultAddress = temporary();
        const auto ownsKeys = ir::requiresDestruction(*module, dictionaryType->arguments[0]);
        const auto ownsValues = ir::requiresDestruction(*module, dictionaryType->arguments[1]);
        const auto cloneKey = ownsKeys
            ? "ptr " + cloneHelper(dictionaryType->arguments[0])
            : std::string("ptr null");
        const auto destroyKey = ownsKeys
            ? "ptr " + destroyHelper(dictionaryType->arguments[0])
            : std::string("ptr null");
        const auto cloneValue = ownsValues
            ? "ptr " + cloneHelper(dictionaryType->arguments[1])
            : std::string("ptr null");
        const auto destroyValue = ownsValues
            ? "ptr " + destroyHelper(dictionaryType->arguments[1])
            : std::string("ptr null");
        out << "  " << resultAddress << " = alloca %joyeer.dictionary\n"
            << "  call void @joyeer_dictionary_create_owned_abi(ptr " << resultAddress
            << ", ptr " << data << ", i64 " << count << ", i64 " << keyLayout->size
            << ", i64 " << valueLayout->size << ", i64 " << entrySize
            << ", i64 " << valueOffset << ", i32 " << *keyKind << ", "
            << cloneKey << ", " << destroyKey << ", " << cloneValue << ", "
            << destroyValue << ")\n"
            << "  " << valueName(instruction.result->id)
            << " = load %joyeer.dictionary, ptr " << resultAddress << "\n";
        return true;
    }

    bool emitDictionarySet(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        if (instruction.operands.size() != 3) return false;
        const auto dictionary = operand(instruction.operands[0]);
        const auto key = operand(instruction.operands[1]);
        const auto storedValue = operand(instruction.operands[2]);
        const auto* keyValue = value(instruction.operands[1]);
        const auto* valueValue = value(instruction.operands[2]);
        const auto keyType = keyValue == nullptr
                ? std::optional<std::string>()
                : llvmType(keyValue->type, instruction.span);
        const auto valueType = valueValue == nullptr
                ? std::optional<std::string>()
                : llvmType(valueValue->type, instruction.span);
        if (!dictionary.has_value() || !key.has_value() ||
            !storedValue.has_value() || !keyType.has_value() ||
            !valueType.has_value()) {
            return false;
        }

        const auto keyAddress = temporary();
        const auto valueAddress = temporary();
        out << "  " << keyAddress << " = alloca " << *keyType << "\n"
            << "  store " << *keyType << ' ' << *key
            << ", ptr " << keyAddress << "\n"
            << "  " << valueAddress << " = alloca " << *valueType << "\n"
            << "  store " << *valueType << ' ' << *storedValue
            << ", ptr " << valueAddress << "\n";
        usesDictionary = true;
        runtimeDeclarations.insert(
                "declare void @joyeer_dictionary_set_owned_abi(ptr, ptr, ptr)");
        out << "  call void @joyeer_dictionary_set_owned_abi(ptr " << *dictionary
            << ", ptr " << keyAddress << ", ptr " << valueAddress << ")\n";
        return true;
    }

    std::optional<int> runtimeKeyKind(ir::TypeId id) {
        const auto* keyType = type(id);
        if (keyType == nullptr) return std::nullopt;
        switch (keyType->kind) {
            case typing::TypeKind::integer: return 1;
            case typing::TypeKind::boolean: return 2;
            case typing::TypeKind::string: return 3;
            case typing::TypeKind::uint8: return 4;
            default:
                reportHere(
                        DiagnosticId::unsupportedType,
                        {},
                        "dictionary keys are not yet supported for type '" +
                                keyType->name + "'");
                return std::nullopt;
        }
    }

    bool emitFieldAddress(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        if (!instruction.symbol.has_value()) return false;
        const auto found = fields.find(*instruction.symbol);
        const auto base = operand(instruction.operands[0]);
        if (found == fields.end() || !base.has_value()) return false;
        const auto typeText = llvmType(found->second.first->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  " << valueName(instruction.result->id)
            << " = getelementptr inbounds " << *typeText << ", ptr " << *base
            << ", i32 0, i32 " << found->second.second << "\n";
        return true;
    }

    bool emitExtractField(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        if (!instruction.symbol.has_value()) return false;
        const auto found = fields.find(*instruction.symbol);
        const auto base = operand(instruction.operands[0]);
        if (found == fields.end() || !base.has_value()) return false;
        const auto typeText = llvmType(found->second.first->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  " << valueName(instruction.result->id)
            << " = extractvalue " << *typeText << ' ' << *base
            << ", " << found->second.second << "\n";
        return true;
    }

    const ir::EnumCaseDefinition* enumCase(
            semantic::SymbolId symbol,
            const ir::EnumerationDefinition** owner = nullptr,
            size_t* index = nullptr) const {
        const auto found = cases.find(symbol);
        if (found == cases.end()) return nullptr;
        if (owner != nullptr) *owner = found->second.first;
        if (index != nullptr) *index = found->second.second;
        return &found->second.first->cases[found->second.second];
    }

    bool emitConstructEnum(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        if (!instruction.symbol.has_value()) return false;
        const ir::EnumerationDefinition* enumeration = nullptr;
        size_t caseIndex = 0;
        const auto* selectedCase = enumCase(
                *instruction.symbol,
                &enumeration,
                &caseIndex);
        if (selectedCase == nullptr || enumeration == nullptr ||
            enumeration->type != instruction.result->type) {
            return false;
        }
        const auto typeText = llvmType(enumeration->type, instruction.span);
        const auto offsets = payloadOffsets(*selectedCase);
        if (!typeText.has_value() || !offsets.has_value()) return false;

        const auto storage = temporary();
        const auto tagAddress = temporary();
        out << "  " << storage << " = alloca " << *typeText << "\n"
            << "  store " << *typeText << " zeroinitializer, ptr " << storage << "\n"
            << "  " << tagAddress << " = getelementptr inbounds " << *typeText
            << ", ptr " << storage << ", i32 0, i32 0\n"
            << "  store i32 " << caseIndex << ", ptr " << tagAddress << "\n";

        if (!instruction.operands.empty()) {
            const auto payloadBase = temporary();
            out << "  " << payloadBase << " = getelementptr inbounds " << *typeText
                << ", ptr " << storage << ", i32 0, i32 1, i32 0\n";
            for (size_t index = 0; index < instruction.operands.size(); ++index) {
                const auto payload = operand(instruction.operands[index]);
                const auto payloadType = llvmType(
                        selectedCase->payloadTypes[index],
                        instruction.span);
                if (!payload.has_value() || !payloadType.has_value()) return false;
                auto payloadAddress = payloadBase;
                if ((*offsets)[index] != 0) {
                    payloadAddress = temporary();
                    out << "  " << payloadAddress << " = getelementptr inbounds i8, ptr "
                        << payloadBase << ", i64 " << (*offsets)[index] << "\n";
                }
                out << "  store " << *payloadType << ' ' << *payload
                    << ", ptr " << payloadAddress << "\n";
            }
        }
        out << "  " << valueName(instruction.result->id) << " = load "
            << *typeText << ", ptr " << storage << "\n";
        return true;
    }

    std::optional<std::string> emitPayloadLoad(
            std::ostringstream& out,
            const std::string& enumValue,
            ir::TypeId enumType,
            semantic::SymbolId caseSymbol,
            size_t payloadIndex,
            SourceSpan span,
            const std::string& requestedResult = {}) {
        const ir::EnumerationDefinition* enumeration = nullptr;
        const auto* selectedCase = enumCase(caseSymbol, &enumeration);
        if (selectedCase == nullptr || enumeration == nullptr ||
            enumeration->type != enumType ||
            payloadIndex >= selectedCase->payloadTypes.size()) {
            return std::nullopt;
        }
        const auto enumTypeText = llvmType(enumType, span);
        const auto payloadTypeText = llvmType(
                selectedCase->payloadTypes[payloadIndex],
                span);
        const auto offsets = payloadOffsets(*selectedCase);
        if (!enumTypeText.has_value() || !payloadTypeText.has_value() ||
            !offsets.has_value()) {
            return std::nullopt;
        }

        const auto storage = temporary();
        const auto payloadBase = temporary();
        out << "  " << storage << " = alloca " << *enumTypeText << "\n"
            << "  store " << *enumTypeText << ' ' << enumValue << ", ptr " << storage << "\n"
            << "  " << payloadBase << " = getelementptr inbounds " << *enumTypeText
            << ", ptr " << storage << ", i32 0, i32 1, i32 0\n";
        auto payloadAddress = payloadBase;
        if ((*offsets)[payloadIndex] != 0) {
            payloadAddress = temporary();
            out << "  " << payloadAddress << " = getelementptr inbounds i8, ptr "
                << payloadBase << ", i64 " << (*offsets)[payloadIndex] << "\n";
        }
        const auto result = requestedResult.empty() ? temporary() : requestedResult;
        out << "  " << result << " = load " << *payloadTypeText
            << ", ptr " << payloadAddress << "\n";
        return result;
    }

    bool emitExtractPayload(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        if (!instruction.symbol.has_value()) return false;
        const auto source = operand(instruction.operands[0]);
        const auto* sourceValue = value(instruction.operands[0]);
        if (!source.has_value() || sourceValue == nullptr || instruction.integerValue < 0) {
            return false;
        }
        return emitPayloadLoad(
                out,
                *source,
                sourceValue->type,
                *instruction.symbol,
                static_cast<size_t>(instruction.integerValue),
                instruction.span,
                valueName(instruction.result->id)).has_value();
    }

    bool emitCount(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto base = operand(instruction.operands[0]);
        const auto* baseValue = value(instruction.operands[0]);
        if (!base.has_value() || baseValue == nullptr) return false;
        const auto baseType = llvmType(baseValue->type, instruction.span);
        const auto* baseTypeInfo = type(baseValue->type);
        if (!baseType.has_value() || baseTypeInfo == nullptr ||
            (baseTypeInfo->kind != typing::TypeKind::string &&
             baseTypeInfo->kind != typing::TypeKind::array &&
             baseTypeInfo->kind != typing::TypeKind::dictionary)) {
            return false;
        }
        out << "  " << valueName(instruction.result->id) << " = extractvalue "
            << *baseType << ' ' << *base << ", 1\n";
        return true;
    }

    bool emitSubscript(
            std::ostringstream& out,
            const ir::Instruction& instruction,
            bool returnsAddress) {
        const auto base = operand(instruction.operands[0]);
        const auto index = operand(instruction.operands[1]);
        const auto* baseValue = value(instruction.operands[0]);
        if (!base.has_value() || !index.has_value() || baseValue == nullptr) return false;
        const auto* baseType = type(baseValue->type);
        if (baseType == nullptr) return false;

        if (baseType->kind == typing::TypeKind::string && !returnsAddress) {
            usesString = true;
            runtimeDeclarations.insert(
                "declare i8 @joyeer_string_byte_at_abi(ptr, i64, i64)");
            const auto parts = emitHandleParts(out, "%joyeer.string", *base);
            if (!parts.has_value()) return false;
            out << "  " << valueName(instruction.result->id)
            << " = call i8 @joyeer_string_byte_at_abi(ptr " << parts->data
            << ", i64 " << parts->count << ", i64 " << *index << ")\n";
            return true;
        }

        if (baseType->kind == typing::TypeKind::array) {
            return emitArraySubscript(out, instruction, *base, *index, returnsAddress);
        }
        if (baseType->kind == typing::TypeKind::dictionary) {
            return emitDictionarySubscript(
                    out,
                    instruction,
                    *base,
                    *index,
                    returnsAddress);
        }

        reportHere(
                DiagnosticId::unsupportedInstruction,
                instruction.span,
                "subscript is not implemented for type '" + baseType->name + "'");
        return false;
    }

    bool emitArraySubscript(
            std::ostringstream& out,
            const ir::Instruction& instruction,
            const std::string& base,
            const std::string& index,
            bool returnsAddress) {
        usesArray = true;
        runtimeDeclarations.insert(
            "declare ptr @joyeer_array_at_abi(ptr, i64, i64)");
        auto arrayValue = base;
        if (returnsAddress) {
            arrayValue = temporary();
            out << "  " << arrayValue << " = load %joyeer.array, ptr " << base << "\n";
        }
        const auto parts = emitHandleParts(out, "%joyeer.array", arrayValue);
        if (!parts.has_value()) return false;
        const auto elementAddress = returnsAddress
                ? valueName(instruction.result->id)
                : temporary();
        out << "  " << elementAddress
            << " = call ptr @joyeer_array_at_abi(ptr " << parts->data
            << ", i64 " << parts->count << ", i64 " << index << ")\n";
        if (returnsAddress) return true;
        const auto resultType = llvmType(instruction.result->type, instruction.span);
        if (!resultType.has_value()) return false;
        out << "  " << valueName(instruction.result->id) << " = load "
            << *resultType << ", ptr " << elementAddress << "\n";
        return true;
    }

    bool emitDictionarySubscript(
            std::ostringstream& out,
            const ir::Instruction& instruction,
            const std::string& base,
            const std::string& key,
            bool returnsAddress) {
        const auto* dictionaryType = type(value(instruction.operands[0])->type);
        if (dictionaryType == nullptr || dictionaryType->arguments.size() != 2) return false;
        const auto keyType = llvmType(dictionaryType->arguments[0], instruction.span);
        const auto keyLayout = layoutFor(dictionaryType->arguments[0]);
        const auto keyKind = runtimeKeyKind(dictionaryType->arguments[0]);
        if (!keyType.has_value() || !keyLayout.has_value() || !keyKind.has_value()) {
            return false;
        }
        usesDictionary = true;
        runtimeDeclarations.insert(
            "declare ptr @joyeer_dictionary_at_abi(ptr, i64, ptr, i64, i32)");

        auto dictionaryValue = base;
        if (returnsAddress) {
            dictionaryValue = temporary();
            out << "  " << dictionaryValue
                << " = load %joyeer.dictionary, ptr " << base << "\n";
        }
        const auto parts = emitHandleParts(out, "%joyeer.dictionary", dictionaryValue);
        if (!parts.has_value()) return false;
        const auto keyAddress = temporary();
        const auto valueAddress = returnsAddress
                ? valueName(instruction.result->id)
                : temporary();
        out << "  " << keyAddress << " = alloca " << *keyType << "\n"
            << "  store " << *keyType << ' ' << key << ", ptr " << keyAddress << "\n"
            << "  " << valueAddress
            << " = call ptr @joyeer_dictionary_at_abi(ptr " << parts->data
            << ", i64 " << parts->count << ", ptr " << keyAddress << ", i64 "
            << keyLayout->size << ", i32 " << *keyKind << ")\n";
        if (returnsAddress) return true;
        const auto resultType = llvmType(instruction.result->type, instruction.span);
        if (!resultType.has_value()) return false;
        out << "  " << valueName(instruction.result->id) << " = load "
            << *resultType << ", ptr " << valueAddress << "\n";
        return true;
    }

    std::string stringLiteralOperand(const std::string& text) {
        const auto globalName = "@.joyeer.string." + std::to_string(nextString++);
        std::ostringstream global;
        global << globalName << " = private unnamed_addr constant [" << text.size()
               << " x i8] c\"" << escapeQuoted(text) << "\", align 1";
        stringGlobals.push_back(global.str());
        usesString = true;
        return "{ ptr " + globalName + ", i64 " +
                std::to_string(text.size()) + " }";
    }

    std::optional<std::string> emitPatternCondition(
            std::ostringstream& out,
            const ir::Pattern& pattern,
            const std::string& testedValue,
            SourceSpan span) {
        switch (pattern.kind) {
            case ir::PatternKind::wildcard:
                return "true";
            case ir::PatternKind::integerLiteral:
            case ir::PatternKind::booleanLiteral:
            case ir::PatternKind::byteLiteral: {
                const auto typeText = llvmType(pattern.type, span);
                if (!typeText.has_value()) return std::nullopt;
                const auto condition = temporary();
                const auto literal = pattern.kind == ir::PatternKind::booleanLiteral
                        ? (pattern.integerValue == 0 ? std::string("false") : std::string("true"))
                        : std::to_string(pattern.integerValue);
                out << "  " << condition << " = icmp eq " << *typeText << ' '
                    << testedValue << ", " << literal << "\n";
                return condition;
            }
            case ir::PatternKind::stringLiteral: {
                runtimeDeclarations.insert(
                        "declare i1 @joyeer_string_equal_abi(ptr, i64, ptr, i64)");
                const auto testedParts = emitHandleParts(out, "%joyeer.string", testedValue);
                const auto literal = stringLiteralOperand(pattern.text);
                const auto literalParts = emitHandleParts(out, "%joyeer.string", literal);
                if (!testedParts.has_value() || !literalParts.has_value()) {
                    return std::nullopt;
                }
                const auto condition = temporary();
                out << "  " << condition
                    << " = call i1 @joyeer_string_equal_abi(ptr " << testedParts->data
                    << ", i64 " << testedParts->count << ", ptr " << literalParts->data
                    << ", i64 " << literalParts->count << ")\n";
                return condition;
            }
            case ir::PatternKind::enumCase: {
                if (!pattern.symbol.has_value()) return std::nullopt;
                const ir::EnumerationDefinition* enumeration = nullptr;
                size_t caseIndex = 0;
                const auto* selectedCase = enumCase(
                        *pattern.symbol,
                        &enumeration,
                        &caseIndex);
                if (selectedCase == nullptr || enumeration == nullptr ||
                    enumeration->type != pattern.type) {
                    return std::nullopt;
                }
                const auto typeText = llvmType(pattern.type, span);
                if (!typeText.has_value()) return std::nullopt;
                const auto tag = temporary();
                const auto tagMatches = temporary();
                out << "  " << tag << " = extractvalue " << *typeText << ' '
                    << testedValue << ", 0\n"
                    << "  " << tagMatches << " = icmp eq i32 " << tag
                    << ", " << caseIndex << "\n";
                auto condition = tagMatches;
                for (size_t index = 0; index < pattern.payloads.size(); ++index) {
                    if (pattern.payloads[index].kind == ir::PatternKind::wildcard) continue;
                    const auto payload = emitPayloadLoad(
                            out,
                            testedValue,
                            pattern.type,
                            *pattern.symbol,
                            index,
                            span);
                    if (!payload.has_value()) return std::nullopt;
                    const auto payloadMatches = emitPatternCondition(
                            out,
                            pattern.payloads[index],
                            *payload,
                            span);
                    if (!payloadMatches.has_value()) return std::nullopt;
                    const auto combined = temporary();
                    out << "  " << combined << " = and i1 " << condition
                        << ", " << *payloadMatches << "\n";
                    condition = combined;
                }
                return condition;
            }
        }
        return std::nullopt;
    }

    bool emitSwitchPattern(
            std::ostringstream& out,
            const ir::Instruction& instruction) {
        const auto scrutinee = operand(instruction.operands[0]);
        if (!scrutinee.has_value() || instruction.switchCases.empty()) return false;

        bool terminated = false;
        for (size_t index = 0; index < instruction.switchCases.size(); ++index) {
            if (index != 0) {
                out << "pattern." << currentBlock->id << '.' << index << ":\n";
            }
            const auto& switchCase = instruction.switchCases[index];
            const auto condition = emitPatternCondition(
                    out,
                    switchCase.pattern,
                    *scrutinee,
                    instruction.span);
            if (!condition.has_value()) return false;
            if (*condition == "true") {
                out << "  br label %" << blockName(switchCase.target) << "\n";
                terminated = true;
                break;
            }
            const auto miss = index + 1 < instruction.switchCases.size()
                    ? "pattern." + std::to_string(currentBlock->id) + '.' +
                            std::to_string(index + 1)
                    : "pattern." + std::to_string(currentBlock->id) + ".miss";
            out << "  br i1 " << *condition << ", label %"
                << blockName(switchCase.target) << ", label %" << miss << "\n";
        }
        if (!terminated) {
            out << "pattern." << currentBlock->id << ".miss:\n"
                << "  unreachable\n";
        }
        return true;
    }

    bool emitStore(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto stored = operand(instruction.operands[0]);
        const auto address = operand(instruction.operands[1]);
        const auto* storedValue = value(instruction.operands[0]);
        if (!stored.has_value() || !address.has_value() || storedValue == nullptr) {
            return false;
        }
        const auto typeText = llvmType(storedValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  store " << *typeText << ' ' << *stored
            << ", ptr " << *address << "\n";
        return true;
    }

    bool emitCopy(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto source = operand(instruction.operands[0]);
        const auto* sourceValue = value(instruction.operands[0]);
        if (!source.has_value() || sourceValue == nullptr) return false;
        if (!ir::requiresDestruction(*module, sourceValue->type)) {
            operands[instruction.result->id] = *source;
            return true;
        }
        const auto typeText = llvmType(sourceValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        const auto sourceAddress = temporary();
        const auto destinationAddress = temporary();
        out << "  " << sourceAddress << " = alloca " << *typeText << "\n"
            << "  store " << *typeText << ' ' << *source << ", ptr "
            << sourceAddress << "\n"
            << "  " << destinationAddress << " = alloca " << *typeText << "\n"
            << "  call void " << cloneHelper(sourceValue->type) << "(ptr "
            << destinationAddress << ", ptr " << sourceAddress << ")\n"
            << "  " << valueName(instruction.result->id) << " = load "
            << *typeText << ", ptr " << destinationAddress << "\n";
        return true;
    }

    bool emitTake(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto source = operand(instruction.operands[0]);
        if (!source.has_value()) return false;
        const auto typeText = llvmType(instruction.result->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  " << valueName(instruction.result->id) << " = load "
            << *typeText << ", ptr " << *source << "\n"
            << "  store " << *typeText << " zeroinitializer, ptr " << *source << "\n";
        return true;
    }

    bool emitDestroy(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto address = operand(instruction.operands[0]);
        const auto* storage = value(instruction.operands[0]);
        if (!address.has_value() || storage == nullptr ||
            !ir::requiresDestruction(*module, storage->type)) {
            return false;
        }
        out << "  call void " << destroyHelper(storage->type) << "(ptr "
            << *address << ")\n";
        return true;
    }

    bool emitArithmetic(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto left = operand(instruction.operands[0]);
        const auto right = operand(instruction.operands[1]);
        const auto* leftValue = value(instruction.operands[0]);
        if (!left.has_value() || !right.has_value() || leftValue == nullptr) return false;
        const auto* operandType = type(leftValue->type);
        const auto result = valueName(instruction.result->id);

        if (operandType != nullptr && operandType->kind == typing::TypeKind::integer) {
            std::string helper;
            switch (instruction.opcode) {
                case ir::Opcode::add: helper = "joyeer_checked_add_int"; break;
                case ir::Opcode::subtract: helper = "joyeer_checked_sub_int"; break;
                case ir::Opcode::multiply: helper = "joyeer_checked_mul_int"; break;
                default: return false;
            }
            runtimeDeclarations.insert("declare i64 @" + helper + "(i64, i64)");
            out << "  " << result << " = call i64 @" << helper
                << "(i64 " << *left << ", i64 " << *right << ")\n";
        } else if (operandType != nullptr &&
                   operandType->kind == typing::TypeKind::string &&
                   instruction.opcode == ir::Opcode::add) {
            usesString = true;
            runtimeDeclarations.insert(
                "declare void @joyeer_string_concat_abi(ptr, ptr, i64, ptr, i64)");
            const auto leftParts = emitHandleParts(out, "%joyeer.string", *left);
            const auto rightParts = emitHandleParts(out, "%joyeer.string", *right);
            if (!leftParts.has_value() || !rightParts.has_value()) return false;
            const auto resultAddress = temporary();
            out << "  " << resultAddress << " = alloca %joyeer.string\n"
            << "  call void @joyeer_string_concat_abi(ptr " << resultAddress
            << ", ptr " << leftParts->data << ", i64 " << leftParts->count
            << ", ptr " << rightParts->data << ", i64 " << rightParts->count
            << ")\n"
            << "  " << result << " = load %joyeer.string, ptr "
            << resultAddress << "\n";
        } else {
            reportHere(
                    DiagnosticId::unsupportedInstruction,
                    instruction.span,
                    "unsupported arithmetic operand type");
            return false;
        }
        return true;
    }

    bool emitComparison(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto left = operand(instruction.operands[0]);
        const auto right = operand(instruction.operands[1]);
        const auto* leftValue = value(instruction.operands[0]);
        if (!left.has_value() || !right.has_value() || leftValue == nullptr) return false;
        const auto* operandType = type(leftValue->type);
        const auto result = valueName(instruction.result->id);

        if (operandType != nullptr && operandType->kind == typing::TypeKind::string) {
            return emitStringComparison(out, instruction, *left, *right);
        }
        const auto typeText = llvmType(leftValue->type, instruction.span);
        if (!typeText.has_value() || operandType == nullptr) return false;
        out << "  " << result << " = icmp "
            << comparisonPredicate(
                    instruction.opcode,
                    operandType->kind == typing::TypeKind::integer)
            << ' ' << *typeText << ' ' << *left << ", " << *right << "\n";
        return true;
    }

    bool emitStringComparison(
            std::ostringstream& out,
            const ir::Instruction& instruction,
            const std::string& left,
            const std::string& right) {
        usesString = true;
        const auto result = valueName(instruction.result->id);
        const auto leftParts = emitHandleParts(out, "%joyeer.string", left);
        const auto rightParts = emitHandleParts(out, "%joyeer.string", right);
        if (!leftParts.has_value() || !rightParts.has_value()) return false;
        if (instruction.opcode == ir::Opcode::equal ||
            instruction.opcode == ir::Opcode::notEqual) {
            runtimeDeclarations.insert(
                "declare i1 @joyeer_string_equal_abi(ptr, i64, ptr, i64)");
            const auto equal = instruction.opcode == ir::Opcode::equal
                    ? result
                    : "%tmp" + std::to_string(nextTemporary++);
            out << "  " << equal
                << " = call i1 @joyeer_string_equal_abi(ptr " << leftParts->data
                << ", i64 " << leftParts->count << ", ptr " << rightParts->data
                << ", i64 " << rightParts->count << ")\n";
            if (instruction.opcode == ir::Opcode::notEqual) {
                out << "  " << result << " = xor i1 " << equal << ", true\n";
            }
            return true;
        }

        runtimeDeclarations.insert(
            "declare i64 @joyeer_string_compare_abi(ptr, i64, ptr, i64)");
        const auto compared = "%tmp" + std::to_string(nextTemporary++);
        out << "  " << compared
            << " = call i64 @joyeer_string_compare_abi(ptr " << leftParts->data
            << ", i64 " << leftParts->count << ", ptr " << rightParts->data
            << ", i64 " << rightParts->count << ")\n";
        out << "  " << result << " = icmp "
            << comparisonPredicate(instruction.opcode, true)
            << " i64 " << compared << ", 0\n";
        return true;
    }

    std::string comparisonPredicate(ir::Opcode opcode, bool isSigned) const {
        switch (opcode) {
            case ir::Opcode::less: return isSigned ? "slt" : "ult";
            case ir::Opcode::lessEqual: return isSigned ? "sle" : "ule";
            case ir::Opcode::greater: return isSigned ? "sgt" : "ugt";
            case ir::Opcode::greaterEqual: return isSigned ? "sge" : "uge";
            case ir::Opcode::equal: return "eq";
            case ir::Opcode::notEqual: return "ne";
            default: return "eq";
        }
    }

    bool emitCall(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto calleeFound = functions.find(*instruction.callee);
        if (calleeFound == functions.end()) return false;
        const auto& callee = *calleeFound->second;
        if (callee.isExternal) {
            if (callee.name == "print" && instruction.operands.size() == 1) {
                return emitPrint(out, instruction);
            }
            if (callee.name == "readFile" && instruction.operands.size() == 1) {
                return emitReadFile(out, instruction, callee);
            }
            reportHere(
                    DiagnosticId::unsupportedExternal,
                    instruction.span,
                    "unsupported external function '" + callee.name + "'");
            return false;
        }

        const auto returnType = llvmType(callee.resultType, instruction.span);
        if (!returnType.has_value()) return false;
        out << "  ";
        if (instruction.result.has_value()) {
            out << valueName(instruction.result->id) << " = ";
        }
        out << "call " << *returnType << ' ' << functionName(callee) << '(';
        for (size_t index = 0; index < instruction.operands.size(); ++index) {
            if (index != 0) out << ", ";
            const auto argument = operand(instruction.operands[index]);
            const auto* argumentValue = value(instruction.operands[index]);
            if (!argument.has_value() || argumentValue == nullptr) return false;
            const auto argumentType = argumentValue->category == ir::ValueCategory::address
                    ? std::optional<std::string>("ptr")
                    : llvmType(argumentValue->type, instruction.span);
            if (!argumentType.has_value()) return false;
            out << *argumentType << ' ' << *argument;
        }
        out << ")\n";
        return true;
    }

    bool emitReadFile(
            std::ostringstream& out,
            const ir::Instruction& instruction,
            const ir::Function& callee) {
        if (!instruction.result.has_value()) return false;
        const auto* pathValue = value(instruction.operands[0]);
        const auto path = operand(instruction.operands[0]);
        const auto* pathType = pathValue == nullptr ? nullptr : type(pathValue->type);
        const auto enumeration = enumerations.find(callee.resultType);
        if (!path.has_value() || pathType == nullptr ||
            pathType->kind != typing::TypeKind::string ||
            enumeration == enumerations.end()) {
            reportHere(
                    DiagnosticId::unsupportedExternal,
                    instruction.span,
                    "readFile requires String -> Result<String, Int>");
            return false;
        }

        std::optional<size_t> okTag;
        std::optional<size_t> errorTag;
        for (size_t index = 0; index < enumeration->second->cases.size(); ++index) {
            const auto& enumCase = enumeration->second->cases[index];
            if (enumCase.name == "Ok" && enumCase.payloadTypes.size() == 1) {
                const auto* payload = type(enumCase.payloadTypes[0]);
                if (payload != nullptr && payload->kind == typing::TypeKind::string) {
                    okTag = index;
                }
            } else if (enumCase.name == "Err" && enumCase.payloadTypes.size() == 1) {
                const auto* payload = type(enumCase.payloadTypes[0]);
                if (payload != nullptr && payload->kind == typing::TypeKind::integer) {
                    errorTag = index;
                }
            }
        }
        const auto resultType = llvmType(callee.resultType, instruction.span);
        const auto pathParts = emitHandleParts(out, "%joyeer.string", *path);
        if (!okTag.has_value() || !errorTag.has_value() ||
            !resultType.has_value() || !pathParts.has_value()) {
            reportHere(
                    DiagnosticId::unsupportedExternal,
                    instruction.span,
                    "readFile result must be Result<String, Int>");
            return false;
        }

        const auto storage = "%tmp" + std::to_string(nextTemporary++);
        const auto tagAddress = "%tmp" + std::to_string(nextTemporary++);
        const auto payloadAddress = "%tmp" + std::to_string(nextTemporary++);
        out << "  " << storage << " = alloca " << *resultType << "\n"
            << "  store " << *resultType << " zeroinitializer, ptr " << storage << "\n"
            << "  " << tagAddress << " = getelementptr inbounds " << *resultType
            << ", ptr " << storage << ", i32 0, i32 0\n"
            << "  " << payloadAddress << " = getelementptr inbounds " << *resultType
            << ", ptr " << storage << ", i32 0, i32 1, i32 0\n";
        runtimeDeclarations.insert(
                "declare void @joyeer_read_file_abi(ptr, ptr, i32, i32, ptr, i64)");
        out << "  call void @joyeer_read_file_abi(ptr " << tagAddress
            << ", ptr " << payloadAddress
            << ", i32 " << *okTag
            << ", i32 " << *errorTag
            << ", ptr " << pathParts->data
            << ", i64 " << pathParts->count << ")\n"
            << "  " << valueName(instruction.result->id) << " = load "
            << *resultType << ", ptr " << storage << "\n";
        return true;
    }

    bool emitPrint(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto argument = operand(instruction.operands[0]);
        const auto* argumentValue = value(instruction.operands[0]);
        if (!argument.has_value() || argumentValue == nullptr) return false;
        const auto* argumentType = type(argumentValue->type);
        if (argumentType == nullptr) return false;

        std::string runtimeName;
        switch (argumentType->kind) {
            case typing::TypeKind::integer: runtimeName = "joyeer_print_int"; break;
            case typing::TypeKind::boolean: runtimeName = "joyeer_print_bool"; break;
            case typing::TypeKind::uint8: runtimeName = "joyeer_print_byte"; break;
            case typing::TypeKind::string: {
                const auto parts = emitHandleParts(out, "%joyeer.string", *argument);
                if (!parts.has_value()) return false;
                runtimeDeclarations.insert(
                        "declare void @joyeer_print_string_abi(ptr, i64)");
                out << "  call void @joyeer_print_string_abi(ptr " << parts->data
                    << ", i64 " << parts->count << ")\n";
                return true;
            }
            default:
                reportHere(
                        DiagnosticId::unsupportedExternal,
                        instruction.span,
                        "print is not implemented for type '" + argumentType->name + "'");
                return false;
        }
        const auto typeText = llvmType(argumentValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        runtimeDeclarations.insert(
                "declare void @" + runtimeName + "(" + *typeText + ")");
        out << "  call void @" << runtimeName << '(' << *typeText << ' '
            << *argument << ")\n";
        return true;
    }

    bool emitReturn(std::ostringstream& out, const ir::Instruction& instruction) {
        const auto returned = operand(instruction.operands[0]);
        const auto* returnedValue = value(instruction.operands[0]);
        if (!returned.has_value() || returnedValue == nullptr) return false;
        const auto typeText = llvmType(returnedValue->type, instruction.span);
        if (!typeText.has_value()) return false;
        out << "  ret " << *typeText << ' ' << *returned << "\n";
        return true;
    }
};

} // namespace

Result Emitter::emit(const ir::Module& module) const {
    return Builder().build(module);
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::invalidModule: return "llvm.invalid-module";
        case DiagnosticId::unsupportedType: return "llvm.unsupported-type";
        case DiagnosticId::unsupportedInstruction: return "llvm.unsupported-instruction";
        case DiagnosticId::unsupportedExternal: return "llvm.unsupported-external";
        case DiagnosticId::invalidEntryPoint: return "llvm.invalid-entry-point";
    }
    return "llvm.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length;
        if (diagnostic.function.has_value()) out << " function=" << *diagnostic.function;
        if (diagnostic.block.has_value()) out << " block=" << *diagnostic.block;
        out << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::llvmbackend