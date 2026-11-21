#include "irlowering.h"
#include "constants.h"
#include "compiler/frontend/intrinsics.h"
#include "compiler/frontend/sourcefile.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdio>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace joyeer::lowering {

namespace {

// An opcode and its modifier (decision 0028): what one instruction does. Converts from an opcode, a comparison
// predicate or a conversion mode.
struct Operation {
    ir::Opcode opcode;
    uint8_t modifier = 0;

    Operation(ir::Opcode opcode) : opcode(opcode) {}
    Operation(ir::ComparePredicate predicate)
        : opcode(ir::Opcode::compare), modifier(static_cast<uint8_t>(predicate)) {}
    Operation(ir::ConversionMode mode)
        : opcode(ir::Opcode::convert), modifier(static_cast<uint8_t>(mode)) {}
};

class DebugScopeGuard {
public:
    DebugScopeGuard(
            std::optional<ir::DebugScopeId>& current,
            std::optional<ir::DebugScopeId> next)
        : current(current), previous(current) {
        if (next.has_value()) current = next;
    }

    ~DebugScopeGuard() {
        current = previous;
    }

    DebugScopeGuard(const DebugScopeGuard&) = delete;
    DebugScopeGuard& operator=(const DebugScopeGuard&) = delete;

private:
    std::optional<ir::DebugScopeId>& current;
    std::optional<ir::DebugScopeId> previous;
};

class Builder {
public:
    Result build(
            const typing::TypeCheckedModel::Ptr& checkedModel,
            std::string sourceName,
            std::optional<ir::SourceInfo> sourceInfo,
            std::vector<ir::SourceInfo> sources) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        module = std::make_shared<ir::Module>();
        module->sourceName = std::move(sourceName);
        module->sourceInfo = std::move(sourceInfo);
        module->sourceFiles = std::move(sources);
        if (!module->sourceFiles.empty() || module->sourceInfo.has_value()) {
            for (const auto& prelude : model->semanticModel()->preludeSources()) {
                if (prelude == nullptr || prelude->tokens.empty()) continue;
                if (module->sourceFiles.empty()) module->sourceFiles.push_back(*module->sourceInfo);
                const auto sourceId = prelude->tokens.front()->span.sourceId;
                // The driver supplies these already; a direct caller gets them appended in identity order.
                if (module->sourceFiles.size() == sourceId) {
                    module->sourceFiles.push_back(ir::SourceInfo {
                        prelude->getAbstractPath().filename().string(), prelude->getParentFolder(),
                        prelude->content.size(), prelude->lineStarts });
                }
            }
        }

        snapshotTypes();
        collectAggregateDefinitions();
        destruction.emplace(*module);
        collectFunctions();
        lowerFunctions();

        if (model->types().size() != module->types.size()) {
            report(DiagnosticId::missingType, {},
                   "an instance needed a type the type checker did not intern before lowering");
        }
        if (diagnostics.empty()) {
            const auto verification = ir::Verifier().verify(*module);
            for (const auto& error : verification.errors) {
                report(
                        DiagnosticId::verificationFailed,
                        ir::verificationErrorSpan(*module, error),
                        std::string(ir::verificationErrorName(error.id)) +
                                ": " + error.message);
            }
        }
        return Result { module, std::move(diagnostics) };
    }

private:
    enum class ValueOwnership {
        trivial,
        borrowed,
        owned,
    };

    enum class CleanupKind {
        storage,
        temporary,
    };

    struct Cleanup {
        CleanupKind kind;
        ir::Value value;
    };

    struct ScopeFrame {
        std::vector<Cleanup> cleanups;
    };

    struct LoopContext {
        ir::BlockId header;
        ir::BlockId exit;
        size_t scopeDepth;
        bool hasBreak = false;
    };

    typing::TypeCheckedModel::Ptr model;
    std::shared_ptr<ir::Module> module;
    std::optional<ir::DestructionAnalysis> destruction;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<semantic::SymbolId, ir::FunctionId> functions;
    // String roles and contracts are emitted only when generated code uses them.
    std::unordered_map<semantic::SymbolId, syntax::FunctionDeclSyntax::Ptr> deferredPreludeFunctions;
    std::vector<std::pair<syntax::FunctionDeclSyntax::Ptr, ir::FunctionId>> pendingPreludeBodies;
    // Top-level `let` constants, evaluated at compile time. Scalars and strings
    // fold into each use; arrays become shared module constants.
    std::unordered_map<semantic::SymbolId, syntax::BindingDeclSyntax::Ptr> constantDeclarations;
    std::vector<semantic::SymbolId> constantOrder;
    // Every top-level binding, valid or not, so a rejected one is reported only once.
    std::unordered_set<semantic::SymbolId> topLevelBindings;
    std::unordered_set<semantic::SymbolId> failedConstants;
    std::unordered_map<semantic::SymbolId, uint32_t> constantIds;
    std::optional<ConstantEvaluator> constantEvaluator;
    // Keyed by `instanceKey`, so each instance of a generic function has its own scopes and variables.
    std::unordered_map<uint64_t, ir::DebugScopeId> debugScopes;
    std::unordered_map<uint64_t, ir::DebugVariableId> debugVariables;
    // The instance of a generic function being collected or lowered (decision 0021): every type
    // lowering reads is substituted with its arguments, and requirement calls reach witnesses.
    const typing::Instance* currentInstance = nullptr;
    std::optional<size_t> currentInstanceIndex;
    std::vector<ir::FunctionId> instanceFunctions;
    // Per generic function: spans of the expressions whose type is noncopyable in the generic body
    // (see prepareOwned).
    std::unordered_map<semantic::SymbolId, std::unordered_set<uint64_t>> genericNoncopyableSpans;
    std::unordered_map<semantic::SymbolId, ir::Value> slots;
    // Parameters read as their own values, with no slot: those of the String role functions (decision 0028 stage
    // 3), which are borrowed, never addressed or assigned, and always inlined, so -O0 keeps no spill of them.
    std::unordered_map<semantic::SymbolId, ir::Value> boundParameters;
    std::unordered_map<ir::ValueId, ValueOwnership> ownership;
    std::unordered_set<ir::ValueId> liveOwnedTemporaries;
    std::vector<ScopeFrame> scopes;
    std::vector<LoopContext> loops;
    ir::FunctionId currentFunctionId = ir::invalidFunctionId;
    ir::BlockId currentBlockId = ir::invalidBlockId;
    ir::ValueId nextValue = 0;
    std::optional<ir::DebugScopeId> currentDebugScope;

    void report(DiagnosticId id, SourceSpan span, std::string message) {
        diagnostics.push_back(Diagnostic { id, span, std::move(message) });
    }

    uint64_t instanceKey(uint32_t id) const {
        const uint64_t slot = currentInstanceIndex.has_value() ? *currentInstanceIndex + 1 : 0;
        return (slot << 32U) | id;
    }

    // The type lowering uses for a node or symbol: in an instance, with the type arguments
    // substituted. Every concrete type was interned by the type checker, so this never grows the
    // type table after it was snapshotted (checked at the end of `build`).
    typing::TypeId substituted(typing::TypeId type) const {
        if (currentInstance == nullptr) return type;
        return model->types().substitute(type, currentInstance->substitution);
    }

    std::optional<typing::TypeId> typeOf(const syntax::NodePtr& node) const {
        const auto type = model->typeOf(node);
        return type.has_value() ? std::optional<typing::TypeId>(substituted(*type)) : std::nullopt;
    }

    std::optional<typing::TypeId> typeOf(semantic::SymbolId symbol) const {
        const auto type = model->typeOf(symbol);
        return type.has_value() ? std::optional<typing::TypeId>(substituted(*type)) : std::nullopt;
    }

    // Whether a value is copied or moved is decided by the generic body (decision 0021): in an
    // instance, `node`'s unsubstituted type decides, not the concrete one.
    bool copyableInBody(const syntax::NodePtr& node, typing::TypeId concrete) const {
        if (currentInstance != nullptr && node != nullptr) {
            if (const auto raw = model->typeOf(node)) return model->isCopyable(*raw);
        }
        return model->isCopyable(concrete);
    }

    static uint64_t spanKey(SourceSpan span) {
        return (static_cast<uint64_t>(span.sourceId) << 48U) ^ (static_cast<uint64_t>(span.offset) << 20U) ^
                span.length;
    }

    // Spans in a generic function's body whose expressions have a type that mentions a type
    // parameter and is noncopyable there. A copy at one of them is a type-checking gap.
    const std::unordered_set<uint64_t>& noncopyableSpans(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        auto& spans = genericNoncopyableSpans[symbol.value_or(semantic::invalidSymbolId)];
        if (!spans.empty() || !symbol.has_value()) return spans;
        const auto within = declaration->span;
        for (const auto& [node, type] : model->nodeTypeTable()) {
            const auto span = model->semanticModel()->node(node)->span;
            if (span.sourceId != within.sourceId || span.offset < within.offset ||
                span.offset + span.length > within.offset + within.length) {
                continue;
            }
            if (model->types().mentionsTypeParameter(type) && !model->isCopyable(type)) spans.insert(spanKey(span));
        }
        spans.insert(0);
        return spans;
    }
    const std::unordered_set<uint64_t>* currentNoncopyableSpans = nullptr;

    std::optional<ir::DebugScopeId> ensureDebugScope(
            semantic::ScopeId semanticScopeId,
            ir::FunctionId functionId) {
        if (module->sourceFile(0) == nullptr) return std::nullopt;
        const auto existing = debugScopes.find(instanceKey(semanticScopeId));
        if (existing != debugScopes.end()) return existing->second;

        const auto* semanticScope = model->semanticModel()->scope(semanticScopeId);
        if (semanticScope == nullptr) return std::nullopt;
        if (semanticScope->kind != semantic::ScopeKind::function &&
            semanticScope->kind != semantic::ScopeKind::block &&
            semanticScope->kind != semantic::ScopeKind::matchArm) {
            return std::nullopt;
        }

        std::optional<ir::DebugScopeId> parent;
        auto semanticParent = semanticScope->parent;
        while (semanticParent.has_value()) {
            parent = ensureDebugScope(*semanticParent, functionId);
            if (parent.has_value()) break;
            const auto* parentScope = model->semanticModel()->scope(*semanticParent);
            semanticParent = parentScope == nullptr
                    ? std::optional<semantic::ScopeId>()
                    : parentScope->parent;
        }

        SourceSpan span;
        if (semanticScope->owner.has_value()) {
            span = model->semanticModel()->node(*semanticScope->owner)->span;
        }
        const auto id = static_cast<ir::DebugScopeId>(module->debugScopes.size());
        module->debugScopes.push_back(ir::DebugScope {
            id,
            semanticScope->kind == semantic::ScopeKind::function
                    ? ir::DebugScopeKind::function
                    : ir::DebugScopeKind::lexicalBlock,
            functionId,
            parent,
            semanticScopeId,
            span,
        });
        debugScopes.emplace(instanceKey(semanticScopeId), id);
        return id;
    }

    std::optional<ir::DebugScopeId> debugScopeForNode(
            const syntax::NodePtr& node,
            ir::FunctionId functionId) {
        if (node == nullptr) return std::nullopt;
        const auto semanticScope = model->semanticModel()->introducedScope(node);
        if (!semanticScope.has_value()) return std::nullopt;
        return ensureDebugScope(*semanticScope, functionId);
    }

    std::optional<ir::DebugVariableId> ensureDebugVariable(
            semantic::SymbolId symbolId,
            ir::FunctionId functionId,
            std::optional<uint32_t> parameterIndex = std::nullopt) {
        if (module->sourceFile(0) == nullptr) return std::nullopt;
        const auto existing = debugVariables.find(instanceKey(symbolId));
        if (existing != debugVariables.end()) return existing->second;

        const auto* symbol = model->semanticModel()->symbol(symbolId);
        const auto type = typeOf(symbolId);
        if (symbol == nullptr || !type.has_value()) return std::nullopt;
        ir::DebugVariableKind kind;
        switch (symbol->kind) {
            case semantic::SymbolKind::parameter:
                kind = ir::DebugVariableKind::parameter;
                break;
            case semantic::SymbolKind::binding:
                kind = ir::DebugVariableKind::local;
                break;
            case semantic::SymbolKind::patternBinding:
                kind = ir::DebugVariableKind::patternBinding;
                break;
            default:
                return std::nullopt;
        }
        const auto scope = ensureDebugScope(symbol->ownerScope, functionId);
        if (!scope.has_value()) return std::nullopt;

        const auto id = static_cast<ir::DebugVariableId>(module->debugVariables.size());
        module->debugVariables.push_back(ir::DebugVariable {
            id,
            kind,
            functionId,
            *scope,
            symbolId,
            symbol->name,
            *type,
            symbol->span,
            parameterIndex,
            symbol->isMutable,
        });
        debugVariables.emplace(instanceKey(symbolId), id);
        return id;
    }

    bool requiresDestroy(typing::TypeId type) const {
        return destruction->requiresDestruction(type);
    }

    void pushScope() {
        scopes.push_back(ScopeFrame {});
    }

    void registerOwnedStorage(ir::Value address) {
        assert(!scopes.empty());
        if (requiresDestroy(address.type)) {
            scopes.back().cleanups.push_back(Cleanup { CleanupKind::storage, address });
        }
    }

    void recordValue(ir::Value value, ValueOwnership valueOwnership) {
        if (!requiresDestroy(value.type)) valueOwnership = ValueOwnership::trivial;
        ownership[value.id] = valueOwnership;
        if (valueOwnership == ValueOwnership::owned) {
            assert(!scopes.empty());
            liveOwnedTemporaries.insert(value.id);
            scopes.back().cleanups.push_back(Cleanup { CleanupKind::temporary, value });
        }
    }

    ValueOwnership ownershipOf(ir::Value value) const {
        if (!requiresDestroy(value.type)) return ValueOwnership::trivial;
        const auto found = ownership.find(value.id);
        return found == ownership.end() ? ValueOwnership::borrowed : found->second;
    }

    void adoptInParentScope(ir::Value value) {
        if (!requiresDestroy(value.type)) return;
        assert(!scopes.empty());
        liveOwnedTemporaries.insert(value.id);
        ownership[value.id] = ValueOwnership::owned;
        scopes.back().cleanups.push_back(Cleanup { CleanupKind::temporary, value });
    }

    std::optional<ir::Value> prepareOwned(ir::Value value, SourceSpan span) {
        if (!requiresDestroy(value.type)) return value;
        if (ownershipOf(value) == ValueOwnership::owned) {
            if (!liveOwnedTemporaries.contains(value.id)) {
                report(
                        DiagnosticId::ownershipViolation,
                        span,
                        "owned value is used after its ownership was transferred");
                return std::nullopt;
            }
            return value;
        }
        // In an instance, a copy of a value whose type is noncopyable in the generic body is a
        // type-checking gap, whatever the concrete type is (decision 0021).
        if (currentNoncopyableSpans != nullptr && currentNoncopyableSpans->contains(spanKey(span))) {
            report(
                    DiagnosticId::ownershipViolation,
                    span,
                    "a value whose type is noncopyable in the generic body would be copied in '" +
                            currentInstance->name + "'");
            return std::nullopt;
        }
        // Type checking rejects every source-level copy of a noncopyable value, so reaching
        // here means a lowering path moves where it should not copy.
        if (!model->isCopyable(value.type)) {
            report(
                    DiagnosticId::ownershipViolation,
                    span,
                    "a noncopyable value would be copied");
            return std::nullopt;
        }
        return emitValue(
                ir::Opcode::copyValue,
                value.type,
                ir::ValueCategory::value,
                { value.id },
                span);
    }

    std::optional<ir::Value> acquireOwned(ir::Value value, SourceSpan span) {
        const auto prepared = prepareOwned(value, span);
        if (prepared.has_value()) liveOwnedTemporaries.erase(prepared->id);
        return prepared;
    }

    // Moves the value out of the place `expression` names, leaving the place empty.
    std::optional<ir::Value> lowerTake(const syntax::ExprPtr& expression) {
        const auto address = lowerAddress(expression);
        if (!address.has_value()) return std::nullopt;
        return emitValue(
                ir::Opcode::take,
                address->type,
                ir::ValueCategory::value,
                { address->id },
                expression->span);
    }

    static bool movesArgument(const syntax::CallArgumentSyntax& argument) {
        return argument.accessMarker != nullptr && argument.accessMarker->kind == kwConsume;
    }

    std::optional<ir::Value> prepareOperand(
            const syntax::ExprPtr& expression,
            typing::TypeId destination,
            bool moved = false) {
        auto value = moved && isAddressable(expression)
                ? lowerTake(expression)
                : lowerExpression(expression);
        if (!value.has_value()) return std::nullopt;
        value = coerce(*value, destination, expression->span);
        if (!value.has_value()) return std::nullopt;
        return prepareOwned(*value, expression->span);
    }

    bool transferOperands(
            ir::Instruction& instruction,
            const std::vector<ir::Value>& operands,
            SourceSpan span) {
        for (const auto& operand : operands) {
            const auto value = acquireOwned(operand, span);
            if (!value.has_value()) return false;
            instruction.operands.push_back(value->id);
        }
        return true;
    }

    void emitRawStore(
            ir::Value value,
            ir::Value address,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt,
            bool implicitCode = false,
            std::optional<ir::DebugVariableId> debugVariable = std::nullopt) {
        auto instruction = makeInstruction(ir::Opcode::store, span, implicitCode);
        instruction.operands = { value.id, address.id };
        instruction.symbol = symbol;
        if (debugVariable.has_value()) {
            instruction.debugVariableBindings.push_back(ir::DebugVariableBinding {
                *debugVariable,
                address.id,
            });
        }
        emit(std::move(instruction));
    }

    void emitDestroyStorage(ir::Value address, SourceSpan span) {
        if (!requiresDestroy(address.type)) return;
        auto instruction = makeInstruction(ir::Opcode::destroy, span, true);
        instruction.operands = { address.id };
        emit(std::move(instruction));
    }

    void emitDestroyTemporary(ir::Value value, SourceSpan span) {
        const auto address = emitValue(
                ir::Opcode::stackAllocate,
                value.type,
                ir::ValueCategory::address,
                {},
            span,
            std::nullopt,
            true);
        emitRawStore(value, address, span, std::nullopt, true);
        emitDestroyStorage(address, span);
    }

    void emitScopeCleanup(const ScopeFrame& scope, SourceSpan span, bool mutateState) {
        for (auto cleanup = scope.cleanups.rbegin(); cleanup != scope.cleanups.rend(); ++cleanup) {
            if (cleanup->kind == CleanupKind::storage) {
                emitDestroyStorage(cleanup->value, span);
                continue;
            }
            if (!liveOwnedTemporaries.contains(cleanup->value.id)) continue;
            emitDestroyTemporary(cleanup->value, span);
            if (mutateState) liveOwnedTemporaries.erase(cleanup->value.id);
        }
    }

    void cleanupAndPopScope(SourceSpan span) {
        assert(!scopes.empty());
        emitScopeCleanup(scopes.back(), span, true);
        scopes.pop_back();
    }

    void discardScopeAfterTerminator() {
        assert(!scopes.empty());
        scopes.pop_back();
    }

