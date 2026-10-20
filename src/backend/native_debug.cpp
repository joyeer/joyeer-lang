#include "native_ir.h"

#include "llvm/BinaryFormat/Dwarf.h"

#include <algorithm>
#include <limits>

namespace joyeer::llvmbackend::native {

void Builder::initializeDebug() {
    debug = std::make_unique<llvm::DIBuilder>(module());
    const auto count = source.sourceFiles.empty() ? 1 : source.sourceFiles.size();
    for (size_t index = 0; index < count; ++index) {
        const auto& file = *source.sourceFile(static_cast<uint32_t>(index));
        debugFiles.push_back(debug->createFile(file.fileName, file.directory));
    }
    debug->createCompileUnit(llvm::dwarf::DW_LANG_C, debugFiles.front(), "Joyeer",
            options.optimizationLevel != OptimizationLevel::O0, "", 0, "",
            fullDebug() ? llvm::DICompileUnit::FullDebug : llvm::DICompileUnit::LineTablesOnly);
    module().addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
    if (options.debugInfoFormat == DebugInfoFormat::codeView) {
        module().addModuleFlag(llvm::Module::Warning, "CodeView", 1);
    } else {
        module().addModuleFlag(llvm::Module::Max, "Dwarf Version", 4);
    }
}

std::pair<uint32_t, uint32_t> Builder::position(SourceSpan span) {
    const auto& starts = source.sourceFile(span.sourceId)->lineStarts;
    const auto upper = std::upper_bound(starts.begin(), starts.end(), span.offset);
    const auto index = upper == starts.begin() ? size_t{0}
            : static_cast<size_t>(std::distance(starts.begin(), upper) - 1);
    const auto rawLine = static_cast<uint64_t>(index) + 1;
    const auto rawColumn = static_cast<uint64_t>(span.offset) - starts[index] + 1;
    auto line = rawLine <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(rawLine) : 0u;
    auto column = rawColumn <= std::numeric_limits<uint16_t>::max() ? static_cast<uint32_t>(rawColumn) : 0u;
    // CodeView reserves several line numbers; column zero is LLVM's unknown
    // position, preferable to truncating byte coordinates into a false location.
    if (options.debugInfoFormat == DebugInfoFormat::codeView &&
            (line > 0x00ffffffu || line == 0x00feefeeu || line == 0x00f00f00u)) {
        line = 0;
        column = 0;
    }
    return {line, column};
}

llvm::DIType* Builder::debugType(ir::TypeId id) {
    if (!fullDebug()) return nullptr;
    if (const auto found = debugTypes.find(id); found != debugTypes.end()) return found->second;
    const auto& value = type(id);
    llvm::DIType* result = nullptr;
    switch (value.kind) {
        case typing::TypeKind::integer:
            result = debug->createBasicType("Int", 64, llvm::dwarf::DW_ATE_signed); break;
        case typing::TypeKind::boolean:
            result = debug->createBasicType("Bool", 8, llvm::dwarf::DW_ATE_boolean); break;
        case typing::TypeKind::uint8:
            result = debug->createBasicType("UInt8", 8, llvm::dwarf::DW_ATE_unsigned_char); break;
        case typing::TypeKind::never: return nullptr;
        default: {
            const auto valueLayout = value.kind == typing::TypeKind::any ? Layout{8, 8} : layout(id);
            auto* file = value.declarationSpan.has_value()
                    ? debugFiles.at(value.declarationSpan->sourceId) : debugFiles.front();
            result = debug->createStructType(nullptr, value.name, file, 0, valueLayout.size * 8,
                    0, llvm::DINode::FlagZero, nullptr, debug->getOrCreateArray({}));
            break;
        }
    }
    debugTypes.emplace(id, result);
    return result;
}

llvm::DILocalScope* Builder::debugScope(ir::DebugScopeId id) {
    if (const auto found = debugScopes.find(id); found != debugScopes.end()) return found->second;
    const auto found = sourceDebugScopes.find(id);
    if (found == sourceDebugScopes.end() || !found->second->parent.has_value()) return nullptr;
    const auto& scope = *found->second;
    auto* parent = debugScope(*scope.parent);
    if (parent == nullptr) return nullptr;
    const auto [line, column] = position(scope.span);
    auto* result = debug->createLexicalBlock(parent, debugFiles.at(scope.span.sourceId), line, column);
    debugScopes.emplace(id, result);
    return result;
}

llvm::DILocalScope* Builder::locationScope(std::optional<ir::DebugScopeId> id, SourceSpan span) {
    llvm::DILocalScope* scope = subprogram;
    auto sourceId = currentFunction != nullptr && currentFunction->debugLocation.has_value()
            ? currentFunction->debugLocation->span.sourceId : 0u;
    if (fullDebug() && id.has_value()) {
        scope = debugScope(*id);
        const auto found = sourceDebugScopes.find(*id);
        if (found != sourceDebugScopes.end()) sourceId = found->second->span.sourceId;
    }
    if (scope == nullptr || sourceId == span.sourceId) return scope;
    // A lexical scope can contain instructions from another source file.
    // Keep its lexical parent while switching only the file attribution.
    const auto key = std::make_pair(scope, span.sourceId);
    if (const auto found = debugFileScopes.find(key); found != debugFileScopes.end()) return found->second;
    auto* result = debug->createLexicalBlockFile(scope, debugFiles.at(span.sourceId), 0);
    debugFileScopes.emplace(key, result);
    return result;
}

llvm::DILocation* Builder::location(const std::optional<ir::DebugLocation>& value) {
    if (!debug || !subprogram || !value.has_value() || value->implicitCode) return nullptr;
    auto* scope = locationScope(value->scope, value->span);
    if (scope == nullptr) return nullptr;
    const auto [line, column] = position(value->span);
    return llvm::DILocation::get(context(), line, column, scope);
}

void Builder::functionDebug(const ir::Function& function) {
    subprogram = nullptr;
    if (!debug || !function.debugLocation.has_value()) return;
    std::vector<llvm::Metadata*> signature;
    if (fullDebug()) {
        signature.push_back(function.returnsValue ? debugType(function.resultType) : nullptr);
        for (const auto& parameter : function.parameters) signature.push_back(debugType(parameter.value.type));
    }
    auto* signatureType = debug->createSubroutineType(debug->getOrCreateTypeArray(signature));
    const auto span = function.debugLocation->span;
    const auto [line, column] = position(span);
    static_cast<void>(column);
    auto flags = llvm::DISubprogram::SPFlagDefinition;
    if (options.optimizationLevel != OptimizationLevel::O0) flags |= llvm::DISubprogram::SPFlagOptimized;
    subprogram = debug->createFunction(debugFiles.at(span.sourceId), function.name,
            activeFunction->getName(), debugFiles.at(span.sourceId), line, signatureType,
            line, llvm::DINode::FlagZero, flags);
    activeFunction->setSubprogram(subprogram);
    if (function.debugScope.has_value()) debugScopes.emplace(*function.debugScope, subprogram);
}

bool Builder::bindDebug(const ir::DebugVariableBinding& binding) {
    if (!fullDebug()) return true;
    const auto found = sourceDebugVariables.find(binding.variable);
    if (found == sourceDebugVariables.end()) return false;
    const auto& variable = *found->second;
    auto* scope = debugScope(variable.scope);
    auto* variableType = debugType(variable.type);
    if (scope == nullptr || variableType == nullptr) return false;
    auto* metadata = debugVariables[binding.variable];
    const auto [line, column] = position(variable.span);
    if (metadata == nullptr) {
        metadata = variable.parameterIndex.has_value()
                ? debug->createParameterVariable(scope, variable.name, *variable.parameterIndex,
                        debugFiles.at(variable.span.sourceId), line, variableType)
                : debug->createAutoVariable(scope, variable.name,
                        debugFiles.at(variable.span.sourceId), line, variableType);
        debugVariables[binding.variable] = metadata;
    }
    auto* declarationScope = locationScope(variable.scope, variable.span);
    if (declarationScope == nullptr || !operands.contains(binding.address)) return false;
    debug->insertDeclare(operand(binding.address), metadata, debug->createExpression(),
            llvm::DILocation::get(context(), line, column, declarationScope), builder.GetInsertBlock());
    return true;
}

} // namespace joyeer::llvmbackend::native