    void emitAllScopeCleanupForReturn(SourceSpan span) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            emitScopeCleanup(*scope, span, false);
        }
    }

    void snapshotTypes() {
        const auto& types = model->types();
        for (typing::TypeId id = 0; id < types.size(); ++id) {
            const auto* type = types.type(id);
            assert(type != nullptr);
            // A type that mentions a type parameter exists only in a generic body; IR sees each
            // instance's concrete types, so it is an error type here and the verifier rejects any use.
            if (types.mentionsTypeParameter(id)) {
                module->types.push_back(ir::TypeName {
                    id, types.displayName(id), typing::TypeKind::error, semantic::invalidSymbolId, {}, {} });
                continue;
            }
            module->types.push_back(ir::TypeName {
                id,
                types.displayName(id),
                type->kind,
                type->symbol,
                type->arguments,
                type->effects,
            });
            const auto* symbol = model->semanticModel()->symbol(type->symbol);
            if (symbol != nullptr && symbol->declaration.has_value()) {
                module->types.back().declarationSpan = symbol->span;
            }
        }
    }

    void collectAggregateDefinitions() {
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.compilationRoot();
        if (root != nullptr) {
            for (const auto& item : root->items) {
                if (item->kind == syntax::Kind::structDecl) {
                    const auto declaration = std::static_pointer_cast<syntax::StructDeclSyntax>(item);
                    if (declaration->genericParameters.empty()) collectStructure(declaration);
                } else if (item->kind == syntax::Kind::enumDecl) {
                    const auto declaration = std::static_pointer_cast<syntax::EnumDeclSyntax>(item);
                    if (declaration->genericParameters.empty()) collectEnumeration(declaration);
                }
            }
            collectGenericTypeInstances(*root);
        }
        for (typing::TypeId id = 0; id < model->types().size(); ++id) {
            const auto* type = model->types().type(id);
            const auto* symbol = type == nullptr
                    ? nullptr
                    : semanticModel.symbol(type->symbol);
            if (type != nullptr && !model->types().mentionsTypeParameter(id) &&
                type->kind == typing::TypeKind::enumeration &&
                symbol != nullptr &&
                symbol->kind == semantic::SymbolKind::builtinType) {
                collectBuiltinEnumeration(*type);
            }
        }
    }

    // One definition per concrete instance of a generic struct or enum (spec 20.12): the type
    // checker interned each instance and its member types before lowering.
    void collectGenericTypeInstances(const syntax::SourceFileSyntax& root) {
        const auto& semanticModel = *model->semanticModel();
        for (const auto& item : root.items) {
            const bool isStructure = item->kind == syntax::Kind::structDecl;
            if (!isStructure && item->kind != syntax::Kind::enumDecl) continue;
            const auto symbol = semanticModel.declaredSymbol(item);
            if (!symbol.has_value() || model->genericParameters(*symbol) == nullptr) continue;
            for (typing::TypeId id = 0; id < model->types().size(); ++id) {
                const auto* type = model->types().type(id);
                if (type == nullptr || type->symbol != *symbol || type->arguments.empty() ||
                    model->types().mentionsTypeParameter(id)) {
                    continue;
                }
                if (isStructure) {
                    collectStructure(std::static_pointer_cast<syntax::StructDeclSyntax>(item), id);
                } else {
                    collectEnumeration(std::static_pointer_cast<syntax::EnumDeclSyntax>(item), id);
                }
            }
        }
    }

    // `instance` is the concrete type of a generic declaration's instance; its field types are the
    // declared ones with the instance's arguments substituted.
    void collectStructure(
            const syntax::StructDeclSyntax::Ptr& declaration,
            std::optional<typing::TypeId> instance = std::nullopt) {
        const auto& semanticModel = *model->semanticModel();
        const auto symbol = semanticModel.declaredSymbol(declaration);
        const auto type = instance.has_value()
                ? instance
                : symbol.has_value() ? typeOf(*symbol) : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) return;

        ir::StructureDefinition structure;
        structure.symbol = *symbol;
        structure.type = *type;
        structure.name = declaration->name == nullptr
                ? std::string()
                : declaration->name->rawValue;
        if (instance.has_value()) structure.name = model->types().displayName(*instance);
        for (const auto& field : declaration->fields) {
            const auto fieldSymbol = semanticModel.declaredSymbol(field);
            auto fieldType = fieldSymbol.has_value()
                    ? typeOf(*fieldSymbol)
                    : std::optional<typing::TypeId>();
            if (!fieldSymbol.has_value() || !fieldType.has_value()) continue;
            if (instance.has_value()) fieldType = model->memberTypeOf(*instance, *fieldType);
            const auto* semanticField = semanticModel.symbol(*fieldSymbol);
            structure.fields.push_back(ir::FieldDefinition {
                *fieldSymbol,
                field->name == nullptr ? std::string() : field->name->rawValue,
                *fieldType,
                semanticField != nullptr && semanticField->isMutable,
            });
        }
        // The library array and dictionary roles have no live flag: their zero value is destroyable (decisions
        // 0025, 0027). The dictionary entry is never a value and declares no `deinit`.
        const auto* structureSymbol = semanticModel.symbol(*symbol);
        const auto libraryRole = structureSymbol != nullptr ? structureSymbol->libraryRole : semantic::LibraryRole::none;
        structure.role = libraryRole == semantic::LibraryRole::arrayType ? ir::StructureRole::array
                : libraryRole == semantic::LibraryRole::dictType ? ir::StructureRole::dict
                : libraryRole == semantic::LibraryRole::dictEntry ? ir::StructureRole::dictEntry
                                                                  : ir::StructureRole::none;
        if (declaration->deinitializer != nullptr && structure.role == ir::StructureRole::none) {
            // See ir::StructureDefinition::deinitializer: the live flag is always last.
            structure.fields.push_back(ir::FieldDefinition {
                semantic::invalidSymbolId,
                ir::liveFieldName,
                model->types().boolType(),
                false,
            });
        }
        module->structures.push_back(std::move(structure));
    }

    void collectEnumeration(
            const syntax::EnumDeclSyntax::Ptr& declaration,
            std::optional<typing::TypeId> instance = std::nullopt) {
        const auto& semanticModel = *model->semanticModel();
        const auto symbol = semanticModel.declaredSymbol(declaration);
        const auto type = instance.has_value()
                ? instance
                : symbol.has_value() ? typeOf(*symbol) : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) return;

        ir::EnumerationDefinition enumeration;
        enumeration.symbol = *symbol;
        enumeration.type = *type;
        enumeration.name = declaration->name == nullptr
                ? std::string()
                : declaration->name->rawValue;
        if (instance.has_value()) enumeration.name = model->types().displayName(*instance);
        for (const auto& enumCase : declaration->cases) {
            const auto caseSymbol = semanticModel.declaredSymbol(enumCase);
            if (!caseSymbol.has_value()) continue;
            const auto* signature = model->callable(*caseSymbol);
            std::vector<typing::TypeId> payloads = signature == nullptr
                    ? std::vector<typing::TypeId>() : signature->parameters;
            if (instance.has_value()) {
                for (auto& payload : payloads) payload = model->memberTypeOf(*instance, payload);
            }
            enumeration.cases.push_back(ir::EnumCaseDefinition {
                *caseSymbol,
                enumCase->name == nullptr ? std::string() : enumCase->name->rawValue,
                std::move(payloads),
            });
        }
        if (symbol.has_value()) {
            const auto* declared = semanticModel.symbol(*symbol);
            const auto role = declared == nullptr ? semantic::LibraryRole::none : declared->libraryRole;
            if (role == semantic::LibraryRole::optional || role == semantic::LibraryRole::result) {
                enumeration.role = role == semantic::LibraryRole::optional
                        ? ir::EnumerationRole::optional : ir::EnumerationRole::result;
                // Positional binding (decision 0024): optional's present case is the one with a payload;
                // result's success case is the first. The type checker validated the shape.
                for (uint32_t index = 0; index < enumeration.cases.size(); ++index) {
                    const bool payload = !enumeration.cases[index].payloadTypes.empty();
                    if (role == semantic::LibraryRole::optional) {
                        (payload ? enumeration.firstRoleCase : enumeration.secondRoleCase) = index;
                    } else {
                        (index == 0 ? enumeration.firstRoleCase : enumeration.secondRoleCase) = index;
                    }
                }
            }
        }
        module->enumerations.push_back(std::move(enumeration));
    }

    void collectBuiltinEnumeration(const typing::TypeRecord& type) {
        const auto& semanticModel = *model->semanticModel();
        const auto* symbol = semanticModel.symbol(type.symbol);
        if (symbol == nullptr || !symbol->memberScope.has_value()) return;
        const auto* members = semanticModel.scope(*symbol->memberScope);
        if (members == nullptr) return;

        std::vector<semantic::SymbolId> caseSymbols;
        for (const auto& [name, caseSymbol] : members->values) {
            static_cast<void>(name);
            caseSymbols.push_back(caseSymbol);
        }
        std::sort(caseSymbols.begin(), caseSymbols.end());

        ir::EnumerationDefinition enumeration;
        enumeration.symbol = type.symbol;
        enumeration.type = type.id;
        enumeration.name = model->types().displayName(type.id);
        for (const auto caseSymbol : caseSymbols) {
            const auto* enumCase = semanticModel.symbol(caseSymbol);
            if (enumCase == nullptr) continue;
            std::vector<typing::TypeId> payloadTypes;
            if (type.kind == typing::TypeKind::enumeration &&
                enumCase->callable.has_value()) {
                for (const auto& parameter : enumCase->callable->parameters) {
                    if (!parameter.type.has_value()) continue;
                    const auto payload = model->types().typeForSymbol(*parameter.type);
                    if (payload.has_value()) payloadTypes.push_back(*payload);
                }
            }
            enumeration.cases.push_back(ir::EnumCaseDefinition {
                caseSymbol,
                enumCase->name,
                std::move(payloadTypes),
            });
        }
        module->enumerations.push_back(std::move(enumeration));
    }

    void collectFunctions() {
        collectPrint();
        collectByteConversions();
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.compilationRoot();
        if (root == nullptr) return;
        const auto contractsSource = semanticModel.contractsPreludeSourceId();
        for (const auto& item : root->items) {
            if (item->kind == syntax::Kind::functionDecl) {
                const auto declaration = std::static_pointer_cast<syntax::FunctionDeclSyntax>(item);
                // A generic function has no code of its own; each instance is collected below.
                const auto symbol = semanticModel.declaredSymbol(declaration);
                if (symbol.has_value() && model->genericParameters(*symbol) != nullptr) continue;
                if (symbol.has_value() &&
                    (contractsSource == declaration->span.sourceId || isStringRoleFunction(*symbol))) {
                    deferredPreludeFunctions.emplace(*symbol, declaration);
                    continue;
                }
                collectFunction(declaration);
            } else if (item->kind == syntax::Kind::bindingDecl) {
                collectConstant(std::static_pointer_cast<syntax::BindingDeclSyntax>(item));
            }
        }
        // Methods are functions whose first parameter is the implicit receiver.
        forEachMethod(*root, [this, &semanticModel](const syntax::FunctionDeclSyntax::Ptr& method) {
            // A method of a generic type is collected per instance below (spec 20.12).
            const auto symbol = semanticModel.declaredSymbol(method);
            if (symbol.has_value() && model->genericParameters(*symbol) != nullptr) return;
            collectFunction(method);
        });
        collectInstances(*root);
        linkDeinitializers(*root);
        evaluateConstants();
        // Lowering can collect more declarations without invalidating references to the functions.
        module->functions.reserve(module->functions.size() + deferredPreludeFunctions.size());
    }

    // The collections prelude's String role functions (decision 0028 stage 3) are collected when a String operation
    // first calls one and lowered after every other function, so a module without String operations, such as a
    // runtime core unit, has none of them. Only lowering calls them: prelude code must not call one by name.
    bool isStringRoleFunction(semantic::SymbolId symbol) const {
        const auto* semanticSymbol = model->semanticModel()->symbol(symbol);
        return semanticSymbol != nullptr && model->stringRoleFunction(semanticSymbol->libraryRole) == symbol;
    }

    std::optional<ir::FunctionId> stringRoleFunction(semantic::LibraryRole role) {
        const auto symbol = model->stringRoleFunction(role);
        if (!symbol.has_value()) return std::nullopt;
        return demandFunction(*symbol);
    }

    std::optional<ir::FunctionId> demandFunction(semantic::SymbolId symbol) {
        if (const auto found = functions.find(symbol); found != functions.end()) return found->second;
        const auto declaration = deferredPreludeFunctions.find(symbol);
        if (declaration == deferredPreludeFunctions.end()) return std::nullopt;
        // A non-generic function: collected outside the instance being lowered, if any.
        const auto* instance = currentInstance;
        const auto instanceIndex = currentInstanceIndex;
        currentInstance = nullptr;
        currentInstanceIndex.reset();
        const auto before = module->functions.size();
        // Callers up the stack hold references into the functions: the room reserved after collection must remain.
        assert(module->functions.size() < module->functions.capacity());
        collectFunction(declaration->second);
        currentInstance = instance;
        currentInstanceIndex = instanceIndex;
        if (module->functions.size() == before) return std::nullopt;
        const auto id = static_cast<ir::FunctionId>(before);
        if (!module->functions[id].isExternal) pendingPreludeBodies.emplace_back(declaration->second, id);
        return id;
    }

    // One IR function per instance of a generic function (decision 0021), named for its type
    // arguments and lowered from the generic declaration's own syntax.
    void collectInstances(const syntax::SourceFileSyntax& root) {
        const auto& instances = model->instances();
        instanceFunctions.assign(instances.size(), ir::invalidFunctionId);
        for (size_t index = 0; index < instances.size(); ++index) {
            const auto declaration = genericDeclaration(root, instances[index].function);
            if (declaration == nullptr) continue;
            enterInstance(index);
            const auto before = module->functions.size();
            collectFunction(declaration);
            if (module->functions.size() > before) instanceFunctions[index] = static_cast<ir::FunctionId>(before);
            leaveInstance();
        }
    }

    syntax::FunctionDeclSyntax::Ptr genericDeclaration(
            const syntax::SourceFileSyntax& root, semantic::SymbolId function) const {
        const auto* symbol = model->semanticModel()->symbol(function);
        if (symbol == nullptr || !symbol->declaration.has_value()) return nullptr;
        const auto node = model->semanticModel()->node(*symbol->declaration);
        if (node == nullptr || node->kind != syntax::Kind::functionDecl) return nullptr;
        (void) root;
        return std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
    }

    void enterInstance(size_t index) {
        currentInstanceIndex = index;
        currentInstance = &model->instances()[index];
        currentNoncopyableSpans = nullptr;
    }

    void leaveInstance() {
        currentInstanceIndex.reset();
        currentInstance = nullptr;
        currentNoncopyableSpans = nullptr;
    }

    // A `deinit` is collected as a method (see forEachMethod); the structure it belongs to
    // records which function that is, so destruction can call it.
    void linkDeinitializers(const syntax::SourceFileSyntax& root) {
        const auto& semanticModel = *model->semanticModel();
        for (const auto& item : root.items) {
            if (item->kind != syntax::Kind::structDecl) continue;
            const auto declaration = std::static_pointer_cast<syntax::StructDeclSyntax>(item);
            if (declaration->deinitializer == nullptr) continue;
            const auto structureSymbol = semanticModel.declaredSymbol(declaration);
            const auto deinitSymbol = semanticModel.declaredSymbol(declaration->deinitializer);
            if (deinitSymbol.has_value() && model->genericParameters(*deinitSymbol) != nullptr) {
                // Each instance of a generic struct runs its own instance of the `deinit`.
                for (auto& instance : module->structures) {
                    if (!structureSymbol.has_value() || instance.symbol != *structureSymbol) continue;
                    const auto* record = model->types().type(instance.type);
                    const auto index = record == nullptr ? std::nullopt
                            : model->instanceIndex(*deinitSymbol, record->arguments);
                    if (!index.has_value() || *index >= instanceFunctions.size() ||
                        instanceFunctions[*index] == ir::invalidFunctionId) {
                        report(DiagnosticId::missingSymbol, declaration->deinitializer->span,
                               "'deinit' has no lowered instance for '" + instance.name + "'");
                        continue;
                    }
                    instance.deinitializer = instanceFunctions[*index];
                    const bool isDict = instance.role == ir::StructureRole::dict;
                    if (ir::isInertZero(instance.role)) {
                        const auto element = model->collectionRoleMethod(instance.type,
                                isDict ? semantic::LibraryRole::dictElement : semantic::LibraryRole::arrayElement);
                        const auto elementIndex = element.has_value()
                                ? model->instanceIndex(*element, record->arguments) : std::nullopt;
                        if (elementIndex.has_value() && *elementIndex < instanceFunctions.size() &&
                            instanceFunctions[*elementIndex] != ir::invalidFunctionId) {
                            module->functions[instanceFunctions[*elementIndex]].elementAddressor = true;
                        }
                    }
                    if (!ir::isInertZero(instance.role) || !model->isCopyable(instance.type)) continue;
                    // A copyable library array or dictionary is copied by its `array_copy` or `dict_copy` method.
                    const auto copySymbol = model->collectionRoleMethod(instance.type,
                            isDict ? semantic::LibraryRole::dictCopy : semantic::LibraryRole::arrayCopy);
                    const auto copyIndex = copySymbol.has_value()
                            ? model->instanceIndex(*copySymbol, record->arguments) : std::nullopt;
                    if (!copyIndex.has_value() || *copyIndex >= instanceFunctions.size() ||
                        instanceFunctions[*copyIndex] == ir::invalidFunctionId) {
                        report(DiagnosticId::missingSymbol, declaration->deinitializer->span,
                               "the copy method has no lowered instance for '" + instance.name + "'");
                        continue;
                    }
                    instance.copier = instanceFunctions[*copyIndex];
                }
                continue;
            }
            const auto function = deinitSymbol.has_value()
                    ? functions.find(*deinitSymbol)
                    : functions.end();
            const auto structure = std::find_if(
                    module->structures.begin(),
                    module->structures.end(),
                    [&structureSymbol](const auto& candidate) {
                        return structureSymbol.has_value() && candidate.symbol == *structureSymbol;
                    });
            if (function == functions.end() || structure == module->structures.end()) {
                report(DiagnosticId::missingSymbol, declaration->deinitializer->span,
                       "'deinit' has no lowered function or structure definition");
                continue;
            }
            structure->deinitializer = function->second;
        }
        // A `deinit` makes its struct (and anything holding it) need destruction, so the
        // analysis built from the aggregate definitions alone is out of date.
        destruction.emplace(*module);
    }

    // Methods, plus a struct's `deinit`, which is lowered like a method taking `self`.
    template<class Visitor>
    static void forEachMethod(const syntax::SourceFileSyntax& root, Visitor&& visit) {
        for (const auto& item : root.items) {
            switch (item->kind) {
                case syntax::Kind::structDecl: {
                    const auto structure = std::static_pointer_cast<syntax::StructDeclSyntax>(item);
                    for (const auto& method : structure->methods) visit(method);
                    if (structure->deinitializer != nullptr) visit(structure->deinitializer);
                    break;
                }
                case syntax::Kind::enumDecl:
                    for (const auto& method :
                         std::static_pointer_cast<syntax::EnumDeclSyntax>(item)->methods) {
                        visit(method);
                    }
                    break;
                case syntax::Kind::extensionDecl:
                    for (const auto& method :
                         std::static_pointer_cast<syntax::ExtensionDeclSyntax>(item)->methods) {
                        visit(method);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    bool isConstantType(typing::TypeId id) const {
        const auto* type = model->types().type(id);
        if (type == nullptr) return false;
        if (model->isArrayRoleType(id)) {
            return type->arguments.size() == 1 && isConstantType(type->arguments.front());
        }
        return typing::isNativeScalar(type->kind) || type->kind == typing::TypeKind::string;
    }

    void collectConstant(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        if (!symbol.has_value()) return;
        topLevelBindings.insert(*symbol);
        if (declaration->isMutable()) {
            report(DiagnosticId::invalidConstant, declaration->span,
                    "top-level 'var' is not supported; declare an immutable 'let' constant");
            return;
        }
        if (declaration->annotation == nullptr) {
            report(DiagnosticId::invalidConstant, declaration->span,
                    "a top-level constant requires an explicit type");
            return;
        }
        const auto type = typeOf(*symbol);
        if (declaration->initializer == nullptr || !type.has_value() || !isConstantType(*type)) {
            report(DiagnosticId::invalidConstant, declaration->span,
                    "a top-level constant must be a Bool, number, String, or array of those, "
                    "with an initializer");
            return;
        }
        constantDeclarations.emplace(*symbol, declaration);
        constantOrder.push_back(*symbol);
    }

    void evaluateConstants() {
        if (constantOrder.empty()) return;
        constantEvaluator.emplace(
                model,
                [this](semantic::SymbolId symbol) -> syntax::ExprPtr {
                    const auto found = constantDeclarations.find(symbol);
                    return found == constantDeclarations.end() ? nullptr : found->second->initializer;
                },
                [this](SourceSpan span, std::string message) {
                    report(DiagnosticId::invalidConstant, span, std::move(message));
                });
        // Evaluate every constant, used or not, so an invalid one is always reported.
        for (const auto symbol : constantOrder) {
            const auto& declaration = constantDeclarations.at(symbol);
            const auto value = constantEvaluator->evaluateSymbol(symbol, declaration->span);
            const auto type = typeOf(symbol);
            // Encoding enforces the size limit, so an oversized table fails here once.
            if (!value.has_value() ||
                (model->isArrayRoleType(*type) &&
                 !constantEvaluator->encode(*value, *type, declaration->span).has_value())) {
                failedConstants.insert(symbol);
            }
        }
    }

    std::optional<ir::Value> lowerConstant(semantic::SymbolId symbol, SourceSpan span) {
        const auto type = typeOf(symbol);
        if (!type.has_value()) {
            report(DiagnosticId::missingType, span, "constant has no resolved type");
            return std::nullopt;
        }
        if (failedConstants.contains(symbol)) return std::nullopt;
        const auto value = constantEvaluator->evaluateSymbol(symbol, span);
        if (!value.has_value()) return std::nullopt;

        const auto kind = model->types().type(*type)->kind;
        auto instruction = makeInstruction(ir::Opcode::literal, span);
        if (model->isArrayRoleType(*type)) {
            auto found = constantIds.find(symbol);
            if (found == constantIds.end()) {
                const auto encoded = constantEvaluator->encode(*value, *type, span);
                if (!encoded.has_value()) {
                    failedConstants.insert(symbol);
                    return std::nullopt;
                }
                ir::Constant constant;
                constant.id = static_cast<uint32_t>(module->constants.size());
                constant.symbol = symbol;
                constant.type = *type;
                constant.name = model->semanticModel()->symbol(symbol)->name;
                constant.data = *encoded;
                module->constants.push_back(std::move(constant));
                found = constantIds.emplace(symbol, module->constants.back().id).first;
            }
            instruction.opcode = ir::Opcode::constantValue;
            instruction.integerValue = found->second;
        } else if (kind == typing::TypeKind::string) {
            instruction.opcode = ir::Opcode::stringConstant;
            instruction.text = value->text;
        } else if (typing::isFloating(kind)) {
            instruction.opcode = ir::Opcode::floatingConstant;
            char text[40] = {};
            std::snprintf(text, sizeof(text), kind == typing::TypeKind::float32 ? "%.9g" : "%.17g",
                    value->floating);
            instruction.text = text;
        } else if (kind == typing::TypeKind::boolean) {
            instruction.opcode = ir::Opcode::literal;
            instruction.integerValue = static_cast<int64_t>(value->integer);
        } else {
            const auto width = typing::scalarBitWidth(kind);
            auto bits = value->integer;
            if (typing::isSignedInteger(kind) && width < 64 && ((bits >> (width - 1)) & 1) != 0) {
                bits |= ~((uint64_t{1} << width) - 1);
            }
            instruction.opcode = ir::Opcode::literal;
            instruction.integerValue = std::bit_cast<int64_t>(bits);
        }
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        // Constants are never owned: every use borrows, and an owned use copies.
        recordValue(result, model->isArrayRoleType(*type) || kind == typing::TypeKind::string
                ? ValueOwnership::borrowed : ValueOwnership::trivial);
        return result;
    }

    void collectPrint() {
        const auto& semanticModel = *model->semanticModel();
        const auto* prelude = semanticModel.scope(semanticModel.preludeScope());
        if (prelude == nullptr) return;
        const auto found = prelude->values.find("print");
        if (found == prelude->values.end()) return;
        const auto* signature = model->callable(found->second);
        if (signature == nullptr || signature->parameters.size() != 1) return;

        ir::Function function;
        function.id = static_cast<ir::FunctionId>(module->functions.size());
        function.symbol = found->second;
        function.name = "print";
        function.parameters.push_back(ir::Parameter {
            ir::Value { 0, signature->parameters[0], ir::ValueCategory::value },
            std::nullopt,
            "value",
            false,
            {},
            true,
        });
        function.resultType = signature->result;
        function.isExternal = true;
        functions.emplace(found->second, function.id);
        module->functions.push_back(std::move(function));
    }

    void collectByteConversions() {
        const auto& semanticModel = *model->semanticModel();
        const auto* prelude = semanticModel.scope(semanticModel.preludeScope());
        if (prelude == nullptr) return;
        for (const auto* name : { "byteToInt", "byteToString" }) {
            const auto found = prelude->values.find(name);
            if (found == prelude->values.end()) continue;
            const auto* signature = model->callable(found->second);
            if (signature == nullptr || signature->parameters.size() != 1) continue;

            ir::Function function;
            function.id = static_cast<ir::FunctionId>(module->functions.size());
            function.symbol = found->second;
            function.name = name;
            function.parameters.push_back(ir::Parameter {
                ir::Value { 0, signature->parameters[0], ir::ValueCategory::value },
                std::nullopt,
                "value",
                false,
                {},
            });
            function.resultType = signature->result;
            function.returnsValue = true;
            function.isExternal = true;
            functions.emplace(found->second, function.id);
            module->functions.push_back(std::move(function));
        }
    }

    void collectFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        const auto& semanticModel = *model->semanticModel();
        const auto symbol = semanticModel.declaredSymbol(declaration);
        if (!symbol.has_value()) {
            report(DiagnosticId::missingSymbol, declaration->span, "function has no symbol");
            return;
        }
        const auto* signature = model->callable(*symbol);
        if (signature == nullptr) {
            report(
                    DiagnosticId::missingType,
                    declaration->span,
                    "function has no typed callable signature");
            return;
        }

        ir::Function function;
        function.id = static_cast<ir::FunctionId>(module->functions.size());
        function.symbol = symbol;
        const auto* semanticSymbol = semanticModel.symbol(*symbol);
        function.name = semanticSymbol == nullptr ? std::string() : semanticSymbol->name;
        if (semanticSymbol != nullptr && semanticSymbol->kind == semantic::SymbolKind::method &&
            semanticSymbol->containingSymbol.has_value()) {
            // Qualified, so a method can never be mistaken for the entry `main`.
            const auto* owner = semanticModel.symbol(*semanticSymbol->containingSymbol);
            if (owner != nullptr) function.name = owner->name + "." + function.name;
        }
        if (currentInstance != nullptr) function.name = currentInstance->name;
        function.isRootModule = semanticSymbol == nullptr || semanticSymbol->isRootModule;
        // The role prelude is not a runtime unit (it may declare no C bindings), but its library bodies use the
        // layout intrinsics, so the verifier treats them as runtime code (decision 0025).
        function.runtimeContext = semanticModel.isRuntimeSource(declaration->span) ||
                semanticModel.isPreludeSource(declaration->span);
        function.coreContext = semanticModel.isCoreSource(declaration->span);
        if (const auto* binding = model->nativeBinding(*symbol)) {
            function.nativeBinding = *binding;
            function.isExternal = binding->kind == NativeBindingKind::cImport;
        }
        if (module->sourceFile(0) != nullptr && !function.isExternal) {
            function.debugScope = debugScopeForNode(declaration, function.id);
            function.debugLocation = ir::DebugLocation {
                declaration->span,
                false,
                function.debugScope,
            };
        }
        function.resultType = substituted(signature->result);
        const auto role = semanticSymbol->libraryRole;
        function.literalFactory = role == semantic::LibraryRole::arrayLiteral ||
                role == semantic::LibraryRole::dictLiteral;
        // The closed role wrappers (0026 A4, extended to the dictionary by role in 0027); `dict_set` is an owned
        // argument context below, and an addressor is always inlined by the backend.
        function.alwaysInline = function.literalFactory || role == semantic::LibraryRole::arrayCopy ||
                role == semantic::LibraryRole::dictCopy || role == semantic::LibraryRole::dictGet ||
                role == semantic::LibraryRole::stringUtf8 || model->stringRoleFunction(role) == *symbol;
        const auto* owner = semanticSymbol->containingSymbol.has_value()
                ? semanticModel.symbol(*semanticSymbol->containingSymbol) : nullptr;
        if (owner != nullptr && (owner->libraryRole == semantic::LibraryRole::arrayType ||
                                 owner->libraryRole == semantic::LibraryRole::dictType) &&
            owner->declaration.has_value()) {
            const auto node = semanticModel.node(*owner->declaration);
            if (node != nullptr && node->kind == syntax::Kind::structDecl &&
                std::static_pointer_cast<syntax::StructDeclSyntax>(node)->deinitializer == declaration) {
                function.alwaysInline = true;
            }
        }
        for (size_t parameter = 0; parameter < signature->parameters.size(); ++parameter) {
            function.alwaysInline |= model->isOwnedArgumentContext(*symbol, parameter);
        }
        function.returnsValue = function.resultType != model->types().voidType() &&
                function.resultType != model->types().neverType();
        function.entry = function.isExternal ? ir::invalidBlockId : 0;

        for (size_t index = 0; index < declaration->parameters.size(); ++index) {
            const auto& parameter = declaration->parameters[index];
            const auto parameterEffect = parameter->accessEffect();
            const auto isAddressProjection =
                    parameterEffect == syntax::AccessEffect::inout ||
                    parameterEffect == syntax::AccessEffect::initializing;
            const auto parameterSymbol = semanticModel.declaredSymbol(parameter);
            const auto parameterType = parameterSymbol.has_value()
                    ? typeOf(*parameterSymbol)
                    : std::optional<typing::TypeId>();
            if (!parameterSymbol.has_value() || !parameterType.has_value()) {
                report(
                        parameterSymbol.has_value()
                                ? DiagnosticId::missingType
                                : DiagnosticId::missingSymbol,
                        parameter->span,
                        "parameter is missing a typed symbol");
                continue;
            }
            function.parameters.push_back(ir::Parameter {
                ir::Value {
                    static_cast<ir::ValueId>(index),
                    *parameterType,
                    isAddressProjection
                            ? ir::ValueCategory::address
                            : ir::ValueCategory::value,
                },
                parameterSymbol,
                parameter->name == nullptr ? std::string() : parameter->name->rawValue,
                isAddressProjection,
                parameter->span,
                false,
                parameterEffect == syntax::AccessEffect::consuming,
                parameterEffect == syntax::AccessEffect::initializing,
                parameter->label == nullptr ? std::string() : parameter->label->rawValue,
            });
            // A String role's parameters are bound to their values, with no storage to describe (see
            // `boundParameters`).
            const bool boundParameter = !isAddressProjection && parameterEffect != syntax::AccessEffect::consuming &&
                    isStringRoleFunction(*symbol);
            const auto debugVariable = function.isExternal || boundParameter ? std::nullopt : ensureDebugVariable(
                    *parameterSymbol,
                    function.id,
                    static_cast<uint32_t>(index + 1));
            if (debugVariable.has_value() && isAddressProjection) {
                function.entryDebugVariableBindings.push_back(ir::DebugVariableBinding {
                    *debugVariable,
                    static_cast<ir::ValueId>(index),
                });
            }
        }

        if (currentInstance == nullptr) functions.emplace(*symbol, function.id);
        module->functions.push_back(std::move(function));
    }

    void lowerFunctions() {
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.compilationRoot();
        if (root == nullptr) return;
        for (const auto& item : root->items) {
            if (item->kind != syntax::Kind::functionDecl) continue;
            const auto declaration = std::static_pointer_cast<syntax::FunctionDeclSyntax>(item);
            const auto symbol = semanticModel.declaredSymbol(declaration);
            if (!symbol.has_value() || deferredPreludeFunctions.contains(*symbol)) continue;
            const auto found = functions.find(*symbol);
            if (found != functions.end() && !module->functions[found->second].isExternal) {
                lowerFunction(declaration, found->second);
            }
        }
        forEachMethod(*root, [this, &semanticModel](const syntax::FunctionDeclSyntax::Ptr& method) {
            const auto symbol = semanticModel.declaredSymbol(method);
            if (!symbol.has_value() || model->genericParameters(*symbol) != nullptr) return;
            const auto found = functions.find(*symbol);
            if (found != functions.end()) lowerFunction(method, found->second);
        });
        for (size_t index = 0; index < instanceFunctions.size(); ++index) {
            if (instanceFunctions[index] == ir::invalidFunctionId) continue;
            const auto declaration = genericDeclaration(*root, model->instances()[index].function);
            enterInstance(index);
            currentNoncopyableSpans = &noncopyableSpans(declaration);
            lowerFunction(declaration, instanceFunctions[index]);
            leaveInstance();
        }
        for (size_t index = 0; index < pendingPreludeBodies.size(); ++index) {
            const auto [declaration, id] = pendingPreludeBodies[index];
            lowerFunction(declaration, id);
        }
    }

    void lowerFunction(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            ir::FunctionId functionId) {
        currentFunctionId = functionId;
        currentBlockId = 0;
        slots.clear();
        ownership.clear();
        liveOwnedTemporaries.clear();
        scopes.clear();
        pushScope();
        auto& function = currentFunction();
        currentDebugScope = function.debugScope;
        function.blocks.push_back(ir::BasicBlock { 0, "entry", {} });
        nextValue = static_cast<ir::ValueId>(function.parameters.size());
        const auto diagnosticStart = diagnostics.size();

        boundParameters.clear();
        const bool bindsParameters = function.symbol.has_value() && isStringRoleFunction(*function.symbol);
        for (const auto& parameter : function.parameters) {
            recordValue(parameter.value, ValueOwnership::borrowed);
            if (parameter.isMutable) {
                if (parameter.symbol.has_value()) slots[*parameter.symbol] = parameter.value;
                continue;
            }
            if (bindsParameters && parameter.symbol.has_value() && !parameter.isConsuming) {
                boundParameters[*parameter.symbol] = parameter.value;
                continue;
            }
            const auto address = emitValue(
                    ir::Opcode::stackAllocate,
                    parameter.value.type,
                    ir::ValueCategory::address,
                    {},
                    parameter.span,
                    parameter.symbol,
                    true);
                    const auto debugVariable = parameter.symbol.has_value()
                        ? ensureDebugVariable(*parameter.symbol, function.id)
                        : std::optional<ir::DebugVariableId>();
                emitRawStore(
                    parameter.value,
                    address,
                    parameter.span,
                    parameter.symbol,
                        true,
                        debugVariable);
            if (parameter.symbol.has_value()) slots[*parameter.symbol] = address;
            if (parameter.isConsuming) registerOwnedStorage(address);
        }

        auto bodyValue = lowerBlock(declaration->body);
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            return;
        }
        const auto trailingSpan = declaration->body != nullptr &&
                !declaration->body->items.empty()
                ? declaration->body->items.back()->span
                : declaration->span;
        const auto hasTrailingExpression = declaration->body != nullptr &&
                !declaration->body->items.empty() &&
                isExpressionKind(declaration->body->items.back()->kind);
        if (function.returnsValue && bodyValue.has_value() && hasTrailingExpression) {
            bodyValue = coerce(*bodyValue, function.resultType, trailingSpan);
            if (bodyValue.has_value() && requiresDestroy(bodyValue->type)) {
                bodyValue = acquireOwned(*bodyValue, trailingSpan);
            }
            if (bodyValue.has_value()) {
                cleanupAndPopScope(declaration->body->span);
                auto instruction = makeInstruction(
                        ir::Opcode::ret,
                        trailingSpan);
                instruction.operands = { bodyValue->id };
                emit(std::move(instruction));
                return;
            }
        }
        cleanupAndPopScope(declaration->span);
        if (function.returnsValue) {
            if (diagnostics.size() == diagnosticStart) {
                report(
                        DiagnosticId::missingReturn,
                        declaration->span,
                        "value-returning function reaches the end without returning");
            }
            emit(makeInstruction(ir::Opcode::unreachable, declaration->span, true));
        } else if (function.resultType == model->types().neverType()) {
            emit(makeInstruction(ir::Opcode::unreachable, declaration->span, true));
        } else {
            emit(makeInstruction(
                    ir::Opcode::ret,
                    declaration->body == nullptr ? declaration->span : declaration->body->span,
                    true));
        }
    }

    std::optional<ir::Value> lowerBlock(const syntax::BlockExprSyntax::Ptr& block) {
        if (block == nullptr) return std::nullopt;
        DebugScopeGuard debugScope(
            currentDebugScope,
            debugScopeForNode(block, currentFunctionId));
        pushScope();
        std::optional<ir::Value> result;
        bool unitResult = block->items.empty();
        for (const auto& item : block->items) {
            if (currentBlockTerminated()) break;
            if (item->kind == syntax::Kind::bindingDecl) {
                lowerBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(item));
                result.reset();
                unitResult = true;
            } else if (item->kind == syntax::Kind::whileStmt) {
                lowerWhile(std::static_pointer_cast<syntax::WhileStmtSyntax>(item));
                result.reset();
                unitResult = true;
            } else if (item->kind == syntax::Kind::breakStmt ||
                       item->kind == syntax::Kind::continueStmt) {
                lowerLoopControl(item);
                result.reset();
            } else if (isExpressionKind(item->kind)) {
                result = lowerExpression(std::static_pointer_cast<syntax::ExprSyntax>(item));
                unitResult = false;
            }
        }
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            return std::nullopt;
        }
        // A discarded trailing value is never needed as an owned one (spec 4.1.1): a place there is
        // only read, never copied. An owned temporary still moves to the parent scope, which
        // destroys it after this block's locals.
        if (result.has_value() && requiresDestroy(result->type) &&
                ownershipOf(*result) != ValueOwnership::owned &&
                model->isValueDiscarded(block->items.back())) {
            result.reset();
            unitResult = true;
        }
        if (result.has_value() && requiresDestroy(result->type)) {
            const auto resultSpan = !block->items.empty()
                    ? block->items.back()->span
                    : block->span;
            result = acquireOwned(*result, resultSpan);
            if (!result.has_value()) {
                cleanupAndPopScope(block->span);
                return std::nullopt;
            }
        }
        if (unitResult) result = emitUnit(block->span);
        cleanupAndPopScope(block->span);
        if (result.has_value() && requiresDestroy(result->type)) {
            adoptInParentScope(*result);
        }
        return result;
    }

    void lowerBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        const auto type = symbol.has_value()
                ? typeOf(*symbol)
                : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) {
            report(
                    symbol.has_value() ? DiagnosticId::missingType : DiagnosticId::missingSymbol,
                    declaration->span,
                    "binding is missing a typed symbol");
            return;
        }
            const auto uninitializedDebugVariable = declaration->initializer == nullptr
                ? ensureDebugVariable(*symbol, currentFunctionId)
                : std::optional<ir::DebugVariableId>();
            const auto bindOnAllocation = declaration->initializer == nullptr &&
                !requiresDestroy(*type);
        const auto address = emitValue(
                ir::Opcode::stackAllocate,
                *type,
                ir::ValueCategory::address,
                {},
                declaration->span,
                symbol,
                false,
                bindOnAllocation
                    ? uninitializedDebugVariable
                    : std::optional<ir::DebugVariableId>());
        slots[*symbol] = address;
        if (declaration->initializer == nullptr) {
            if (requiresDestroy(*type)) {
                auto initialize = makeInstruction(
                        ir::Opcode::zeroInitialize,
                        declaration->span);
                initialize.operands = { address.id };
                if (uninitializedDebugVariable.has_value()) {
                    initialize.debugVariableBindings.push_back(ir::DebugVariableBinding {
                        *uninitializedDebugVariable,
                        address.id,
                    });
                }
                emit(std::move(initialize));
                registerOwnedStorage(address);
            }
            return;
        }
        auto initializer = lowerExpression(declaration->initializer);
        if (!initializer.has_value()) return;
        initializer = coerce(*initializer, *type, declaration->initializer->span);
        if (!initializer.has_value()) return;
        if (requiresDestroy(*type)) {
            initializer = acquireOwned(*initializer, declaration->initializer->span);
            if (!initializer.has_value()) return;
        }
        const auto debugVariable = ensureDebugVariable(*symbol, currentFunctionId);
        emitRawStore(
            *initializer,
            address,
            declaration->span,
            symbol,
            false,
            debugVariable);
        if (requiresDestroy(*type)) {
            registerOwnedStorage(address);
        }
    }

    std::optional<ir::Value> lowerExpression(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return std::nullopt;
        switch (expression->kind) {
            case syntax::Kind::unitExpr:
                return emitUnit(expression->span);
            case syntax::Kind::literalExpr:
                return lowerLiteral(std::static_pointer_cast<syntax::LiteralExprSyntax>(expression));
            case syntax::Kind::nameExpr:
                return lowerName(expression);
            case syntax::Kind::parenthesizedExpr:
                return lowerExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::prefixExpr:
                return lowerPrefix(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression));
            case syntax::Kind::accessExpr: {
                const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(expression);
                // `consume x` in an owned context moves the place out (decision 0015).
                if (access->marker != nullptr && access->marker->kind == kwConsume &&
                    isAddressable(access->operand)) {
                    return lowerTake(access->operand);
                }
                return lowerExpression(access->operand);
            }
            case syntax::Kind::propagateExpr:
                return lowerPropagation(
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression));
            case syntax::Kind::binaryExpr:
                return lowerBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
            case syntax::Kind::assignmentExpr:
                return lowerAssignment(
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression));
            case syntax::Kind::memberExpr:
                return lowerMember(std::static_pointer_cast<syntax::MemberExprSyntax>(expression));
            case syntax::Kind::callExpr:
                return lowerCall(std::static_pointer_cast<syntax::CallExprSyntax>(expression));
            case syntax::Kind::subscriptExpr:
                return lowerSubscript(std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression));
            case syntax::Kind::arrayExpr:
                return lowerArray(std::static_pointer_cast<syntax::ArrayExprSyntax>(expression));
            case syntax::Kind::dictionaryExpr:
                return lowerDictionary(
                        std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression));
            case syntax::Kind::contextualCaseExpr:
                return lowerContextualCase(
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression));
            case syntax::Kind::returnExpr:
                lowerReturn(std::static_pointer_cast<syntax::ReturnExprSyntax>(expression));
                return std::nullopt;
            case syntax::Kind::blockExpr:
                return lowerBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression));
            case syntax::Kind::ifExpr:
                return lowerIf(std::static_pointer_cast<syntax::IfExprSyntax>(expression));
            case syntax::Kind::matchExpr:
                return lowerMatch(std::static_pointer_cast<syntax::MatchExprSyntax>(expression));
            default:
                report(
                        DiagnosticId::unsupportedSyntax,
                        expression->span,
                        std::string("IR lowering is not implemented for ") +
                                syntax::kindName(expression->kind));
                return std::nullopt;
        }
    }

    std::optional<ir::Value> lowerLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression) {
        const auto type = typeOf(expression);
        if (!type.has_value() || expression->literal == nullptr) {
            report(DiagnosticId::missingType, expression->span, "literal has no type");
            return std::nullopt;
        }

        ir::Opcode opcode;
        switch (expression->literal->kind) {
            case decimalLiteral:
                opcode = typing::isFloating(model->types().type(*type)->kind)
                        ? ir::Opcode::floatingConstant : ir::Opcode::literal;
                break;
            case floatLiteral: opcode = ir::Opcode::floatingConstant; break;
            case booleanLiteral: opcode = ir::Opcode::literal; break;
            case stringLiteral: opcode = ir::Opcode::stringConstant; break;
            case byteLiteral: opcode = ir::Opcode::literal; break;
            case nilLiteral: {
                const auto none = findRoleCase(*type, ir::EnumerationRole::optional, true);
                if (!none.has_value()) {
                    report(
                            DiagnosticId::missingSymbol,
                            expression->span,
                            "nil type has no Optional.None case in IR");
                    return std::nullopt;
                }
                return emitEnumConstruction(*type, *none, {}, expression->span);
            }
            default:
                report(
                        DiagnosticId::unsupportedSyntax,
                        expression->span,
                        "literal kind is not supported by primitive IR lowering");
                return std::nullopt;
        }

        auto instruction = makeInstruction(opcode, expression->span);
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        if (expression->literal->kind == booleanLiteral) {
            instruction.integerValue = expression->literal->rawValue == Literals::TRUE ? 1 : 0;
        } else if (opcode == ir::Opcode::floatingConstant || expression->literal->kind == stringLiteral) {
            instruction.text = expression->literal->rawValue;
        } else {
            instruction.integerValue = expression->literal->intValue;
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(
            result,
            expression->literal->kind == stringLiteral
                ? ValueOwnership::borrowed
                : ValueOwnership::trivial);
        return result;
    }

    // A function named where it is not called (spec 5.6.1).
    std::optional<ir::Value> lowerFunctionAddress(semantic::SymbolId symbol, const syntax::NodePtr& expression) {
        const auto type = typeOf(expression);
        const auto function = demandFunction(symbol);
        if (!function.has_value() || !type.has_value()) {
            report(DiagnosticId::missingSymbol, expression->span,
                   "function value has no IR function declaration");
            return std::nullopt;
        }
        auto instruction = makeInstruction(ir::Opcode::functionAddress, expression->span);
        instruction.callee = *function;
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }

    bool namesFunctionValue(const syntax::NodePtr& expression, std::optional<semantic::SymbolId> symbol) const {
        if (!symbol.has_value()) return false;
        const auto* declaration = model->semanticModel()->symbol(*symbol);
        const auto type = typeOf(expression);
        const auto* record = type.has_value() ? model->types().type(*type) : nullptr;
        return declaration != nullptr && declaration->kind == semantic::SymbolKind::function &&
                record != nullptr && record->kind == typing::TypeKind::function;
    }

    std::optional<ir::Value> lowerName(const syntax::NodePtr& expression) {
        const auto symbol = model->referencedSymbol(expression);
        if (namesFunctionValue(expression, symbol)) return lowerFunctionAddress(*symbol, expression);
        if (symbol.has_value() && topLevelBindings.contains(*symbol)) {
            // A rejected declaration was already reported; do not add a second error.
            if (!constantDeclarations.contains(*symbol)) return std::nullopt;
            return lowerConstant(*symbol, expression->span);
        }
        if (symbol.has_value()) {
            if (const auto bound = boundParameters.find(*symbol); bound != boundParameters.end()) return bound->second;
        }
        if (!symbol.has_value() || !slots.contains(*symbol)) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "name does not refer to a lowered local storage slot");
            return std::nullopt;
        }
        const auto address = slots.at(*symbol);
        return emitValue(
                ir::Opcode::load,
                address.type,
                ir::ValueCategory::value,
                { address.id },
                expression->span,
                symbol);
    }

    std::optional<ir::Value> lowerPrefix(const syntax::PrefixExprSyntax::Ptr& expression) {
        const auto type = typeOf(expression);
        if (type.has_value() && expression->op != nullptr && expression->op->kind == minus &&
                typing::isInteger(model->types().type(*type)->kind) &&
                expression->operand->kind == syntax::Kind::literalExpr) {
            const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression->operand);
            if (literal->literal->kind == decimalLiteral) {
                auto constant = makeInstruction(ir::Opcode::literal, expression->span);
                constant.result = makeValue(*type, ir::ValueCategory::value);
                constant.integerValue = std::bit_cast<int64_t>(
                        uint64_t{0} - static_cast<uint64_t>(literal->literal->intValue));
                const auto result = *constant.result;
                emit(std::move(constant));
                recordValue(result, ValueOwnership::trivial);
                return result;
            }
        }
        const auto operand = lowerExpression(expression->operand);
        if (currentBlockTerminated()) return std::nullopt;
        if (!operand.has_value() || !type.has_value()) return std::nullopt;
        if (expression->castTarget != nullptr && expression->op->rawValue == "as?") {
            return lowerTryConversion(expression, *operand, *type);
        }
        if (expression->castTarget != nullptr) {
            if (operand->type == *type) return operand;
            return emitValue(
                    expression->op->rawValue == "&as"
                            ? ir::ConversionMode::wrapping : ir::ConversionMode::checked,
                    *type,
                    ir::ValueCategory::value,
                    { operand->id },
                    expression->span);
        }
        if (expression->op != nullptr && expression->op->kind == bang) {
            auto constant = makeInstruction(ir::Opcode::literal, expression->op->span);
            constant.integerValue = 0;
            constant.result = makeValue(*type, ir::ValueCategory::value);
            const auto falseValue = *constant.result;
            emit(std::move(constant));
            return emitValue(
                    ir::ComparePredicate::equal,
                    *type,
                    ir::ValueCategory::value,
                    { operand->id, falseValue.id },
                    expression->span);
        }
        if (expression->op != nullptr && expression->op->kind == tilde) {
            // ~x is x ^ all-ones; the constant is truncated to the operand width.
            auto ones = makeInstruction(ir::Opcode::literal, expression->op->span);
            const auto kind = model->types().type(*type)->kind;
            const auto bits = typing::scalarBitWidth(kind);
            ones.integerValue = typing::isSignedInteger(kind) || bits == 64
                    ? -1 : static_cast<int64_t>((uint64_t{1} << bits) - 1);
            ones.result = makeValue(*type, ir::ValueCategory::value);
            const auto onesValue = *ones.result;
            emit(std::move(ones));
            return emitValue(
                    ir::Opcode::bitXor,
                    *type,
                    ir::ValueCategory::value,
                    { operand->id, onesValue.id },
                    expression->span);
        }
        if (expression->op == nullptr || expression->op->kind != minus) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "prefix operator is not supported by primitive IR lowering");
            return std::nullopt;
        }

        const bool floating = typing::isFloating(model->types().type(*type)->kind);
        auto zero = makeInstruction(floating ? ir::Opcode::floatingConstant : ir::Opcode::literal,
                expression->op->span);
        if (floating) zero.text = "-0.0";
        zero.result = makeValue(*type, ir::ValueCategory::value);
        const auto zeroValue = *zero.result;
        emit(std::move(zero));
        return emitValue(
                ir::Opcode::subtract,
                *type,
                ir::ValueCategory::value,
                { zeroValue.id, operand->id },
                expression->span);
    }

    std::optional<ir::Value> lowerBinary(const syntax::BinaryExprSyntax::Ptr& expression) {
        if (expression->op != nullptr &&
            (expression->op->kind == andAnd || expression->op->kind == orOr)) {
            return lowerLogical(expression);
        }
        const auto type = typeOf(expression);
        if (expression->op == nullptr || !type.has_value()) return std::nullopt;
        if (expression->op->kind == equalEqual || expression->op->kind == notEqual) {
            // A conformer other than a builtin compares through its `Equatable` witness (decision 0029).
            const auto operandType = typeOf(expression->left);
            if (operandType.has_value() && !model->isBuiltinEquatable(*operandType)) {
                return lowerWitnessEquality(expression->left, expression->right, *operandType,
                                            expression->op->kind == equalEqual, *type, expression->span);
            }
        }
        return lowerBuiltinBinary(expression->op->kind, expression->left, expression->right, *type, expression->span);
    }

    // A binary operator on builtin operands. Where the checked body names a builtin type, the left operand is read
    // as a value before the right is evaluated (spec 20.13.2). In a generic body the operands are a type parameter's
    // values, so the left stays borrowed, as the checked body decided (decision 0021). `Void` equality is a constant
    // once both operands are evaluated.
    std::optional<ir::Value> lowerBuiltinBinary(
            TokenKind op, const syntax::ExprPtr& leftOperand, const syntax::ExprPtr& rightOperand,
            typing::TypeId type, SourceSpan span) {
        auto left = lowerExpression(leftOperand);
        if (!left.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto checked = model->typeOf(leftOperand);
        const bool equality = op == equalEqual || op == notEqual;
        if (!equality || !checked.has_value() || !model->types().mentionsTypeParameter(*checked)) {
            left = prepareOwned(*left, leftOperand->span);
            if (!left.has_value()) return std::nullopt;
        }
        const auto right = lowerExpression(rightOperand);
        if (!left.has_value() || !right.has_value() || currentBlockTerminated()) return std::nullopt;
        if ((op == equalEqual || op == notEqual) && left->type == model->types().voidType()) {
            return emitBoolLiteral(op == equalEqual, span);
        }
        if (isString(left->type)) {
            return emitStringOperator(op, *left, *right, type, span);
        }
        const auto opcode = binaryOpcode(op);
        if (!opcode.has_value()) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    span,
                    "binary operator is not supported by primitive IR lowering");
            return std::nullopt;
        }
        return emitValue(
                *opcode,
                type,
                ir::ValueCategory::value,
                { left->id, right->id },
                span);
    }

    ir::Value emitBoolLiteral(bool value, SourceSpan span) {
        auto constant = makeInstruction(ir::Opcode::literal, span);
        constant.integerValue = value ? 1 : 0;
        constant.result = makeValue(model->types().boolType(), ir::ValueCategory::value);
        const auto result = *constant.result;
        emit(std::move(constant));
        recordValue(result, ValueOwnership::trivial);
        return result;
    }

    // `left == right` on a hand-written conformance (decision 0029, spec 20.13.2): a call of the witness with both
    // operands borrowed; `!=` compares the result with `false`.
    std::optional<ir::Value> lowerWitnessEquality(
            const syntax::ExprPtr& leftOperand, const syntax::ExprPtr& rightOperand, typing::TypeId operandType,
            bool equal, typing::TypeId type, SourceSpan span) {
        const auto requirement = model->equatableRequirement();
        const auto callee = requirement.has_value() ? witnessFunction(operandType, *requirement) : std::nullopt;
        if (!callee.has_value()) {
            report(DiagnosticId::missingSymbol, span, "the 'Equatable' conformance has no witness to call");
            return std::nullopt;
        }
        const auto left = lowerExpression(leftOperand);
        if (!left.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto right = lowerExpression(rightOperand);
        if (!right.has_value() || currentBlockTerminated()) return std::nullopt;
        auto call = makeInstruction(ir::Opcode::call, span);
        call.callee = *callee;
        call.operands = { left->id, right->id };
        call.result = makeValue(type, ir::ValueCategory::value);
        const auto result = *call.result;
        emit(std::move(call));
        recordValue(result, ValueOwnership::trivial);
        if (equal) return result;
        const auto no = emitBoolLiteral(false, span);
        return emitValue(ir::ComparePredicate::equal, type, ir::ValueCategory::value, { result.id, no.id }, span);
    }

    bool isString(typing::TypeId type) const {
        const auto* record = model->types().type(type);
        return record != nullptr && record->kind == typing::TypeKind::string;
    }

    // A call to the collections prelude's function for a String role (decision 0028 stage 3). The operands are
    // borrowed; a String or array result is owned.
    std::optional<ir::Value> emitStringRole(
            semantic::LibraryRole role, const std::vector<ir::Value>& operands, typing::TypeId type,
            SourceSpan span) {
        const auto callee = stringRoleFunction(role);
        if (!callee.has_value()) {
            report(DiagnosticId::missingSymbol, span, "the String operation has no collections prelude function");
            return std::nullopt;
        }
        auto call = makeInstruction(ir::Opcode::call, span);
        call.callee = *callee;
        for (const auto& operand : operands) call.operands.push_back(operand.id);
        call.result = makeValue(type, ir::ValueCategory::value);
        const auto result = *call.result;
        emit(std::move(call));
        recordValue(result, requiresDestroy(type) ? ValueOwnership::owned : ValueOwnership::trivial);
        return result;
    }

    // `+`, `==`, `!=` and the orderings on Strings: `+` concatenates, `!=` is `compare eq` of the equality with
    // false, and each ordering compares the -1, 0 or 1 order with 0.
    std::optional<ir::Value> emitStringOperator(
            TokenKind token, ir::Value left, ir::Value right, typing::TypeId type, SourceSpan span) {
        if (token == plus) return emitStringRole(semantic::LibraryRole::stringConcat, { left, right }, type, span);
        const auto predicate = binaryOpcode(token);
        if (!predicate.has_value() || predicate->opcode != ir::Opcode::compare) {
            report(DiagnosticId::unsupportedSyntax, span, "operator is not supported on String values");
            return std::nullopt;
        }
        const auto relation = static_cast<ir::ComparePredicate>(predicate->modifier);
        const bool equality = relation == ir::ComparePredicate::equal || relation == ir::ComparePredicate::notEqual;
        const auto resultType = equality ? model->types().boolType() : model->types().intType();
        const auto computed = emitStringRole(
                equality ? semantic::LibraryRole::stringEqual : semantic::LibraryRole::stringOrder,
                { left, right }, resultType, span);
        if (!computed.has_value() || relation == ir::ComparePredicate::equal) return computed;
        auto constant = makeInstruction(ir::Opcode::literal, span);
        constant.integerValue = 0;
        constant.result = makeValue(resultType, ir::ValueCategory::value);
        const auto zero = *constant.result;
        emit(std::move(constant));
        return emitValue(equality ? ir::ComparePredicate::equal : relation, type, ir::ValueCategory::value,
                         { computed->id, zero.id }, span);
    }

    // `x as? T`: .Ok(converted) when the value fits in T, else .Err(.OutOfRange).
    std::optional<ir::Value> lowerTryConversion(
            const syntax::PrefixExprSyntax::Ptr& expression,
            ir::Value operand,
            typing::TypeId resultType) {
        const auto* result = model->types().type(resultType);
        const auto okCase = findRoleCase(resultType, ir::EnumerationRole::result, false);
        const auto errCase = findRoleCase(resultType, ir::EnumerationRole::result, true);
        if (result == nullptr || result->arguments.size() != 2 ||
            !okCase.has_value() || !errCase.has_value()) {
            report(DiagnosticId::missingType, expression->span,
                    "'as?' result is not a Result with Ok and Err cases");
            return std::nullopt;
        }
        const auto target = result->arguments[0];
        const auto errorType = result->arguments[1];
        const auto outOfRange = findEnumCase(errorType, "OutOfRange");
        if (!outOfRange.has_value()) {
            report(DiagnosticId::missingSymbol, expression->span,
                    "ConversionError has no OutOfRange case in IR");
            return std::nullopt;
        }

        auto test = makeInstruction(ir::Opcode::integerFits, expression->span);
        test.result = makeValue(model->types().boolType(), ir::ValueCategory::value);
        test.operands = { operand.id };
        test.integerValue = static_cast<int64_t>(target);
        const auto fits = *test.result;
        emit(std::move(test));
        recordValue(fits, ValueOwnership::trivial);

        const auto slot = emitValue(
                ir::Opcode::stackAllocate, resultType, ir::ValueCategory::address,
                {}, expression->span, std::nullopt, true);
        const auto okBlock = createBlock("convert.ok");
        const auto errBlock = createBlock("convert.out_of_range");
        const auto mergeBlock = createBlock("convert.merge");
        emitConditionalBranch(fits, okBlock, errBlock, expression->span);

        switchToBlock(okBlock);
        const auto converted = emitValue(
                ir::ConversionMode::wrapping, target, ir::ValueCategory::value,
                { operand.id }, expression->span);
        const auto ok = emitEnumConstruction(resultType, *okCase, { converted }, expression->span);
        if (!ok.has_value()) return std::nullopt;
        emitRawStore(*ok, slot, expression->span, std::nullopt, true);
        emitBranch(mergeBlock, expression->span);

        switchToBlock(errBlock);
        const auto failure = emitEnumConstruction(errorType, *outOfRange, {}, expression->span);
        if (!failure.has_value()) return std::nullopt;
        const auto err = emitEnumConstruction(resultType, *errCase, { *failure }, expression->span);
        if (!err.has_value()) return std::nullopt;
        emitRawStore(*err, slot, expression->span, std::nullopt, true);
        emitBranch(mergeBlock, expression->span);

        switchToBlock(mergeBlock);
        return emitValue(
                ir::Opcode::load, resultType, ir::ValueCategory::value,
                { slot.id }, expression->span);
    }

    std::optional<ir::Value> lowerLogical(
            const syntax::BinaryExprSyntax::Ptr& expression) {
        const auto isAnd = expression->op->kind == andAnd;
        pushScope();
        const auto left = lowerExpression(expression->left);
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            return std::nullopt;
        }
        cleanupAndPopScope(expression->left->span);
        if (!left.has_value()) return std::nullopt;

        const auto resultAddress = emitValue(
                ir::Opcode::stackAllocate,
                model->types().boolType(),
                ir::ValueCategory::address,
                {},
                expression->span,
                std::nullopt,
                true);
        emitRawStore(*left, resultAddress, expression->left->span, std::nullopt, true);
        const auto rightBlock = createBlock(isAnd ? "and.rhs" : "or.rhs");
        const auto mergeBlock = createBlock(isAnd ? "and.merge" : "or.merge");
        emitConditionalBranch(
                *left,
                isAnd ? rightBlock : mergeBlock,
                isAnd ? mergeBlock : rightBlock,
                expression->left->span);

        switchToBlock(rightBlock);
        pushScope();
        const auto right = lowerExpression(expression->right);
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
        } else if (right.has_value()) {
            emitRawStore(*right, resultAddress, expression->right->span, std::nullopt, true);
            cleanupAndPopScope(expression->right->span);
            emitBranch(mergeBlock, expression->span);
        } else {
            cleanupAndPopScope(expression->right->span);
            report(
                    DiagnosticId::missingType,
                    expression->right->span,
                    "logical right operand did not lower a Boolean value");
            emit(makeInstruction(ir::Opcode::unreachable, expression->right->span, true));
        }

        switchToBlock(mergeBlock);
        return emitValue(
                ir::Opcode::load,
                model->types().boolType(),
                ir::ValueCategory::value,
                { resultAddress.id },
                expression->span);
    }

    std::optional<Operation> binaryOpcode(TokenKind token) const {
        switch (token) {
            case plus: return ir::Opcode::add;
            case minus: return ir::Opcode::subtract;
            case multiply: return ir::Opcode::multiply;
            case divide: return ir::Opcode::divide;
            case percentage: return ir::Opcode::remainder;
            case ampersand: return ir::Opcode::bitAnd;
            case verticalBar: return ir::Opcode::bitOr;
            case caret: return ir::Opcode::bitXor;
            case shiftLeft: return ir::Opcode::shiftLeft;
            case shiftRight: return ir::Opcode::shiftRight;
            case less: return ir::ComparePredicate::less;
            case lessEqual: return ir::ComparePredicate::lessEqual;
            case greater: return ir::ComparePredicate::greater;
            case greaterEqual: return ir::ComparePredicate::greaterEqual;
            case equalEqual: return ir::ComparePredicate::equal;
            case notEqual: return ir::ComparePredicate::notEqual;
            default: return std::nullopt;
        }
    }

    std::optional<ir::Value> lowerAssignment(
            const syntax::AssignmentExprSyntax::Ptr& expression) {
        if (expression->op != nullptr && expression->op->kind != equal) {
            return lowerCompoundAssignment(expression);
        }
        auto target = expression->target;
        while (target != nullptr &&
               (target->kind == syntax::Kind::accessExpr ||
                target->kind == syntax::Kind::parenthesizedExpr)) {
            target = target->kind == syntax::Kind::accessExpr
                    ? std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand
                    : std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression;
        }
        if (target != nullptr && target->kind == syntax::Kind::subscriptExpr) {
            const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
            const auto baseType = typeOf(subscript->base);
            if (baseType.has_value() && model->types().isDictionary(*baseType)) {
                return lowerDictionarySet(expression, subscript, *baseType);
            }
        }
        const auto deferredAddress = prepareAddress(expression->target,
                !targetIndexesCannotDisturb(expression->target) || !cannotDisturb(expression->value));
        if (!deferredAddress.has_value() || currentBlockTerminated()) return std::nullopt;
        auto value = lowerExpression(expression->value);
        if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto address = (*deferredAddress)();
        if (address.has_value() && value.has_value()) {
            value = coerce(*value, address->type, expression->value->span);
            if (!value.has_value()) return std::nullopt;
            if (requiresDestroy(address->type)) {
                value = acquireOwned(*value, expression->value->span);
                if (!value.has_value()) return std::nullopt;
                emitDestroyStorage(*address, expression->target->span);
            }
            emitRawStore(*value, *address, expression->span);
            return emitUnit(expression->span);
        }
        return std::nullopt;
    }

    // `target op= value`: any index is evaluated once, then the right side,
    // and only then is the element address computed, the old value read,
    // combined with the right side, and stored back.
    std::optional<ir::Value> lowerCompoundAssignment(
            const syntax::AssignmentExprSyntax::Ptr& expression) {
        const auto deferredAddress = prepareAddress(expression->target,
                !targetIndexesCannotDisturb(expression->target) || !cannotDisturb(expression->value));
        if (!deferredAddress.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto right = lowerExpression(expression->value);
        if (!right.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto address = (*deferredAddress)();
        if (!address.has_value()) return std::nullopt;
        const auto old = emitValue(
                ir::Opcode::load,
                address->type,
                ir::ValueCategory::value,
                { address->id },
                expression->target->span);
        std::optional<ir::Value> combined;
        if (isString(address->type)) {
            combined = emitStringOperator(compoundBaseOperator(expression->op->kind), old, *right, address->type,
                                          expression->span);
        } else if (const auto opcode = binaryOpcode(compoundBaseOperator(expression->op->kind))) {
            combined = emitValue(*opcode, address->type, ir::ValueCategory::value, { old.id, right->id },
                                 expression->span);
        } else {
            report(DiagnosticId::unsupportedSyntax, expression->span,
                    "compound assignment operator is not supported by primitive IR lowering");
        }
        if (!combined.has_value()) return std::nullopt;
        auto result = *combined;
        if (requiresDestroy(address->type)) {
            const auto owned = acquireOwned(result, expression->value->span);
            if (!owned.has_value()) return std::nullopt;
            result = *owned;
            emitDestroyStorage(*address, expression->target->span);
        }
        emitRawStore(result, *address, expression->span);
        return emitUnit(expression->span);
    }

    // Whether evaluating `expression` provably runs no code that could change storage read earlier in the same
    // expression: a name, a literal, or a field or element read of such operands. Anything else (a call, `&`,
    // `consume`, an operator, a block) may disturb, so operands evaluated before it are owned and element addresses
    // are taken after it (spec 4.4.3's order).
    static bool cannotDisturb(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return true;
        switch (expression->kind) {
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::unitExpr:
                return true;
            case syntax::Kind::parenthesizedExpr:
                return cannotDisturb(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::memberExpr:
                return cannotDisturb(std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base);
            case syntax::Kind::subscriptExpr: {
                const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                return cannotDisturb(subscript->base) && cannotDisturb(subscript->index);
            }
            default:
                return false;
        }
    }

    // Whether every index operand along an assignment target's path cannot disturb (see `cannotDisturb`).
    static bool targetIndexesCannotDisturb(const syntax::ExprPtr& target) {
        if (target == nullptr) return true;
        switch (target->kind) {
            case syntax::Kind::accessExpr:
                return targetIndexesCannotDisturb(std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand);
            case syntax::Kind::parenthesizedExpr:
                return targetIndexesCannotDisturb(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression);
            case syntax::Kind::memberExpr:
                return targetIndexesCannotDisturb(std::static_pointer_cast<syntax::MemberExprSyntax>(target)->base);
            case syntax::Kind::subscriptExpr: {
                const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
                return targetIndexesCannotDisturb(subscript->base) && cannotDisturb(subscript->index);
            }
            default:
                return true;
        }
    }

    // `&d[k] = v` on a library dictionary (decision 0027): insert or replace through `dict_set`. The base's own
    // index operands, then the key, then the value are evaluated (spec 4.4.3); a base key is owned when a later
    // operand may disturb the binding it was read from. Only then is the base addressed, so the right side cannot
    // leave that address dangling, and a missing outer key traps after the value was evaluated. A trap in the right
    // side inserts nothing.
    std::optional<ir::Value> lowerDictionarySet(
            const syntax::AssignmentExprSyntax::Ptr& expression,
            const syntax::SubscriptExprSyntax::Ptr& subscript,
            typing::TypeId dictionaryType) {
        const auto* record = model->types().type(dictionaryType);
        const auto method = model->collectionRoleMethod(dictionaryType, semantic::LibraryRole::dictSet);
        const auto instance = method.has_value() && record != nullptr
                ? model->instanceIndex(*method, record->arguments) : std::nullopt;
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            report(DiagnosticId::missingSymbol, expression->span, "the dictionary insertion has no lowered instance");
            return std::nullopt;
        }
        const auto keyType = record->arguments[0];
        const auto valueType = record->arguments[1];
        const bool ownKeys = !targetIndexesCannotDisturb(subscript) || !cannotDisturb(expression->value);
        auto base = prepareAddress(subscript->base, ownKeys);
        if (!base.has_value() || currentBlockTerminated()) return std::nullopt;
        auto key = prepareOperand(subscript->index, keyType);
        if (!key.has_value() || currentBlockTerminated()) return std::nullopt;
        auto value = prepareOperand(expression->value, valueType);
        if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
        const auto dictionary = (*base)();
        if (!dictionary.has_value()) return std::nullopt;
        auto call = makeInstruction(ir::Opcode::call, expression->span);
        call.callee = instanceFunctions[*instance];
        call.operands = { dictionary->id };
        if (!transferOperands(call, { *key, *value }, expression->span)) return std::nullopt;
        emit(std::move(call));
        return emitUnit(expression->span);
    }

    std::optional<ir::Value> lowerAddress(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return std::nullopt;
        if (expression->kind == syntax::Kind::accessExpr) {
            return lowerAddress(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        }
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return lowerAddress(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        }
        if (expression->kind == syntax::Kind::nameExpr) {
            const auto symbol = model->referencedSymbol(expression);
            if (symbol.has_value() && slots.contains(*symbol)) return slots.at(*symbol);
        }
        if (expression->kind == syntax::Kind::memberExpr) {
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
            const auto symbol = model->referencedSymbol(member);
            const auto* semanticSymbol = symbol.has_value()
                    ? model->semanticModel()->symbol(*symbol)
                    : nullptr;
            if (semanticSymbol != nullptr &&
                semanticSymbol->kind == semantic::SymbolKind::structureField) {
                const auto base = lowerAddress(member->base);
                const auto type = typeOf(member);
                if (!base.has_value() || !type.has_value()) return std::nullopt;
                return emitValue(
                        ir::Opcode::fieldAddress,
                        *type,
                        ir::ValueCategory::address,
                        { base->id },
                        member->span,
                        symbol);
            }
        }
        if (expression->kind == syntax::Kind::subscriptExpr) {
            const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(
                    expression);
            if (!cannotDisturb(subscript->index)) {
                // The index may reallocate the base's storage or reassign a key: evaluate every index first (spec
                // 4.4.3's order), then address.
                const auto deferred = prepareAddress(expression, true);
                if (!deferred.has_value() || currentBlockTerminated()) return std::nullopt;
                return (*deferred)();
            }
            const auto base = lowerAddress(subscript->base);
            const auto index = lowerExpression(subscript->index);
            const auto type = typeOf(subscript);
            if (!base.has_value() || !index.has_value() || !type.has_value()) {
                return std::nullopt;
            }
            if (const auto baseType = typeOf(subscript->base); baseType.has_value() && isRoleSubscriptBase(*baseType)) {
                return emitRoleElementAddress(*baseType, *base, *index, *type, subscript->span);
            }
            // Only a collection role's element has an address; type checking rejects writing, passing `inout` or
            // consuming a String byte.
            report(DiagnosticId::unsupportedSyntax, subscript->span,
                   "only an array or dictionary element has an address");
            return std::nullopt;
        }
        report(
                DiagnosticId::unsupportedSyntax,
                expression->span,
                "assignment target is not supported by primitive IR lowering");
        return std::nullopt;
    }

    // An assignment target whose index operands are already evaluated but
    // whose element address is not yet computed. Element addresses point into
    // storage the right-hand side may reallocate (for example by appending to
    // the same array), so they are taken only after the right side is lowered.
    // With `ownKeys`, an index that needs destruction is held as an owned
    // temporary, so a later operand that consumes or reassigns the binding it
    // was read from cannot leave the deferred lookup with a dangling key.
    using DeferredAddress = std::function<std::optional<ir::Value>()>;

    std::optional<DeferredAddress> prepareAddress(const syntax::ExprPtr& expression, bool ownKeys = false) {
        if (expression == nullptr) return std::nullopt;
        if (expression->kind == syntax::Kind::accessExpr) {
            return prepareAddress(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand, ownKeys);
        }
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return prepareAddress(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression, ownKeys);
        }
        if (expression->kind == syntax::Kind::subscriptExpr) {
            const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(
                    expression);
            auto base = prepareAddress(subscript->base, ownKeys);
            if (!base.has_value()) return std::nullopt;
            auto index = lowerExpression(subscript->index);
            if (index.has_value() && ownKeys) index = prepareOwned(*index, subscript->index->span);
            const auto type = typeOf(subscript);
            if (!index.has_value() || !type.has_value()) return std::nullopt;
            const auto roleBase = typeOf(subscript->base);
            if (!roleBase.has_value() || !isRoleSubscriptBase(*roleBase)) {
                // Only a collection role's element has an address; type checking rejects writing, passing `inout`
                // or consuming a String byte.
                report(DiagnosticId::unsupportedSyntax, subscript->span,
                       "only an array or dictionary element has an address");
                return std::nullopt;
            }
            return DeferredAddress([this, base = std::move(*base), index = *index, type = *type, roleBase = *roleBase,
                                    span = subscript->span]() -> std::optional<ir::Value> {
                const auto baseAddress = base();
                if (!baseAddress.has_value()) return std::nullopt;
                return emitRoleElementAddress(roleBase, *baseAddress, index, type, span);
            });
        }
        if (expression->kind == syntax::Kind::memberExpr) {
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
            const auto symbol = model->referencedSymbol(member);
            const auto* semanticSymbol = symbol.has_value()
                    ? model->semanticModel()->symbol(*symbol)
                    : nullptr;
            const auto type = typeOf(member);
            if (semanticSymbol != nullptr &&
                semanticSymbol->kind == semantic::SymbolKind::structureField &&
                type.has_value()) {
                auto base = prepareAddress(member->base, ownKeys);
                if (!base.has_value()) return std::nullopt;
                return DeferredAddress([this, base = std::move(*base), type = *type, symbol,
                                        span = member->span]() -> std::optional<ir::Value> {
                    const auto baseAddress = base();
                    if (!baseAddress.has_value()) return std::nullopt;
                    return emitValue(
                            ir::Opcode::fieldAddress,
                            type,
                            ir::ValueCategory::address,
                            { baseAddress->id },
                            span,
                            symbol);
                });
            }
        }
        return DeferredAddress([this, expression] { return lowerAddress(expression); });
    }

    bool isAddressable(const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return false;
        switch (expression->kind) {
            case syntax::Kind::nameExpr: {
                const auto symbol = model->referencedSymbol(expression);
                return symbol.has_value() && slots.contains(*symbol);
            }
            case syntax::Kind::accessExpr:
                return isAddressable(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
            case syntax::Kind::parenthesizedExpr:
                return isAddressable(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::memberExpr:
                return isAddressable(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression)->base);
            case syntax::Kind::subscriptExpr:
                return isAddressable(
                        std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression)->base);
            default:
                return false;
        }
    }

    std::optional<ir::Value> lowerMember(const syntax::MemberExprSyntax::Ptr& expression) {
        const auto symbol = model->referencedSymbol(expression);
        const auto* semanticSymbol = symbol.has_value()
                ? model->semanticModel()->symbol(*symbol)
                : nullptr;
        const auto type = typeOf(expression);
        if (semanticSymbol == nullptr || !type.has_value()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "member has no typed resolved symbol");
            return std::nullopt;
        }
        if (namesFunctionValue(expression, symbol)) return lowerFunctionAddress(*symbol, expression);
        if (topLevelBindings.contains(*symbol)) {
            if (!constantDeclarations.contains(*symbol)) return std::nullopt;
            return lowerConstant(*symbol, expression->span);
        }

        if (semanticSymbol->kind == semantic::SymbolKind::structureField) {
            if (isAddressable(expression)) {
                const auto address = lowerAddress(expression);
                if (!address.has_value()) return std::nullopt;
                return emitValue(
                        ir::Opcode::load,
                        *type,
                        ir::ValueCategory::value,
                        { address->id },
                        expression->span,
                        symbol);
            }
            const auto base = lowerExpression(expression->base);
            if (!base.has_value()) return std::nullopt;
            return emitValue(
                    ir::Opcode::extractField,
                    *type,
                    ir::ValueCategory::value,
                    { base->id },
                    expression->span,
                    symbol);
        }

        if (semanticSymbol->kind == semantic::SymbolKind::builtinMember &&
            semanticSymbol->name == "count") {
            const auto base = lowerExpression(expression->base);
            if (!base.has_value()) return std::nullopt;
            return emitStringRole(semantic::LibraryRole::stringCount, { *base }, *type, expression->span);
        }

        if (semanticSymbol->kind == semantic::SymbolKind::enumCase ||
            semanticSymbol->kind == semantic::SymbolKind::builtinEnumCase) {
            return emitEnumConstruction(*type, *symbol, {}, expression->span);
        }

        report(
                DiagnosticId::unsupportedSyntax,
                expression->span,
                "member kind is not supported by aggregate IR lowering");
        return std::nullopt;
    }

    // Whether `type` is subscripted through a role addressor: a library array or dictionary.
    bool isRoleSubscriptBase(typing::TypeId type) const {
        return model->isArrayRoleType(type) || model->types().isDictionary(type);
    }

    // `a[i]` on a library array (decision 0025) and `d[k]` on a library dictionary (decision 0027): the addressor of
    // the receiver's instance returns the element's address as an `Int`, which only this lowering turns into a typed
    // place. A dictionary key is passed raw as the borrowed argument; an owned key temporary keeps its own cleanup.
    std::optional<ir::Value> emitRoleElementAddress(
            typing::TypeId arrayType, const ir::Value& baseAddress, const ir::Value& index,
            typing::TypeId elementType, SourceSpan span) {
        const auto method = model->collectionRoleMethod(arrayType, model->types().isDictionary(arrayType)
                ? semantic::LibraryRole::dictElement : semantic::LibraryRole::arrayElement);
        const auto* record = model->types().type(arrayType);
        const auto instance = method.has_value() && record != nullptr
                ? model->instanceIndex(*method, record->arguments) : std::nullopt;
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            report(DiagnosticId::missingSymbol, span, "the element addressor has no lowered instance");
            return std::nullopt;
        }
        auto call = makeInstruction(ir::Opcode::call, span);
        call.callee = instanceFunctions[*instance];
        // A borrowing receiver is passed as a value that aliases the array's storage.
        auto receiver = baseAddress;
        if (module->functions[*call.callee].parameters.at(0).value.category == ir::ValueCategory::value) {
            receiver = emitValue(ir::Opcode::load, arrayType, ir::ValueCategory::value, { baseAddress.id }, span);
        }
        call.operands = { receiver.id, index.id };
        call.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto address = *call.result;
        emit(std::move(call));
        recordValue(address, ValueOwnership::trivial);
        if (elementType == model->types().neverType()) {
            emit(makeInstruction(ir::Opcode::unreachable, span));
            return std::nullopt;
        }
        return emitValue(ir::Opcode::integerAddress, elementType, ir::ValueCategory::address, { address.id }, span);
    }

    std::optional<ir::Value> lowerSubscript(
            const syntax::SubscriptExprSyntax::Ptr& expression) {
        if (const auto baseType = typeOf(expression->base); baseType.has_value() && isRoleSubscriptBase(*baseType)) {
            std::optional<ir::Value> baseAddress;
            if (isAddressable(expression->base) && !cannotDisturb(expression->index)) {
                // The index may reallocate the base's storage or reassign one of its keys: the base's indexes are
                // evaluated (and owned), then this index, and only then is the base addressed.
                const auto deferred = prepareAddress(expression->base, true);
                if (!deferred.has_value() || currentBlockTerminated()) return std::nullopt;
                const auto index = lowerExpression(expression->index);
                const auto type = typeOf(expression);
                if (!index.has_value() || !type.has_value() || currentBlockTerminated()) return std::nullopt;
                baseAddress = (*deferred)();
                if (!baseAddress.has_value()) return std::nullopt;
                const auto place = emitRoleElementAddress(*baseType, *baseAddress, *index, *type, expression->span);
                if (!place.has_value()) return std::nullopt;
                return emitValue(ir::Opcode::load, *type, ir::ValueCategory::value, { place->id }, expression->span);
            }
            if (isAddressable(expression->base)) {
                baseAddress = lowerAddress(expression->base);
            } else {
                // A temporary receiver is spilled so the addressor can borrow it; the value keeps its own cleanup.
                const auto value = lowerExpression(expression->base);
                if (!value.has_value()) return std::nullopt;
                baseAddress = emitValue(ir::Opcode::stackAllocate, *baseType, ir::ValueCategory::address, {},
                                        expression->base->span);
                emitRawStore(*value, *baseAddress, expression->base->span);
            }
            const auto index = lowerExpression(expression->index);
            const auto type = typeOf(expression);
            if (!baseAddress.has_value() || !index.has_value() || !type.has_value()) return std::nullopt;
            const auto place = emitRoleElementAddress(*baseType, *baseAddress, *index, *type, expression->span);
            if (!place.has_value()) return std::nullopt;
            return emitValue(ir::Opcode::load, *type, ir::ValueCategory::value, { place->id }, expression->span);
        }
        std::optional<ir::Value> base;
        std::optional<ir::Value> index;
        if (isAddressable(expression->base) && !cannotDisturb(expression->index)) {
            // A String byte read whose index may reassign the string: the base's own indexes, then this index, and
            // only then is the string loaded (spec 4.4.3's order), so the read never sees a freed buffer.
            const auto deferred = prepareAddress(expression->base, true);
            const auto baseType = typeOf(expression->base);
            if (!deferred.has_value() || !baseType.has_value() || currentBlockTerminated()) return std::nullopt;
            index = lowerExpression(expression->index);
            if (!index.has_value() || currentBlockTerminated()) return std::nullopt;
            const auto address = (*deferred)();
            if (!address.has_value()) return std::nullopt;
            base = emitValue(ir::Opcode::load, *baseType, ir::ValueCategory::value, { address->id },
                             expression->base->span);
        } else {
            base = lowerExpression(expression->base);
            index = lowerExpression(expression->index);
        }
        const auto type = typeOf(expression);
        if (!base.has_value() || !index.has_value() || !type.has_value()) {
            return std::nullopt;
        }
        return emitStringRole(semantic::LibraryRole::stringByte, { *base, *index }, *type, expression->span);
    }

    std::optional<ir::Value> lowerArray(const syntax::ArrayExprSyntax::Ptr& expression) {
        const auto type = typeOf(expression);
        if (!type.has_value()) return std::nullopt;
        const auto* arrayType = model->types().type(*type);
        if (arrayType == nullptr || !model->isArrayRoleType(*type) ||
            arrayType->arguments.size() != 1) {
            report(DiagnosticId::missingType, expression->span, "array literal has no array type");
            return std::nullopt;
        }

        const auto elementType = arrayType->arguments[0];
        const auto arguments = arrayType->arguments;
        const auto literal = model->arrayRoleMethod(*type, semantic::LibraryRole::arrayLiteral);
        const auto instance = literal.has_value() ? model->instanceIndex(*literal, arguments) : std::nullopt;
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            report(DiagnosticId::missingSymbol, expression->span, "the array literal method has no lowered instance");
            return std::nullopt;
        }
        std::vector<ir::Value> elements;
        for (const auto& element : expression->elements) {
            const auto value = prepareOperand(element, elementType);
            if (!value.has_value()) return std::nullopt;
            elements.push_back(*value);
        }
        if (currentBlockTerminated()) return std::nullopt;
        const auto integer = [&](int64_t number, SourceSpan span) {
            auto constant = makeInstruction(ir::Opcode::literal, span);
            constant.integerValue = number;
            constant.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto value = *constant.result;
            emit(std::move(constant));
            recordValue(value, ValueOwnership::trivial);
            return value;
        };
        const auto count = integer(static_cast<int64_t>(elements.size()), expression->span);
        const auto receiverStorage = emitValue(ir::Opcode::stackAllocate, *type, ir::ValueCategory::address,
                                              {}, expression->span);
        auto zero = makeInstruction(ir::Opcode::zeroInitialize, expression->span);
        zero.operands = { receiverStorage.id };
        emit(std::move(zero));
        const auto receiver = emitValue(ir::Opcode::load, *type, ir::ValueCategory::value,
                                       { receiverStorage.id }, expression->span);
        recordValue(receiver, ValueOwnership::owned);
        const auto ownedReceiver = acquireOwned(receiver, expression->span);
        if (!ownedReceiver.has_value()) return std::nullopt;

        auto storage = makeInstruction(ir::Opcode::stackBlock, expression->span);
        storage.integerValue = static_cast<int64_t>(elementType);
        storage.operands = { count.id };
        storage.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto block = *storage.result;
        emit(std::move(storage));
        recordValue(block, ValueOwnership::trivial);
        // No element can fail after ownership starts moving into the untracked stack block.
        for (size_t index = 0; index < elements.size(); ++index) {
            const auto value = acquireOwned(elements[index], expression->elements[index]->span);
            if (!value.has_value()) return std::nullopt;
            if (elementType == model->types().voidType()) continue;
            const auto offset = integer(static_cast<int64_t>(index), expression->elements[index]->span);
            auto slot = makeInstruction(ir::Opcode::slotAddress, expression->elements[index]->span);
            slot.integerValue = static_cast<int64_t>(elementType);
            slot.operands = { block.id, offset.id };
            slot.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto address = *slot.result;
            emit(std::move(slot));
            recordValue(address, ValueOwnership::trivial);
            const auto place = emitValue(ir::Opcode::integerAddress, elementType, ir::ValueCategory::address,
                                         { address.id }, expression->elements[index]->span);
            emitRawStore(*value, place, expression->elements[index]->span);
        }
        auto instruction = makeInstruction(ir::Opcode::call, expression->span);
        instruction.callee = instanceFunctions[*instance];
        instruction.operands = { ownedReceiver->id, block.id, count.id };
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    std::optional<ir::Value> lowerDictionary(
            const syntax::DictionaryExprSyntax::Ptr& expression) {
        const auto type = typeOf(expression);
        if (!type.has_value()) return std::nullopt;
        const auto* dictionaryType = model->types().type(*type);
        const auto entryType = model->dictEntryType(*type);
        const auto fields = model->dictEntryFields();
        if (dictionaryType == nullptr || !model->types().isDictionary(*type) || !entryType.has_value() ||
            !fields.has_value()) {
            report(DiagnosticId::missingType, expression->span, "dictionary literal has no dictionary type");
            return std::nullopt;
        }
        const auto arguments = dictionaryType->arguments;
        const auto keyType = arguments[0];
        const auto valueType = arguments[1];
        const auto literal = model->collectionRoleMethod(*type, semantic::LibraryRole::dictLiteral);
        const auto instance = literal.has_value() ? model->instanceIndex(*literal, arguments) : std::nullopt;
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            report(DiagnosticId::missingSymbol, expression->span,
                   "the dictionary literal method has no lowered instance");
            return std::nullopt;
        }
        // Every key and value is evaluated first, left to right, entry by entry (decision 0027, rule 4).
        std::vector<std::pair<ir::Value, ir::Value>> entries;
        for (const auto& entry : expression->entries) {
            const auto key = prepareOperand(entry->key, keyType);
            if (!key.has_value()) return std::nullopt;
            const auto value = prepareOperand(entry->value, valueType);
            if (!value.has_value()) return std::nullopt;
            entries.emplace_back(*key, *value);
        }
        if (currentBlockTerminated()) return std::nullopt;
        const auto integer = [&](int64_t number, SourceSpan span) {
            auto constant = makeInstruction(ir::Opcode::literal, span);
            constant.integerValue = number;
            constant.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto value = *constant.result;
            emit(std::move(constant));
            recordValue(value, ValueOwnership::trivial);
            return value;
        };
        const auto count = integer(static_cast<int64_t>(entries.size()), expression->span);
        const auto receiverStorage = emitValue(ir::Opcode::stackAllocate, *type, ir::ValueCategory::address,
                                              {}, expression->span);
        auto zero = makeInstruction(ir::Opcode::zeroInitialize, expression->span);
        zero.operands = { receiverStorage.id };
        emit(std::move(zero));
        const auto receiver = emitValue(ir::Opcode::load, *type, ir::ValueCategory::value,
                                       { receiverStorage.id }, expression->span);
        recordValue(receiver, ValueOwnership::owned);
        const auto ownedReceiver = acquireOwned(receiver, expression->span);
        if (!ownedReceiver.has_value()) return std::nullopt;

        auto storage = makeInstruction(ir::Opcode::stackBlock, expression->span);
        storage.integerValue = static_cast<int64_t>(*entryType);
        storage.operands = { count.id };
        storage.result = makeValue(model->types().intType(), ir::ValueCategory::value);
        const auto block = *storage.result;
        emit(std::move(storage));
        recordValue(block, ValueOwnership::trivial);
        // Each entry's key, and its value unless `V` is Void, are stored through the entry's fields; nothing can fail
        // after ownership starts moving into the untracked stack block.
        for (size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = expression->entries[index];
            const auto key = acquireOwned(entries[index].first, entry->key->span);
            const auto value = acquireOwned(entries[index].second, entry->value->span);
            if (!key.has_value() || !value.has_value()) return std::nullopt;
            const auto offset = integer(static_cast<int64_t>(index), entry->span);
            auto slot = makeInstruction(ir::Opcode::slotAddress, entry->span);
            slot.integerValue = static_cast<int64_t>(*entryType);
            slot.operands = { block.id, offset.id };
            slot.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto address = *slot.result;
            emit(std::move(slot));
            recordValue(address, ValueOwnership::trivial);
            const auto place = emitValue(ir::Opcode::integerAddress, *entryType, ir::ValueCategory::address,
                                         { address.id }, entry->span);
            const auto keyPlace = emitValue(ir::Opcode::fieldAddress, keyType, ir::ValueCategory::address,
                                            { place.id }, entry->key->span, (*fields)[0]);
            emitRawStore(*key, keyPlace, entry->key->span);
            if (valueType == model->types().voidType()) continue;
            const auto valuePlace = emitValue(ir::Opcode::fieldAddress, valueType, ir::ValueCategory::address,
                                              { place.id }, entry->value->span, (*fields)[1]);
            emitRawStore(*value, valuePlace, entry->value->span);
        }
        auto instruction = makeInstruction(ir::Opcode::call, expression->span);
        instruction.callee = instanceFunctions[*instance];
        instruction.operands = { ownedReceiver->id, block.id, count.id };
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    std::optional<ir::Value> lowerContextualCase(
            const syntax::ContextualCaseExprSyntax::Ptr& expression) {
        const auto caseSymbol = model->referencedSymbol(expression);
        const auto type = typeOf(expression);
        if (!caseSymbol.has_value() || !type.has_value()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "contextual enum case has no typed target");
            return std::nullopt;
        }
        return lowerEnumArguments(
                *type, *caseSymbol, expression->arguments, expression->span);
    }

    // The case a role binds (decision 0024): present/success is the first role case, absent/failure
    // the second. Never found by spelling.
    std::optional<semantic::SymbolId> findRoleCase(
            typing::TypeId type,
            ir::EnumerationRole role,
            bool second) const {
        const auto enumeration = std::find_if(
                module->enumerations.begin(),
                module->enumerations.end(),
                [type](const auto& candidate) { return candidate.type == type; });
        if (enumeration == module->enumerations.end() || enumeration->role != role) return std::nullopt;
        const auto index = second ? enumeration->secondRoleCase : enumeration->firstRoleCase;
        if (index >= enumeration->cases.size()) return std::nullopt;
        return enumeration->cases[index].symbol;
    }

    std::optional<semantic::SymbolId> findEnumCase(
            typing::TypeId type,
            const std::string& name) const {
        const auto enumeration = std::find_if(
                module->enumerations.begin(),
                module->enumerations.end(),
                [type](const auto& candidate) { return candidate.type == type; });
        if (enumeration == module->enumerations.end()) return std::nullopt;
        const auto enumCase = std::find_if(
                enumeration->cases.begin(),
                enumeration->cases.end(),
                [&name](const auto& candidate) { return candidate.name == name; });
        return enumCase == enumeration->cases.end()
                ? std::nullopt
                : std::optional<semantic::SymbolId>(enumCase->symbol);
    }

    const ir::EnumCaseDefinition* enumCaseDefinition(
            typing::TypeId type,
            semantic::SymbolId caseSymbol) const {
        const auto enumeration = std::find_if(
                module->enumerations.begin(),
                module->enumerations.end(),
                [type](const auto& candidate) { return candidate.type == type; });
        if (enumeration == module->enumerations.end()) return nullptr;
        const auto found = std::find_if(
                enumeration->cases.begin(),
                enumeration->cases.end(),
                [caseSymbol](const auto& candidate) { return candidate.symbol == caseSymbol; });
        return found == enumeration->cases.end() ? nullptr : &*found;
    }

    std::optional<ir::Value> emitEnumConstruction(
            typing::TypeId type,
            semantic::SymbolId caseSymbol,
            const std::vector<ir::Value>& payloads,
            SourceSpan span) {
        const auto* enumCase = enumCaseDefinition(type, caseSymbol);
        if (enumCase == nullptr || enumCase->payloadTypes.size() != payloads.size()) {
            report(
                    DiagnosticId::missingSymbol,
                    span,
                    "enum construction has no matching concrete case definition");
            return std::nullopt;
        }
        auto instruction = makeInstruction(ir::Opcode::constructEnum, span);
        instruction.result = makeValue(type, ir::ValueCategory::value);
        instruction.symbol = caseSymbol;
        for (size_t index = 0; index < payloads.size(); ++index) {
            auto converted = coerce(payloads[index], enumCase->payloadTypes[index], span);
            if (!converted.has_value()) return std::nullopt;
            if (requiresDestroy(enumCase->payloadTypes[index])) {
                converted = acquireOwned(*converted, span);
                if (!converted.has_value()) return std::nullopt;
            }
            instruction.operands.push_back(converted->id);
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    std::optional<ir::Value> lowerEnumArguments(
            typing::TypeId type,
            semantic::SymbolId caseSymbol,
            const std::vector<syntax::CallArgumentSyntax::Ptr>& arguments,
            SourceSpan span) {
        const auto* definition = enumCaseDefinition(type, caseSymbol);
        if (definition == nullptr || definition->payloadTypes.size() != arguments.size()) {
            report(
                    DiagnosticId::missingSymbol,
                    span,
                    "enum construction has no matching concrete case definition");
            return std::nullopt;
        }
        std::vector<ir::Value> payloads;
        for (size_t index = 0; index < arguments.size(); ++index) {
            const auto payload = prepareOperand(
                    arguments[index]->value, definition->payloadTypes[index],
                    movesArgument(*arguments[index]));
            if (!payload.has_value()) return std::nullopt;
            payloads.push_back(*payload);
        }
        return emitEnumConstruction(type, caseSymbol, payloads, span);
    }

    ir::Value emitExtractPayload(
            typing::TypeId payloadType,
            ir::ValueId operandId,
            semantic::SymbolId caseSymbol,
            int64_t payloadIndex,
            SourceSpan span) {
        auto instruction = makeInstruction(ir::Opcode::extractPayload, span);
        instruction.result = makeValue(payloadType, ir::ValueCategory::value);
        instruction.operands = { operandId };
        instruction.symbol = caseSymbol;
        instruction.integerValue = payloadIndex;
        const auto result = *instruction.result;
        emit(std::move(instruction));
        return result;
    }

    std::optional<ir::Value> lowerPropagation(
            const syntax::PropagateExprSyntax::Ptr& expression) {
        auto operand = lowerExpression(expression->operand);
        const auto resultType = typeOf(expression);
        if (!operand.has_value() || !resultType.has_value()) return std::nullopt;
        const auto* operandType = model->types().type(operand->type);
        const bool isResult = model->types().isResult(operandType);
        const auto role = isResult ? ir::EnumerationRole::result : ir::EnumerationRole::optional;
        const auto successCase = findRoleCase(operand->type, role, false);
        const auto failureCase = findRoleCase(operand->type, role, true);
        const auto returnedFailureCase = findRoleCase(currentFunction().resultType, role, true);
        if (operandType == nullptr || !successCase.has_value() ||
            !failureCase.has_value() || !returnedFailureCase.has_value()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "propagation requires matching concrete enum cases");
            return std::nullopt;
        }

        operand = acquireOwned(*operand, expression->operand->span);
        if (!operand.has_value()) return std::nullopt;
        const bool neverSuccess = *resultType == model->types().neverType();
        const bool neverFailure = isResult &&
                operandType->arguments[1] == model->types().neverType();
        const bool hasPayload = *resultType != model->types().voidType() && !neverSuccess;
        const auto resultAddress = hasPayload
                ? std::optional<ir::Value>(emitValue(
                        ir::Opcode::stackAllocate,
                        *resultType,
                        ir::ValueCategory::address,
                        {},
                        expression->span))
                : std::nullopt;
        const auto successBlock = createBlock("propagate.success");
        const auto failureBlock = createBlock("propagate.failure");
        const auto mergeBlock = neverSuccess
                ? std::nullopt
                : std::optional<ir::BlockId>(createBlock("propagate.merge"));
        auto dispatch = makeInstruction(ir::Opcode::switchPattern, expression->span);
        dispatch.operands = { operand->id };
        for (const auto [caseSymbol, target] :
             { std::pair { *successCase, successBlock },
               std::pair { *failureCase, failureBlock } }) {
            const auto* definition = enumCaseDefinition(operand->type, caseSymbol);
            if (definition == nullptr) {
                report(
                        DiagnosticId::missingSymbol,
                        expression->span,
                        "propagation case has no concrete definition");
                return std::nullopt;
            }
            ir::Pattern pattern;
            pattern.kind = ir::PatternKind::enumCase;
            pattern.type = operand->type;
            pattern.symbol = caseSymbol;
            for (const auto payloadType : definition->payloadTypes) {
                ir::Pattern wildcard;
                wildcard.kind = ir::PatternKind::wildcard;
                wildcard.type = payloadType;
                pattern.payloads.push_back(std::move(wildcard));
            }
            dispatch.switchCases.push_back(ir::SwitchCase { pattern, target });
        }
        emit(std::move(dispatch));

        switchToBlock(failureBlock);
        if (neverFailure) {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
        } else {
            std::vector<ir::Value> failurePayloads;
            if (isResult) {
                const auto error = emitExtractPayload(
                        operandType->arguments[1],
                        operand->id,
                        *failureCase,
                        0,
                        expression->span);
                if (requiresDestroy(error.type)) recordValue(error, ValueOwnership::owned);
                failurePayloads.push_back(error);
            }
            const auto failure = emitEnumConstruction(
                    currentFunction().resultType,
                    *returnedFailureCase,
                    failurePayloads,
                    expression->span);
            if (!failure.has_value()) return std::nullopt;
            emitReturnValue(*failure, expression->span, expression->span);
        }

        switchToBlock(successBlock);
        if (neverSuccess) {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
            return std::nullopt;
        }
        const auto success = emitExtractPayload(
                *resultType,
                operand->id,
                *successCase,
                0,
                expression->span);
        if (requiresDestroy(success.type)) recordValue(success, ValueOwnership::owned);
        if (resultAddress.has_value()) {
            const auto owned = acquireOwned(success, expression->span);
            if (!owned.has_value()) return std::nullopt;
            emitRawStore(*owned, *resultAddress, expression->span);
        }
        emitBranch(*mergeBlock, expression->span);

        switchToBlock(*mergeBlock);
        if (!resultAddress.has_value()) return emitUnit(expression->span);
        if (requiresDestroy(*resultType)) {
            registerOwnedStorage(*resultAddress);
            return emitValue(
                    ir::Opcode::take,
                    *resultType,
                    ir::ValueCategory::value,
                    { resultAddress->id },
                    expression->span);
        }
        return emitValue(
                ir::Opcode::load,
                *resultType,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }

    // True when a pattern binds a noncopyable value, so the match must move it out of the
    // scrutinee instead of copying it.
    bool patternBindsNoncopyable(const syntax::PatternPtr& pattern) const {
        if (pattern == nullptr) return false;
        if (pattern->kind == syntax::Kind::bindingPattern) {
            const auto type = typeOf(pattern);
            return type.has_value() && !copyableInBody(pattern, *type);
        }
        if (pattern->kind != syntax::Kind::enumCasePattern) return false;
        const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        for (const auto& argument : enumPattern->arguments) {
            if (patternBindsNoncopyable(argument->pattern)) return true;
        }
        return false;
    }

    std::optional<ir::Value> lowerMatch(const syntax::MatchExprSyntax::Ptr& expression) {
        bool movesPayloads = false;
        for (const auto& arm : expression->arms) {
            movesPayloads = movesPayloads || patternBindsNoncopyable(arm->pattern);
        }
        // With a moving pattern the scrutinee needs storage to move payloads out of: the place
        // of `match consume x`, or a slot that takes over an owned temporary. Whatever the arms
        // do not take is destroyed with that storage.
        std::optional<ir::Value> scrutineeAddress;
        std::optional<ir::Value> scrutinee;
        if (movesPayloads) {
            auto operand = expression->scrutinee;
            if (operand->kind == syntax::Kind::accessExpr) {
                const auto access = std::static_pointer_cast<syntax::AccessExprSyntax>(operand);
                if (access->marker == nullptr || access->marker->kind != kwConsume) {
                    report(
                            DiagnosticId::ownershipViolation,
                            operand->span,
                            "only 'consume' can move a noncopyable payload out of a place");
                    return std::nullopt;
                }
                operand = access->operand;
            }
            if (isAddressable(operand)) {
                scrutineeAddress = lowerAddress(operand);
            } else {
                const auto temporary = lowerExpression(expression->scrutinee);
                if (!temporary.has_value()) return std::nullopt;
                const auto owned = acquireOwned(*temporary, expression->scrutinee->span);
                if (!owned.has_value()) return std::nullopt;
                scrutineeAddress = emitValue(
                        ir::Opcode::stackAllocate,
                        owned->type,
                        ir::ValueCategory::address,
                        {},
                        expression->scrutinee->span);
                emitRawStore(*owned, *scrutineeAddress, expression->scrutinee->span);
                registerOwnedStorage(*scrutineeAddress);
            }
            if (!scrutineeAddress.has_value()) return std::nullopt;
            scrutinee = emitValue(
                    ir::Opcode::load,
                    scrutineeAddress->type,
                    ir::ValueCategory::value,
                    { scrutineeAddress->id },
                    expression->scrutinee->span);
        } else {
            scrutinee = lowerExpression(expression->scrutinee);
        }
        const auto resultType = typeOf(expression);
        if (!scrutinee.has_value() || !resultType.has_value()) return std::nullopt;

        // A discarded value needs no result: each branch's value is discarded in its own block.
        const auto producesResult = *resultType != model->types().voidType() &&
                *resultType != model->types().neverType() &&
                !model->isValueDiscarded(expression);
        const auto resultAddress = producesResult
                ? std::optional<ir::Value>(emitValue(
                        ir::Opcode::stackAllocate,
                        *resultType,
                        ir::ValueCategory::address,
                        {},
                        expression->span))
                : std::optional<ir::Value>();

        // A wildcard, binding or unit arm matches every value, so no arm after it can run: the dispatch
        // ends with it, as the IR verifier requires of a top-level wildcard, and the arms after it, which
        // type checking has already checked, are not lowered.
        size_t liveArms = expression->arms.size();
        for (size_t index = 0; index < expression->arms.size(); ++index) {
            if (catchesEveryValue(expression->arms[index]->pattern)) {
                liveArms = index + 1;
                break;
            }
        }

        std::vector<ir::BlockId> armBlocks;
        for (size_t index = 0; index < liveArms; ++index) {
            armBlocks.push_back(createBlock("match.arm." + std::to_string(index)));
        }
        const auto mergeBlock = createBlock("match.merge");

        auto dispatch = makeInstruction(ir::Opcode::switchPattern, expression->span);
        dispatch.operands = { scrutinee->id };
        for (size_t index = 0; index < liveArms; ++index) {
            const auto pattern = lowerPattern(expression->arms[index]->pattern);
            if (!pattern.has_value()) return std::nullopt;
            dispatch.switchCases.push_back(ir::SwitchCase { *pattern, armBlocks[index] });
        }
        emit(std::move(dispatch));

        bool hasMergePredecessor = false;
        for (size_t index = 0; index < liveArms; ++index) {
            const auto& arm = expression->arms[index];
            switchToBlock(armBlocks[index]);
            DebugScopeGuard armDebugScope(
                    currentDebugScope,
                    debugScopeForNode(arm, currentFunctionId));
            pushScope();
            bindPattern(arm->pattern, *scrutinee, scrutineeAddress);
            const auto value = lowerExpression(arm->body);
            if (currentBlockTerminated()) {
                discardScopeAfterTerminator();
                continue;
            }
            if (resultAddress.has_value()) {
                if (value.has_value()) {
                    emitStore(*value, *resultAddress, arm->body->span);
                } else {
                    report(
                            DiagnosticId::missingType,
                            arm->body->span,
                            "value-producing match arm did not lower a value");
                }
            }
            cleanupAndPopScope(arm->span);
            emitBranch(mergeBlock, arm->span);
            hasMergePredecessor = true;
        }

        switchToBlock(mergeBlock);
        if (!hasMergePredecessor) {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
            return std::nullopt;
        }
        if (!resultAddress.has_value()) return emitUnit(expression->span);
        registerOwnedStorage(*resultAddress);
        // A noncopyable result moves out of its slot; loading it would copy it.
        return emitValue(
                copyableInBody(expression, resultAddress->type) ? ir::Opcode::load : ir::Opcode::take,
                resultAddress->type,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }

    // The top-level patterns that lowerPattern() makes a wildcard.
    static bool catchesEveryValue(const syntax::PatternPtr& pattern) {
        return pattern != nullptr &&
                (pattern->kind == syntax::Kind::wildcardPattern || pattern->kind == syntax::Kind::unitPattern ||
                 pattern->kind == syntax::Kind::bindingPattern);
    }

    std::optional<ir::Pattern> lowerPattern(const syntax::PatternPtr& pattern) {
        if (pattern == nullptr) return std::nullopt;
        const auto type = typeOf(pattern);
        if (!type.has_value()) {
            report(DiagnosticId::missingType, pattern->span, "pattern has no type");
            return std::nullopt;
        }

        ir::Pattern result;
        result.type = *type;
        switch (pattern->kind) {
            case syntax::Kind::wildcardPattern:
            case syntax::Kind::unitPattern:
            case syntax::Kind::bindingPattern:
                result.kind = ir::PatternKind::wildcard;
                break;
            case syntax::Kind::literalPattern: {
                const auto literal = std::static_pointer_cast<syntax::LiteralPatternSyntax>(pattern);
                if (literal->literal == nullptr) return std::nullopt;
                switch (literal->literal->kind) {
                    case decimalLiteral:
                        result.kind = typing::isFloating(model->types().type(*type)->kind)
                                ? ir::PatternKind::floatingLiteral : ir::PatternKind::integerLiteral;
                        result.integerValue = literal->literal->intValue;
                        if (result.kind == ir::PatternKind::floatingLiteral) {
                            result.text = literal->literal->rawValue;
                        }
                        break;
                    case floatLiteral:
                        result.kind = ir::PatternKind::floatingLiteral;
                        result.text = literal->literal->rawValue;
                        break;
                    case booleanLiteral:
                        result.kind = ir::PatternKind::booleanLiteral;
                        result.integerValue = literal->literal->rawValue == Literals::TRUE ? 1 : 0;
                        break;
                    case stringLiteral:
                        result.kind = ir::PatternKind::stringLiteral;
                        result.text = literal->literal->rawValue;
                        break;
                    case byteLiteral:
                        result.kind = ir::PatternKind::byteLiteral;
                        result.integerValue = literal->literal->intValue;
                        break;
                    case nilLiteral: {
                        const auto none = findRoleCase(*type, ir::EnumerationRole::optional, true);
                        if (!none.has_value()) return std::nullopt;
                        result.kind = ir::PatternKind::enumCase;
                        result.symbol = *none;
                        break;
                    }
                    default:
                        report(
                                DiagnosticId::unsupportedSyntax,
                                pattern->span,
                                "literal pattern is not supported by IR lowering");
                        return std::nullopt;
                }
                break;
            }
            case syntax::Kind::enumCasePattern: {
                const auto enumPattern =
                        std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
                const auto caseSymbol = model->referencedSymbol(enumPattern);
                if (!caseSymbol.has_value()) {
                    report(
                            DiagnosticId::missingSymbol,
                            pattern->span,
                            "enum pattern has no resolved case symbol");
                    return std::nullopt;
                }
                result.kind = ir::PatternKind::enumCase;
                result.symbol = *caseSymbol;
                for (const auto& argument : enumPattern->arguments) {
                    const auto payload = lowerPattern(argument->pattern);
                    if (!payload.has_value()) return std::nullopt;
                    result.payloads.push_back(*payload);
                }
                break;
            }
            default:
                report(
                        DiagnosticId::unsupportedSyntax,
                        pattern->span,
                        "pattern kind is not supported by IR lowering");
                return std::nullopt;
        }
        return result;
    }

    // A binding owns its value from here to the end of the scope that holds it.
    void storeBinding(
            const syntax::PatternPtr& pattern,
            semantic::SymbolId symbol,
            ir::Value owned) {
        const auto address = emitValue(
                ir::Opcode::stackAllocate,
                owned.type,
                ir::ValueCategory::address,
                {},
                pattern->span,
                symbol);
        const auto debugVariable = ensureDebugVariable(symbol, currentFunctionId);
        emitRawStore(owned, address, pattern->span, symbol, false, debugVariable);
        slots[symbol] = address;
        registerOwnedStorage(address);
    }

    // Moves the payloads that `pattern` binds and cannot copy out of the enum stored at
    // `address`, leaving each emptied so the storage can still be destroyed whole.
    void bindMovedPayloads(const syntax::PatternPtr& pattern, ir::Value address) {
        if (pattern == nullptr || pattern->kind != syntax::Kind::enumCasePattern) return;
        const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        const auto caseSymbol = model->referencedSymbol(enumPattern);
        if (!caseSymbol.has_value()) return;
        for (size_t index = 0; index < enumPattern->arguments.size(); ++index) {
            const auto& payloadPattern = enumPattern->arguments[index]->pattern;
            if (payloadPattern == nullptr ||
                (payloadPattern->kind != syntax::Kind::bindingPattern &&
                 payloadPattern->kind != syntax::Kind::enumCasePattern)) {
                continue;
            }
            const auto payloadType = typeOf(payloadPattern);
            if (!payloadType.has_value()) continue;
            auto instruction = makeInstruction(ir::Opcode::payloadAddress, payloadPattern->span);
            instruction.result = makeValue(*payloadType, ir::ValueCategory::address);
            instruction.operands = { address.id };
            instruction.symbol = *caseSymbol;
            instruction.integerValue = static_cast<int64_t>(index);
            const auto payloadAddress = *instruction.result;
            emit(std::move(instruction));
            if (payloadPattern->kind == syntax::Kind::bindingPattern) {
                const auto symbol = model->semanticModel()->declaredSymbol(payloadPattern);
                if (!symbol.has_value()) continue;
                if (copyableInBody(payloadPattern, *payloadType)) {
                    // A copyable payload beside a moved one still binds by copy.
                    const auto loaded = emitValue(
                            ir::Opcode::load,
                            *payloadType,
                            ir::ValueCategory::value,
                            { payloadAddress.id },
                            payloadPattern->span);
                    bindPattern(payloadPattern, loaded);
                    continue;
                }
                const auto taken = emitValue(
                        ir::Opcode::take,
                        *payloadType,
                        ir::ValueCategory::value,
                        { payloadAddress.id },
                        payloadPattern->span);
                const auto owned = acquireOwned(taken, payloadPattern->span);
                if (owned.has_value()) storeBinding(payloadPattern, *symbol, *owned);
            } else {
                bindMovedPayloads(payloadPattern, payloadAddress);
            }
        }
    }

    void bindPattern(
            const syntax::PatternPtr& pattern,
            ir::Value value,
            std::optional<ir::Value> address = std::nullopt) {
        if (pattern == nullptr) return;
        if (address.has_value() && patternBindsNoncopyable(pattern)) {
            bindMovedPayloads(pattern, *address);
            return;
        }
        if (pattern->kind == syntax::Kind::bindingPattern) {
            const auto symbol = model->semanticModel()->declaredSymbol(pattern);
            if (!symbol.has_value()) return;
            if (!copyableInBody(pattern, value.type)) {
                // Type checking rejects this; a copy here would destroy the value twice.
                report(
                        DiagnosticId::ownershipViolation,
                        pattern->span,
                        "a pattern cannot copy a noncopyable value");
                return;
            }
            if (requiresDestroy(value.type)) {
                value = emitValue(
                        ir::Opcode::copyValue,
                        value.type,
                        ir::ValueCategory::value,
                        { value.id },
                        pattern->span);
            }
            const auto owned = acquireOwned(value, pattern->span);
            if (!owned.has_value()) return;
            const auto address = emitValue(
                    ir::Opcode::stackAllocate,
                    value.type,
                    ir::ValueCategory::address,
                    {},
                    pattern->span,
                    symbol);
                    const auto debugVariable = ensureDebugVariable(*symbol, currentFunctionId);
                    emitRawStore(
                        *owned,
                        address,
                        pattern->span,
                        symbol,
                        false,
                        debugVariable);
            slots[*symbol] = address;
                    registerOwnedStorage(address);
            return;
        }
        if (pattern->kind != syntax::Kind::enumCasePattern) return;

        const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        const auto caseSymbol = model->referencedSymbol(enumPattern);
        if (!caseSymbol.has_value()) return;
        for (size_t index = 0; index < enumPattern->arguments.size(); ++index) {
            const auto& payloadPattern = enumPattern->arguments[index]->pattern;
            if (payloadPattern->kind != syntax::Kind::bindingPattern &&
                payloadPattern->kind != syntax::Kind::enumCasePattern) {
                continue;
            }
            const auto payloadType = typeOf(payloadPattern);
            if (!payloadType.has_value()) continue;
            const auto payload = emitExtractPayload(
                    *payloadType,
                    value.id,
                    *caseSymbol,
                    static_cast<int64_t>(index),
                    payloadPattern->span);
            bindPattern(payloadPattern, payload);
        }
    }

    std::optional<ir::Value> lowerIf(const syntax::IfExprSyntax::Ptr& expression) {
        const auto condition = lowerExpression(expression->condition);
        const auto resultType = typeOf(expression);
        if (!condition.has_value() || !resultType.has_value()) return std::nullopt;

        // A discarded value needs no result: each branch's value is discarded in its own block.
        const auto producesResult = *resultType != model->types().voidType() &&
                *resultType != model->types().neverType() &&
                !model->isValueDiscarded(expression);
        const auto resultAddress = producesResult
                ? std::optional<ir::Value>(emitValue(
                        ir::Opcode::stackAllocate,
                        *resultType,
                        ir::ValueCategory::address,
                        {},
                        expression->span))
                : std::optional<ir::Value>();

        const auto thenBlock = createBlock("if.then");
        const auto elseBlock = createBlock("if.else");
        const auto mergeBlock = createBlock("if.merge");
        emitConditionalBranch(*condition, thenBlock, elseBlock, expression->condition->span);

        bool hasMergePredecessor = false;
        switchToBlock(thenBlock);
        pushScope();
        const auto thenValue = lowerBlock(expression->thenBranch);
        if (!currentBlockTerminated()) {
            if (resultAddress.has_value()) {
                if (thenValue.has_value()) {
                    emitStore(*thenValue, *resultAddress, expression->thenBranch->span);
                } else {
                    report(
                            DiagnosticId::missingType,
                            expression->thenBranch->span,
                            "value-producing if branch did not lower a value");
                }
            }
            cleanupAndPopScope(expression->thenBranch->span);
            emitBranch(mergeBlock, expression->thenBranch->span);
            hasMergePredecessor = true;
        } else {
            discardScopeAfterTerminator();
        }

        switchToBlock(elseBlock);
        pushScope();
        const auto elseValue = lowerExpression(expression->elseBranch);
        if (!currentBlockTerminated()) {
            if (resultAddress.has_value()) {
                if (elseValue.has_value()) {
                    emitStore(*elseValue, *resultAddress, expression->elseBranch->span);
                } else {
                    report(
                            DiagnosticId::missingType,
                            expression->span,
                            "value-producing else branch did not lower a value");
                }
            }
            cleanupAndPopScope(expression->span);
            emitBranch(mergeBlock, expression->span);
            hasMergePredecessor = true;
        } else {
            discardScopeAfterTerminator();
        }

        switchToBlock(mergeBlock);
        if (!hasMergePredecessor) {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
            return std::nullopt;
        }
        if (!resultAddress.has_value()) return emitUnit(expression->span);
        registerOwnedStorage(*resultAddress);
        // A noncopyable result moves out of its slot; loading it would copy it.
        return emitValue(
                copyableInBody(expression, resultAddress->type) ? ir::Opcode::load : ir::Opcode::take,
                resultAddress->type,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }

    void lowerLoopControl(const syntax::NodePtr& statement) {
        if (loops.empty()) {
            report(DiagnosticId::unsupportedSyntax, statement->span,
                    "loop control requires an enclosing loop");
            return;
        }
        auto& loop = loops.back();
        for (size_t depth = scopes.size(); depth > loop.scopeDepth; --depth) {
            emitScopeCleanup(scopes[depth - 1], statement->span, false);
        }
        const auto isBreak = statement->kind == syntax::Kind::breakStmt;
        if (isBreak) loop.hasBreak = true;
        emitBranch(isBreak ? loop.exit : loop.header, statement->span);
    }

    void lowerWhile(const syntax::WhileStmtSyntax::Ptr& statement) {
        const auto headerBlock = createBlock("while.header");
        const auto bodyBlock = createBlock("while.body");
        const auto exitBlock = createBlock("while.exit");
        loops.push_back({ headerBlock, exitBlock, scopes.size() });
        emitBranch(headerBlock, statement->span);

        switchToBlock(headerBlock);
        pushScope();
        const auto condition = lowerExpression(statement->condition);
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            switchToBlock(bodyBlock);
            emit(makeInstruction(ir::Opcode::unreachable, statement->body->span, true));
            switchToBlock(exitBlock);
            if (!loops.back().hasBreak) {
                emit(makeInstruction(ir::Opcode::unreachable, statement->span, true));
            }
            loops.pop_back();
            return;
        }
        if (condition.has_value()) {
            cleanupAndPopScope(statement->condition->span);
            emitConditionalBranch(
                    *condition,
                    bodyBlock,
                    exitBlock,
                    statement->condition->span);
        } else {
            cleanupAndPopScope(statement->condition->span);
            report(
                    DiagnosticId::missingType,
                    statement->condition->span,
                    "while condition did not lower a Boolean value");
            emit(makeInstruction(ir::Opcode::unreachable, statement->condition->span));
        }

        switchToBlock(bodyBlock);
        pushScope();
        lowerBlock(statement->body);
        if (!currentBlockTerminated()) {
            cleanupAndPopScope(statement->body->span);
            emitBranch(headerBlock, statement->body->span);
        } else {
            discardScopeAfterTerminator();
        }

        switchToBlock(exitBlock);
        loops.pop_back();
    }

    std::optional<ir::Value> lowerCall(const syntax::CallExprSyntax::Ptr& expression) {
        const auto target = model->callTarget(expression);
        if (!target.has_value()) {
            const auto calleeType = typeOf(expression->callee);
            const auto* record = calleeType.has_value() ? model->types().type(*calleeType) : nullptr;
            if (record != nullptr && record->kind == typing::TypeKind::function) {
                return lowerFunctionValueCall(expression, *calleeType);
            }
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "call target has no resolved symbol");
            return std::nullopt;
        }

        const auto* targetSymbol = model->semanticModel()->symbol(*target);
        if (targetSymbol != nullptr &&
            targetSymbol->kind == semantic::SymbolKind::synthesizedInitializer) {
            return lowerStructConstruction(expression, *target);
        }
        if (targetSymbol != nullptr &&
            (targetSymbol->kind == semantic::SymbolKind::enumCase ||
             targetSymbol->kind == semantic::SymbolKind::builtinEnumCase)) {
            const auto type = typeOf(expression);
            if (!type.has_value()) return std::nullopt;
            return lowerEnumArguments(
                    *type, *target, expression->arguments, expression->span);
        }
        if (targetSymbol != nullptr &&
            targetSymbol->kind == semantic::SymbolKind::builtinFunction &&
            intrinsic::isLayoutIntrinsic(targetSymbol->name)) {
            return lowerLayoutIntrinsic(expression, targetSymbol->name);
        }
        if (targetSymbol != nullptr &&
            targetSymbol->kind == semantic::SymbolKind::builtinMember &&
            targetSymbol->name == "utf8") {
            return lowerStringUtf8(expression);
        }
        if (const auto builtin = lowerBuiltinEqualsRequirement(expression, targetSymbol)) return *builtin;
        const auto calleeId = calledFunction(expression, *target, targetSymbol);
        std::vector<ir::Parameter> divergingParameters;
        if (!calleeId.has_value()) {
            // Evaluate the checked operands with normal ownership even when no call can be emitted.
            const auto* generic = model->genericCall(expression);
            const auto* declared = model->callable(*target);
            const auto* parameters = generic == nullptr ? nullptr : model->genericParameters(generic->callee);
            const bool diverges = std::any_of(expression->arguments.begin(), expression->arguments.end(),
                    [&](const auto& argument) { return typeOf(argument->value) == model->types().neverType(); });
            if (!diverges || targetSymbol == nullptr || !targetSymbol->callable.has_value() ||
                declared == nullptr || parameters == nullptr || parameters->size() != generic->arguments.size() ||
                declared->parameters.size() != targetSymbol->callable->parameters.size()) {
                report(DiagnosticId::missingSymbol, expression->span, "call target has no IR function declaration");
                return std::nullopt;
            }
            std::unordered_map<semantic::SymbolId, typing::TypeId> substitution;
            for (size_t index = 0; index < parameters->size(); ++index) {
                substitution.emplace((*parameters)[index], substituted(generic->arguments[index]));
            }
            const auto add = [&](typing::TypeId type, syntax::AccessEffect effect) {
                ir::Parameter parameter;
                parameter.value.type = model->types().substitute(type, substitution);
                parameter.value.category = effect == syntax::AccessEffect::inout ||
                        effect == syntax::AccessEffect::initializing
                        ? ir::ValueCategory::address : ir::ValueCategory::value;
                parameter.isConsuming = effect == syntax::AccessEffect::consuming;
                divergingParameters.push_back(parameter);
            };
            if (declared->receiverType.has_value() && targetSymbol->callable->receiver.has_value()) {
                add(*declared->receiverType, targetSymbol->callable->receiver->access);
            }
            for (size_t index = 0; index < declared->parameters.size(); ++index) {
                add(declared->parameters[index], targetSymbol->callable->parameters[index].access);
            }
        }

        // A method's receiver is the callee's first IR parameter, ahead of the
        // written arguments, and follows the same access convention as any other.
        struct Operand {
            syntax::ExprPtr value;
            SourceSpan span;
        };
        std::vector<Operand> operands;
        if (targetSymbol != nullptr && targetSymbol->kind == semantic::SymbolKind::method) {
            if (expression->callee->kind != syntax::Kind::memberExpr) {
                report(
                        DiagnosticId::unsupportedSyntax,
                        expression->span,
                        "a method call requires a receiver expression");
                return std::nullopt;
            }
            const auto member =
                    std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
            operands.push_back({member->base, member->base->span});
        }
        for (const auto& argument : expression->arguments) {
            operands.push_back({argument->value, argument->span});
        }

        std::vector<ir::ValueId> arguments;
        std::vector<ir::Value> consumedArguments;
        const auto& parameters = calleeId.has_value() ? module->functions[*calleeId].parameters : divergingParameters;
        const size_t receivers = targetSymbol != nullptr && targetSymbol->kind == semantic::SymbolKind::method ? 1 : 0;
        for (size_t index = 0; index < operands.size(); ++index) {
            const auto& operand = operands[index];
            const auto* parameter = index < parameters.size() ? &parameters[index] : nullptr;
            const auto value = lowerCallOperand(
                    operand.value,
                    operand.span,
                    parameter != nullptr && parameter->value.category == ir::ValueCategory::address,
                    parameter != nullptr && parameter->isConsuming,
                    parameter != nullptr && !parameter->acceptsAnyType
                            ? std::optional<typing::TypeId>(parameter->value.type)
                            : std::nullopt,
                    consumedArguments,
                    index < receivers || !model->isOwnedArgumentContext(*target, index - receivers) ||
                            movesArgument(*expression->arguments[index - receivers]));
            if (!value.has_value()) return std::nullopt;
            arguments.push_back(value->id);
        }
        if (!calleeId.has_value()) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "a diverging call argument unexpectedly returned");
            return std::nullopt;
        }
        auto instruction = makeInstruction(ir::Opcode::call, expression->span);
        instruction.callee = *calleeId;
        return finishCall(expression, std::move(instruction), std::move(arguments), consumedArguments);
    }

    // `t.equals(other: u)` in an instance whose `T` is a builtin conformer (decision 0029): the builtin `==`, since
    // the compiler's conformances have no witness method. Returns nothing for any other call.
    std::optional<std::optional<ir::Value>> lowerBuiltinEqualsRequirement(
            const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol* targetSymbol) {
        const auto requirement = model->equatableRequirement();
        if (targetSymbol == nullptr || !requirement.has_value() || targetSymbol->id != *requirement ||
            expression->callee->kind != syntax::Kind::memberExpr || expression->arguments.size() != 1) {
            return std::nullopt;
        }
        const auto base = std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base;
        const auto baseType = typeOf(base);
        const auto type = typeOf(expression);
        if (!baseType.has_value() || !type.has_value() || !model->isBuiltinEquatable(*baseType)) return std::nullopt;
        return lowerBuiltinBinary(equalEqual, base, expression->arguments[0]->value, *type, expression->span);
    }

    // The IR function of `requirement`'s witness for `base`: an instance when the witness is declared in a generic
    // type (decision 0022), else the witness's own function.
    std::optional<ir::FunctionId> witnessFunction(typing::TypeId base, semantic::SymbolId requirement) {
        const auto witness = model->witness(base, requirement);
        if (!witness.has_value()) return std::nullopt;
        if (model->genericParameters(*witness) != nullptr) {
            const auto* receiver = model->types().type(base);
            if (receiver == nullptr) return std::nullopt;
            const auto instance = model->instanceIndex(*witness, receiver->arguments);
            if (!instance.has_value() || *instance >= instanceFunctions.size() ||
                instanceFunctions[*instance] == ir::invalidFunctionId) {
                return std::nullopt;
            }
            return instanceFunctions[*instance];
        }
        const auto found = functions.find(*witness);
        if (found == functions.end()) return std::nullopt;
        return found->second;
    }

    // The IR function a direct call reaches: an instance for a call of a generic function, the
    // witness for a requirement call on a type parameter (decision 0021), else the target.
    std::optional<ir::FunctionId> calledFunction(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId target,
            const semantic::Symbol* targetSymbol) {
        if (const auto* call = model->genericCall(expression)) {
            std::vector<typing::TypeId> arguments;
            for (const auto argument : call->arguments) arguments.push_back(substituted(argument));
            const auto instance = model->instanceIndex(call->callee, arguments);
            if (!instance.has_value() || *instance >= instanceFunctions.size() ||
                instanceFunctions[*instance] == ir::invalidFunctionId) {
                return std::nullopt;
            }
            return instanceFunctions[*instance];
        }
        const auto* owner = targetSymbol != nullptr && targetSymbol->containingSymbol.has_value()
                ? model->semanticModel()->symbol(*targetSymbol->containingSymbol)
                : nullptr;
        if (owner != nullptr && owner->kind == semantic::SymbolKind::protocol &&
            expression->callee->kind == syntax::Kind::memberExpr) {
            const auto base = typeOf(std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base);
            return base.has_value() ? witnessFunction(*base, target) : std::nullopt;
        }
        return demandFunction(target);
    }

    // One argument of a call: an address for `inout` and `initializing`, a moved value for `consuming`,
    // otherwise a value coerced to the parameter type. An owned consumed value is added to
    // `consumedArguments`, to be acquired once every argument is evaluated.
    std::optional<ir::Value> lowerCallOperand(
            const syntax::ExprPtr& operand,
            SourceSpan span,
            bool expectsAddress,
            bool consumesValue,
            std::optional<typing::TypeId> parameterType,
            std::vector<ir::Value>& consumedArguments,
            bool movesSource = true) {
        std::optional<ir::Value> value;
        if (expectsAddress) {
            value = lowerAddress(operand);
        } else if (consumesValue && movesSource && isAddressable(operand)) {
            const auto address = lowerAddress(operand);
            if (address.has_value()) {
                value = emitValue(
                        ir::Opcode::take,
                        address->type,
                        ir::ValueCategory::value,
                        { address->id },
                        span);
            }
        } else {
            value = lowerExpression(operand);
        }
        if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
        if (!expectsAddress && parameterType.has_value()) {
            value = coerce(*value, *parameterType, operand->span);
            if (!value.has_value()) return std::nullopt;
        }
        if (consumesValue && requiresDestroy(value->type)) {
            value = prepareOwned(*value, span);
            if (!value.has_value()) return std::nullopt;
            consumedArguments.push_back(*value);
        }
        return value;
    }

    // `f(a, &b)` or `(s.f)(a)` (spec 5.6.1): the callee is evaluated first to a copy, then the arguments
    // by the function type's effects.
    std::optional<ir::Value> lowerFunctionValueCall(
            const syntax::CallExprSyntax::Ptr& expression,
            typing::TypeId calleeType) {
        const auto* record = model->types().type(calleeType);
        const auto parameters = record->arguments;
        const auto effects = record->effects;
        const auto callee = lowerExpression(expression->callee);
        if (!callee.has_value()) return std::nullopt;
        std::vector<ir::ValueId> arguments { callee->id };
        std::vector<ir::Value> consumedArguments;
        for (size_t index = 0; index < expression->arguments.size() && index < effects.size(); ++index) {
            const auto& argument = expression->arguments[index];
            const auto effect = effects[index];
            const auto value = lowerCallOperand(
                    argument->value,
                    argument->span,
                    effect == syntax::AccessEffect::inout || effect == syntax::AccessEffect::initializing,
                    effect == syntax::AccessEffect::consuming,
                    parameters[index],
                    consumedArguments);
            if (!value.has_value()) return std::nullopt;
            arguments.push_back(value->id);
        }
        return finishCall(
                expression,
                makeInstruction(ir::Opcode::callIndirect, expression->span),
                std::move(arguments),
                consumedArguments);
    }

    // Emits a call whose operands are lowered: acquires the consumed arguments, then gives the result
    // a value, or ends the block after a `Never` call.
    std::optional<ir::Value> finishCall(
            const syntax::CallExprSyntax::Ptr& expression,
            ir::Instruction instruction,
            std::vector<ir::ValueId> arguments,
            const std::vector<ir::Value>& consumedArguments) {
        for (const auto& argument : consumedArguments) {
            if (!acquireOwned(argument, expression->span).has_value()) return std::nullopt;
        }
        instruction.operands = std::move(arguments);
        const auto type = typeOf(expression);
        if (type.has_value() && *type != model->types().voidType() &&
            *type != model->types().neverType()) {
            instruction.result = makeValue(*type, ir::ValueCategory::value);
        }
        const auto result = instruction.result;
        emit(std::move(instruction));
        if (type == model->types().neverType()) {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
            return std::nullopt;
        }
        if (result.has_value()) {
            recordValue(
                *result,
                requiresDestroy(result->type)
                    ? ValueOwnership::owned
                    : ValueOwnership::trivial);
        }
        if (type == model->types().voidType()) return emitUnit(expression->span);
        return result;
    }

    // The layout intrinsics (spec 20.12.5). The size and alignment are queries of the backend's
    // layout; the other three treat an `Int` as the address of a `T` slot. The type argument is
    // substituted for the current instance, so a generic function gets the layout of its `T`.
    std::optional<ir::Value> lowerLayoutIntrinsic(
            const syntax::CallExprSyntax::Ptr& expression, const std::string& name) {
        if (name == "joyeer_intrinsic_callback_address") {
            const auto* reference = model->genericCall(expression);
            if (reference == nullptr) {
                report(DiagnosticId::missingSymbol, expression->span, "callback has no typed function reference");
                return std::nullopt;
            }
            std::vector<typing::TypeId> arguments;
            for (const auto argument : reference->arguments) arguments.push_back(substituted(argument));
            const auto instance = model->instanceIndex(reference->callee, arguments);
            if (!instance.has_value() || *instance >= instanceFunctions.size() ||
                instanceFunctions[*instance] == ir::invalidFunctionId) {
                report(DiagnosticId::missingSymbol, expression->span, "callback has no lowered instance");
                return std::nullopt;
            }
            auto instruction = makeInstruction(ir::Opcode::functionAddress, expression->span);
            instruction.callee = instanceFunctions[*instance];
            instruction.result = makeValue(model->types().functionType(
                    { model->types().intType() }, { syntax::AccessEffect::consuming }, model->types().voidType()),
                    ir::ValueCategory::value);
            const auto address = *instruction.result;
            emit(std::move(instruction));
            recordValue(address, ValueOwnership::trivial);
            return emitValue(ir::Opcode::pointerToInteger, model->types().intType(), ir::ValueCategory::value,
                             { address.id }, expression->span);
        }
        const auto written = model->layoutIntrinsicType(expression);
        if (!written.has_value()) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' needs one type argument");
            return std::nullopt;
        }
        const auto concrete = substituted(*written);
        // An uninhabited element has no slots. Generic code can describe its empty storage, but cannot load a value.
        if (concrete == model->types().neverType() && name == "joyeer_intrinsic_move_out") {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
            return std::nullopt;
        }
        const auto type = concrete == model->types().neverType() ? model->types().voidType() : concrete;
        if (name == "joyeer_intrinsic_stack_slot") {
            if (type == model->types().voidType()) {
                return emitValue(ir::Opcode::literal, model->types().intType(),
                                 ir::ValueCategory::value, {}, expression->span);
            }
            const auto slot = emitValue(ir::Opcode::stackAllocate, type, ir::ValueCategory::address,
                                        {}, expression->span);
            return emitValue(ir::Opcode::pointerToInteger, model->types().intType(), ir::ValueCategory::value,
                             { slot.id }, expression->span);
        }
        if (name == "joyeer_intrinsic_needs_destroy") {
            auto instruction = makeInstruction(ir::Opcode::literal, expression->span);
            instruction.integerValue = requiresDestroy(type) ? 1 : 0;
            instruction.result = makeValue(model->types().boolType(), ir::ValueCategory::value);
            const auto result = *instruction.result;
            emit(std::move(instruction));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
        if (name == "joyeer_intrinsic_clone_callback" || name == "joyeer_intrinsic_destroy_callback") {
            // A callback only exists for a type that needs one; otherwise the answer is 0 (decision 0025).
            const bool clone = name == "joyeer_intrinsic_clone_callback";
            if (clone ? !model->isCopyable(type) : model->isLinear(type)) {
                return emitValue(ir::Opcode::literal, model->types().intType(), ir::ValueCategory::value,
                                 {}, expression->span);
            }
            auto instruction = makeInstruction(ir::Opcode::typeHelper, expression->span);
            instruction.modifier = static_cast<uint8_t>(clone ? ir::TypeHelperKind::clone : ir::TypeHelperKind::destroy);
            instruction.integerValue = static_cast<int64_t>(type);
            instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto result = *instruction.result;
            emit(std::move(instruction));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
        if (name == "joyeer_intrinsic_key_kind") {
            // The runtime's key kinds (lib/Runtime/core/dictionaries.joyeer); any other type is an error, never 0,
            // which would select the runtime's byte comparison (decision 0027).
            int64_t kind = 0;
            switch (model->types().type(type)->kind) {
                case typing::TypeKind::integer: kind = 1; break;
                case typing::TypeKind::boolean: kind = 2; break;
                case typing::TypeKind::string: kind = 3; break;
                case typing::TypeKind::uint8: kind = 4; break;
                default: break;
            }
            if (kind == 0) {
                report(DiagnosticId::unsupportedSyntax, expression->span,
                       "'" + name + "' needs a dictionary key type, but '" + model->types().displayName(type) +
                               "' is not one");
                return std::nullopt;
            }
            auto constant = makeInstruction(ir::Opcode::literal, expression->span);
            constant.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            constant.integerValue = kind;
            const auto result = *constant.result;
            emit(std::move(constant));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
        if (name == "joyeer_intrinsic_place_address" || name == "joyeer_intrinsic_borrow_address") {
            if (expression->arguments.size() != 1) {
                report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes a place");
                return std::nullopt;
            }
            const auto place = lowerAddress(expression->arguments[0]->value);
            if (!place.has_value()) return std::nullopt;
            return emitValue(ir::Opcode::pointerToInteger, model->types().intType(), ir::ValueCategory::value,
                             { place->id }, expression->span);
        }
        if (name == "joyeer_intrinsic_stack_block") {
            if (expression->arguments.size() != 1) {
                report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes a count");
                return std::nullopt;
            }
            const auto count = lowerExpression(expression->arguments[0]->value);
            if (!count.has_value()) return std::nullopt;
            auto instruction = makeInstruction(ir::Opcode::stackBlock, expression->span);
            instruction.integerValue = static_cast<int64_t>(type);
            instruction.operands = { count->id };
            instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto result = *instruction.result;
            emit(std::move(instruction));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
        if (name == "joyeer_intrinsic_slot_address") {
            if (expression->arguments.size() != 2) {
                report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes a base and an index");
                return std::nullopt;
            }
            std::vector<ir::ValueId> operands;
            for (const auto& argument : expression->arguments) {
                auto value = lowerExpression(argument->value);
                if (!value.has_value()) return std::nullopt;
                value = coerce(*value, model->types().intType(), argument->value->span);
                if (!value.has_value()) return std::nullopt;
                operands.push_back(value->id);
            }
            auto instruction = makeInstruction(ir::Opcode::slotAddress, expression->span);
            instruction.integerValue = static_cast<int64_t>(type);
            instruction.operands = std::move(operands);
            instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto result = *instruction.result;
            emit(std::move(instruction));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
        if (name == "joyeer_intrinsic_destroy_range" || name == "joyeer_intrinsic_clone_at") {
            if (expression->arguments.size() != 2) {
                report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes two Int arguments");
                return std::nullopt;
            }
            std::vector<ir::ValueId> operands;
            for (const auto& argument : expression->arguments) {
                auto value = lowerExpression(argument->value);
                if (!value.has_value()) return std::nullopt;
                value = coerce(*value, model->types().intType(), argument->value->span);
                if (!value.has_value()) return std::nullopt;
                operands.push_back(value->id);
            }
            if (name == "joyeer_intrinsic_destroy_range" && model->isLinear(type)) {
                // A linear element is never destroyed: a nonempty range is a defect (decision 0025).
                const auto zero = emitValue(ir::Opcode::literal, model->types().intType(), ir::ValueCategory::value,
                                            {}, expression->span);
                const auto nonempty = emitValue(ir::ComparePredicate::notEqual, model->types().boolType(),
                                                ir::ValueCategory::value, { operands[1], zero.id }, expression->span);
                auto guard = makeInstruction(ir::Opcode::trapIf, expression->span);
                guard.operands = { nonempty.id };
                guard.text = "linear container dropped while not empty";
                emit(std::move(guard));
                return emitUnit(expression->span);
            }
            if (name == "joyeer_intrinsic_destroy_range" && !requiresDestroy(type)) return emitUnit(expression->span);
            auto instruction = makeInstruction(
                    name == "joyeer_intrinsic_clone_at" ? ir::Opcode::cloneAt : ir::Opcode::destroyRange,
                    expression->span);
            instruction.integerValue = static_cast<int64_t>(type);
            instruction.operands = std::move(operands);
            emit(std::move(instruction));
            return emitUnit(expression->span);
        }
        if (name == "joyeer_intrinsic_size_of" || name == "joyeer_intrinsic_align_of") {
            auto instruction = makeInstruction(
                    name == "joyeer_intrinsic_size_of" ? ir::Opcode::typeSize : ir::Opcode::typeAlignment,
                    expression->span);
            instruction.integerValue = static_cast<int64_t>(type);
            instruction.result = makeValue(model->types().intType(), ir::ValueCategory::value);
            const auto result = *instruction.result;
            emit(std::move(instruction));
            recordValue(result, ValueOwnership::trivial);
            return result;
        }
        if (expression->arguments.empty()) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "'" + name + "' takes an address");
            return std::nullopt;
        }
        auto address = lowerExpression(expression->arguments[0]->value);
        if (!address.has_value()) return std::nullopt;
        address = coerce(*address, model->types().intType(), expression->arguments[0]->value->span);
        if (!address.has_value()) return std::nullopt;
        const auto slot = emitValue(
                ir::Opcode::integerAddress, type, ir::ValueCategory::address, { address->id }, expression->span);
        if (name == "joyeer_intrinsic_move_out") {
            return emitValue(ir::Opcode::take, type, ir::ValueCategory::value, { slot.id }, expression->span);
        }
        if (name == "joyeer_intrinsic_destroy_at") {
            emitDestroyStorage(slot, expression->span);
            return emitUnit(expression->span);
        }
        if (expression->arguments.size() != 2) {
            report(DiagnosticId::unsupportedSyntax, expression->span, "move_in takes an address and a value");
            return std::nullopt;
        }
        const auto& argument = expression->arguments[1];
        std::optional<ir::Value> value;
        if (isAddressable(argument->value)) {
            const auto source = lowerAddress(argument->value);
            if (source.has_value()) {
                value = emitValue(ir::Opcode::take, source->type, ir::ValueCategory::value, { source->id },
                                  argument->span);
            }
        } else {
            value = lowerExpression(argument->value);
        }
        if (!value.has_value()) return std::nullopt;
        value = coerce(*value, type, argument->value->span);
        if (!value.has_value()) return std::nullopt;
        if (requiresDestroy(value->type)) {
            value = prepareOwned(*value, argument->span);
            if (!value.has_value()) return std::nullopt;
            if (!acquireOwned(*value, expression->span).has_value()) return std::nullopt;
        }
        emitRawStore(*value, slot, expression->span);
        return emitUnit(expression->span);
    }

    std::optional<ir::Value> lowerStringUtf8(
            const syntax::CallExprSyntax::Ptr& expression) {
        if (expression->callee->kind != syntax::Kind::memberExpr ||
            !expression->arguments.empty()) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "String.utf8 requires no arguments");
            return std::nullopt;
        }
        const auto member =
                std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
        const auto string = lowerExpression(member->base);
        const auto type = typeOf(expression);
        if (!string.has_value() || !type.has_value()) return std::nullopt;

        // The library array's `string_utf8` method, called on an inert zero `[UInt8]` like a literal method
        // (decision 0028 stage 3); the String is borrowed.
        const auto method = model->arrayRoleMethod(*type, semantic::LibraryRole::stringUtf8);
        const auto* record = model->types().type(*type);
        const auto instance = method.has_value() && record != nullptr
                ? model->instanceIndex(*method, record->arguments) : std::nullopt;
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            report(DiagnosticId::missingSymbol, expression->span, "String.utf8 has no lowered array method");
            return std::nullopt;
        }
        const auto receiverStorage = emitValue(ir::Opcode::stackAllocate, *type, ir::ValueCategory::address,
                                              {}, expression->span);
        auto zero = makeInstruction(ir::Opcode::zeroInitialize, expression->span);
        zero.operands = { receiverStorage.id };
        emit(std::move(zero));
        const auto receiver = emitValue(ir::Opcode::load, *type, ir::ValueCategory::value,
                                       { receiverStorage.id }, expression->span);
        recordValue(receiver, ValueOwnership::owned);
        const auto ownedReceiver = acquireOwned(receiver, expression->span);
        if (!ownedReceiver.has_value()) return std::nullopt;
        auto call = makeInstruction(ir::Opcode::call, expression->span);
        call.callee = instanceFunctions[*instance];
        call.operands = { ownedReceiver->id, string->id };
        call.result = makeValue(*type, ir::ValueCategory::value);
        const auto result = *call.result;
        emit(std::move(call));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    std::optional<ir::Value> lowerStructConstruction(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId initializer) {
        const auto* initializerSymbol = model->semanticModel()->symbol(initializer);
        const auto type = typeOf(expression);
        if (initializerSymbol == nullptr || !initializerSymbol->containingSymbol.has_value() ||
            !type.has_value()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "struct initializer has no containing structure or result type");
            return std::nullopt;
        }
        const auto structure = std::find_if(
                module->structures.begin(),
                module->structures.end(),
                [initializerSymbol, &type](const auto& candidate) {
                    return candidate.symbol == *initializerSymbol->containingSymbol &&
                            candidate.type == *type;
                });
        if (structure == module->structures.end()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "struct initializer has no IR structure definition");
            return std::nullopt;
        }

        std::vector<std::optional<ir::Value>> fieldValues(structure->fields.size());
        for (const auto& argument : expression->arguments) {
            if (argument->label == nullptr) continue;
            const auto field = std::find_if(
                    structure->fields.begin(),
                    structure->fields.end(),
                    [&argument](const auto& candidate) {
                        return candidate.name == argument->label->rawValue;
                    });
            if (field == structure->fields.end()) continue;
            const auto value = prepareOperand(
                    argument->value, field->type, movesArgument(*argument));
            if (!value.has_value()) return std::nullopt;
            fieldValues[static_cast<size_t>(
                    std::distance(structure->fields.begin(), field))] = *value;
        }

        for (size_t index = 0; index < fieldValues.size(); ++index) {
            if (fieldValues[index].has_value()) continue;
            if (structure->fields[index].name == ir::liveFieldName) {
                auto live = makeInstruction(ir::Opcode::literal, expression->span, true);
                live.integerValue = 1;
                live.result = makeValue(structure->fields[index].type, ir::ValueCategory::value);
                fieldValues[index] = *live.result;
                emit(std::move(live));
                recordValue(*fieldValues[index], ValueOwnership::trivial);
                continue;
            }
            const auto* fieldSymbol = model->semanticModel()->symbol(
                    structure->fields[index].symbol);
            const auto& declaration = fieldSymbol != nullptr &&
                    fieldSymbol->declaration.has_value()
                    ? model->semanticModel()->node(*fieldSymbol->declaration)
                    : syntax::NodePtr();
            if (declaration == nullptr ||
                declaration->kind != syntax::Kind::structFieldDecl) {
                report(
                        DiagnosticId::missingSymbol,
                        expression->span,
                        "omitted struct field has no default initializer");
                return std::nullopt;
            }
            const auto fieldDeclaration =
                    std::static_pointer_cast<syntax::StructFieldDeclSyntax>(declaration);
            const auto value = prepareOperand(
                    fieldDeclaration->initializer, structure->fields[index].type);
            if (!value.has_value()) {
                report(
                        DiagnosticId::unsupportedSyntax,
                        fieldDeclaration->span,
                        "struct field default could not be lowered");
                return std::nullopt;
            }
            fieldValues[index] = *value;
        }

        auto instruction = makeInstruction(ir::Opcode::constructStruct, expression->span);
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        instruction.symbol = structure->symbol;
        for (size_t index = 0; index < fieldValues.size(); ++index) {
            const auto value = acquireOwned(*fieldValues[index], expression->span);
            if (!value.has_value()) return std::nullopt;
            instruction.operands.push_back(value->id);
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    void lowerReturn(const syntax::ReturnExprSyntax::Ptr& expression) {
        if (expression->value == nullptr) {
            emitAllScopeCleanupForReturn(expression->span);
            emit(makeInstruction(ir::Opcode::ret, expression->span));
            return;
        }
        auto value = lowerReturnedValue(expression->value);
        if (!value.has_value()) return;
        value = coerce(*value, currentFunction().resultType, expression->value->span);
        if (!value.has_value()) return;
        emitReturnValue(*value, expression->span, expression->value->span);
    }

    // Returning an owning local of a noncopyable type moves it out (decision 0015): a copy
    // would leave the local to be destroyed, running the `deinit` of a value that escaped.
    std::optional<ir::Value> lowerReturnedValue(const syntax::ExprPtr& expression) {
        const auto type = typeOf(expression);
        if (!type.has_value() || copyableInBody(expression, *type) || !isAddressable(expression)) {
            return lowerExpression(expression);
        }
        const auto address = lowerAddress(expression);
        if (!address.has_value()) return std::nullopt;
        return emitValue(
                ir::Opcode::take,
                address->type,
                ir::ValueCategory::value,
                { address->id },
                expression->span);
    }

    void emitReturnValue(ir::Value value, SourceSpan span, SourceSpan valueSpan) {
        if (requiresDestroy(value.type)) {
            const auto owned = acquireOwned(value, valueSpan);
            if (!owned.has_value()) return;
            value = *owned;
        }
        emitAllScopeCleanupForReturn(span);
        if (currentFunction().resultType == model->types().voidType()) {
            emit(makeInstruction(ir::Opcode::ret, span));
            return;
        }
        auto instruction = makeInstruction(ir::Opcode::ret, span);
        instruction.operands = { value.id };
        emit(std::move(instruction));
    }

    ir::Value emitUnit(SourceSpan span) {
        return emitValue(
                ir::Opcode::literal,
                model->types().voidType(),
                ir::ValueCategory::value,
                {},
                span);
    }

    ir::Value emitValue(
            Operation operation,
            typing::TypeId type,
            ir::ValueCategory category,
            std::vector<ir::ValueId> operands,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt,
            bool implicitCode = false,
            std::optional<ir::DebugVariableId> debugVariable = std::nullopt) {
        const auto opcode = operation.opcode;
        auto instruction = makeInstruction(operation, span, implicitCode);
        instruction.result = makeValue(type, category);
        instruction.operands = std::move(operands);
        instruction.symbol = symbol;
        const auto result = *instruction.result;
        if (debugVariable.has_value()) {
            instruction.debugVariableBindings.push_back(ir::DebugVariableBinding {
                *debugVariable,
                result.id,
            });
        }
        emit(std::move(instruction));
        if (category == ir::ValueCategory::value) {
            auto valueOwnership = ValueOwnership::borrowed;
            if (opcode == ir::Opcode::copyValue || opcode == ir::Opcode::take) {
                valueOwnership = ValueOwnership::owned;
            }
            recordValue(result, valueOwnership);
        }
        return result;
    }

    void emitStore(
            ir::Value value,
            ir::Value address,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt) {
        auto converted = coerce(value, address.type, span);
        if (!converted.has_value()) return;
        if (requiresDestroy(address.type)) {
            converted = acquireOwned(*converted, span);
            if (!converted.has_value()) return;
        }
        emitRawStore(*converted, address, span, symbol);
    }

    std::optional<ir::Value> coerce(
            ir::Value value,
            typing::TypeId destination,
            SourceSpan span) {
        if (value.type == destination || destination == model->types().anyType()) return value;
        const auto* destinationType = model->types().type(destination);
        if (model->types().isOptional(destinationType) &&
            destinationType->arguments.size() == 1 &&
            destinationType->arguments[0] == value.type) {
            const auto some = findRoleCase(destination, ir::EnumerationRole::optional, false);
            if (some.has_value()) {
                return emitEnumConstruction(destination, *some, { value }, span);
            }
        }
        report(
                DiagnosticId::missingType,
                span,
                "typed conversion from '" + model->types().displayName(value.type) +
                        "' to '" + model->types().displayName(destination) +
                        "' has no IR representation");
        return std::nullopt;
    }

    ir::BlockId createBlock(std::string name) {
        auto& blocks = currentFunction().blocks;
        const auto id = static_cast<ir::BlockId>(blocks.size());
        blocks.push_back(ir::BasicBlock { id, std::move(name), {} });
        return id;
    }

    void switchToBlock(ir::BlockId block) {
        assert(block < currentFunction().blocks.size());
        currentBlockId = block;
    }

    void emitBranch(ir::BlockId target, SourceSpan span) {
        auto instruction = makeInstruction(ir::Opcode::branch, span);
        instruction.targets = { target };
        emit(std::move(instruction));
    }

    void emitConditionalBranch(
            ir::Value condition,
            ir::BlockId trueBlock,
            ir::BlockId falseBlock,
            SourceSpan span) {
        auto instruction = makeInstruction(ir::Opcode::conditionalBranch, span);
        instruction.operands = { condition.id };
        instruction.targets = { trueBlock, falseBlock };
        emit(std::move(instruction));
    }

    ir::Instruction makeInstruction(
            Operation operation,
            SourceSpan span,
            bool implicitCode = false) const {
        auto instruction = ir::Instruction { operation.opcode };
        instruction.modifier = operation.modifier;
        instruction.span = span;
        if (module->sourceFile(0) != nullptr) {
            instruction.debugLocation = ir::DebugLocation {
                span,
                implicitCode,
                currentDebugScope,
            };
        }
        return instruction;
    }

    ir::Value makeValue(typing::TypeId type, ir::ValueCategory category) {
        return ir::Value { nextValue++, type, category };
    }

    void emit(ir::Instruction instruction) {
        currentBlock().instructions.push_back(std::move(instruction));
    }

    bool currentBlockTerminated() const {
        const auto& instructions = currentBlock().instructions;
        return !instructions.empty() && ir::isTerminator(instructions.back().opcode);
    }

    ir::Function& currentFunction() {
        assert(currentFunctionId < module->functions.size());
        return module->functions[currentFunctionId];
    }

    const ir::Function& currentFunction() const {
        assert(currentFunctionId < module->functions.size());
        return module->functions[currentFunctionId];
    }

    ir::BasicBlock& currentBlock() {
        auto& blocks = currentFunction().blocks;
        assert(currentBlockId < blocks.size());
        return blocks[currentBlockId];
    }

    const ir::BasicBlock& currentBlock() const {
        const auto& blocks = currentFunction().blocks;
        assert(currentBlockId < blocks.size());
        return blocks[currentBlockId];
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::unitExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::propagateExpr:
            case syntax::Kind::binaryExpr:
            case syntax::Kind::assignmentExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
            case syntax::Kind::subscriptExpr:
            case syntax::Kind::arrayExpr:
            case syntax::Kind::dictionaryExpr:
            case syntax::Kind::contextualCaseExpr:
            case syntax::Kind::blockExpr:
            case syntax::Kind::ifExpr:
            case syntax::Kind::returnExpr:
            case syntax::Kind::matchExpr:
                return true;
            default:
                return false;
        }
    }
};

} // namespace

Result Lowerer::lower(
        const typing::TypeCheckedModel::Ptr& model,
        std::string sourceName,
        std::optional<ir::SourceInfo> sourceInfo,
        std::vector<ir::SourceInfo> sources) const {
    return Builder().build(
            model, std::move(sourceName), std::move(sourceInfo), std::move(sources));
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::unsupportedSyntax: return "ir-lowering.unsupported-syntax";
        case DiagnosticId::missingType: return "ir-lowering.missing-type";
        case DiagnosticId::missingSymbol: return "ir-lowering.missing-symbol";
        case DiagnosticId::missingReturn: return "ir-lowering.missing-return";
        case DiagnosticId::ownershipViolation: return "ir-lowering.ownership-violation";
        case DiagnosticId::verificationFailed: return "ir-lowering.verification-failed";
        case DiagnosticId::invalidConstant: return "ir-lowering.invalid-constant";
    }
    return "ir-lowering.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length
            << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::lowering
