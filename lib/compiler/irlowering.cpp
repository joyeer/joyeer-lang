#include "joyeer/compiler/irlowering.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace joyeer::lowering {

namespace {

class Builder {
public:
    Result build(
            const typing::TypeCheckedModel::Ptr& checkedModel,
            std::string sourceName,
            std::optional<ir::SourceInfo> sourceInfo) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        module = std::make_shared<ir::Module>();
        module->sourceName = std::move(sourceName);
        module->sourceInfo = std::move(sourceInfo);

        snapshotTypes();
        collectAggregateDefinitions();
        collectFunctions();
        lowerFunctions();

        if (diagnostics.empty()) {
            const auto verification = ir::Verifier().verify(*module);
            for (const auto& error : verification.errors) {
                report(
                        DiagnosticId::verificationFailed,
                        {},
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

    typing::TypeCheckedModel::Ptr model;
    std::shared_ptr<ir::Module> module;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<semantic::SymbolId, ir::FunctionId> functions;
    std::unordered_map<semantic::SymbolId, ir::Value> slots;
    std::unordered_map<ir::ValueId, ValueOwnership> ownership;
    std::unordered_set<ir::ValueId> liveOwnedTemporaries;
    std::vector<ScopeFrame> scopes;
    ir::FunctionId currentFunctionId = ir::invalidFunctionId;
    ir::BlockId currentBlockId = ir::invalidBlockId;
    ir::ValueId nextValue = 0;

    void report(DiagnosticId id, SourceSpan span, std::string message) {
        diagnostics.push_back(Diagnostic { id, span, std::move(message) });
    }

    bool requiresDestroy(typing::TypeId type) const {
        return ir::requiresDestruction(*module, type);
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

    std::optional<ir::Value> acquireOwned(ir::Value value, SourceSpan span) {
        if (!requiresDestroy(value.type)) return value;
        if (ownershipOf(value) == ValueOwnership::owned) {
            if (!liveOwnedTemporaries.erase(value.id)) {
                report(
                        DiagnosticId::ownershipViolation,
                        span,
                        "owned value is used after its ownership was transferred");
                return std::nullopt;
            }
            return value;
        }
        auto copy = emitValue(
                ir::Opcode::copyValue,
                value.type,
                ir::ValueCategory::value,
                { value.id },
                span);
        liveOwnedTemporaries.erase(copy.id);
        return copy;
    }

    void emitRawStore(
            ir::Value value,
            ir::Value address,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt,
            bool implicitCode = false) {
        auto instruction = makeInstruction(ir::Opcode::store, span, implicitCode);
        instruction.operands = { value.id, address.id };
        instruction.symbol = symbol;
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
            module->types.push_back(ir::TypeName {
                id,
                types.displayName(id),
                type->kind,
                type->symbol,
                type->arguments,
            });
        }
    }

    void collectAggregateDefinitions() {
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.root();
        if (root != nullptr) {
            for (const auto& item : root->items) {
                if (item->kind == syntax::Kind::structDecl) {
                    collectStructure(
                            std::static_pointer_cast<syntax::StructDeclSyntax>(item));
                } else if (item->kind == syntax::Kind::enumDecl) {
                    collectEnumeration(
                            std::static_pointer_cast<syntax::EnumDeclSyntax>(item));
                }
            }
        }
        for (typing::TypeId id = 0; id < model->types().size(); ++id) {
            const auto* type = model->types().type(id);
            if (type != nullptr &&
                (type->kind == typing::TypeKind::optional ||
                 type->kind == typing::TypeKind::result)) {
                collectBuiltinEnumeration(*type);
            }
        }
    }

    void collectStructure(const syntax::StructDeclSyntax::Ptr& declaration) {
        const auto& semanticModel = *model->semanticModel();
        const auto symbol = semanticModel.declaredSymbol(declaration);
        const auto type = symbol.has_value()
                ? model->typeOf(*symbol)
                : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) return;

        ir::StructureDefinition structure;
        structure.symbol = *symbol;
        structure.type = *type;
        structure.name = declaration->name == nullptr
                ? std::string()
                : declaration->name->rawValue;
        for (const auto& field : declaration->fields) {
            const auto fieldSymbol = semanticModel.declaredSymbol(field);
            const auto fieldType = fieldSymbol.has_value()
                    ? model->typeOf(*fieldSymbol)
                    : std::optional<typing::TypeId>();
            if (!fieldSymbol.has_value() || !fieldType.has_value()) continue;
            const auto* semanticField = semanticModel.symbol(*fieldSymbol);
            structure.fields.push_back(ir::FieldDefinition {
                *fieldSymbol,
                field->name == nullptr ? std::string() : field->name->rawValue,
                *fieldType,
                semanticField != nullptr && semanticField->isMutable,
            });
        }
        module->structures.push_back(std::move(structure));
    }

    void collectEnumeration(const syntax::EnumDeclSyntax::Ptr& declaration) {
        const auto& semanticModel = *model->semanticModel();
        const auto symbol = semanticModel.declaredSymbol(declaration);
        const auto type = symbol.has_value()
                ? model->typeOf(*symbol)
                : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) return;

        ir::EnumerationDefinition enumeration;
        enumeration.symbol = *symbol;
        enumeration.type = *type;
        enumeration.name = declaration->name == nullptr
                ? std::string()
                : declaration->name->rawValue;
        for (const auto& enumCase : declaration->cases) {
            const auto caseSymbol = semanticModel.declaredSymbol(enumCase);
            if (!caseSymbol.has_value()) continue;
            const auto* signature = model->callable(*caseSymbol);
            enumeration.cases.push_back(ir::EnumCaseDefinition {
                *caseSymbol,
                enumCase->name == nullptr ? std::string() : enumCase->name->rawValue,
                signature == nullptr
                        ? std::vector<typing::TypeId>()
                        : signature->parameters,
            });
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
            if (type.kind == typing::TypeKind::optional && enumCase->name == "Some") {
                payloadTypes.push_back(type.arguments[0]);
            } else if (type.kind == typing::TypeKind::result && enumCase->name == "Ok") {
                payloadTypes.push_back(type.arguments[0]);
            } else if (type.kind == typing::TypeKind::result && enumCase->name == "Err") {
                payloadTypes.push_back(type.arguments[1]);
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
        collectReadFile();
        collectByteConversions();
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.root();
        if (root == nullptr) return;
        for (const auto& item : root->items) {
            if (item->kind == syntax::Kind::functionDecl) {
                collectFunction(std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
            } else if (item->kind == syntax::Kind::bindingDecl) {
                report(
                        DiagnosticId::unsupportedSyntax,
                        item->span,
                        "top-level bindings are not supported by primitive IR lowering");
            }
        }
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

    void collectReadFile() {
        const auto& semanticModel = *model->semanticModel();
        const auto* prelude = semanticModel.scope(semanticModel.preludeScope());
        if (prelude == nullptr) return;
        const auto found = prelude->values.find("readFile");
        if (found == prelude->values.end()) return;
        const auto* signature = model->callable(found->second);
        if (signature == nullptr || signature->parameters.size() != 1) return;

        ir::Function function;
        function.id = static_cast<ir::FunctionId>(module->functions.size());
        function.symbol = found->second;
        function.name = "readFile";
        function.parameters.push_back(ir::Parameter {
            ir::Value { 0, signature->parameters[0], ir::ValueCategory::value },
            std::nullopt,
            "path",
            false,
            {},
        });
        function.resultType = signature->result;
        function.returnsValue = true;
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
        if (module->sourceInfo.has_value()) {
            function.debugLocation = ir::DebugLocation { declaration->span, false };
        }
        function.resultType = signature->result;
        function.returnsValue = signature->result != model->types().voidType() &&
                signature->result != model->types().neverType();
        function.entry = 0;

        for (size_t index = 0; index < declaration->parameters.size(); ++index) {
            const auto& parameter = declaration->parameters[index];
            const auto parameterEffect = parameter->accessEffect();
            const auto isAddressProjection =
                    parameterEffect == syntax::AccessEffect::inout ||
                    parameterEffect == syntax::AccessEffect::initializing;
            const auto parameterSymbol = semanticModel.declaredSymbol(parameter);
            const auto parameterType = parameterSymbol.has_value()
                    ? model->typeOf(*parameterSymbol)
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
            });
        }

        functions.emplace(*symbol, function.id);
        module->functions.push_back(std::move(function));
    }

    void lowerFunctions() {
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.root();
        if (root == nullptr) return;
        for (const auto& item : root->items) {
            if (item->kind != syntax::Kind::functionDecl) continue;
            const auto declaration = std::static_pointer_cast<syntax::FunctionDeclSyntax>(item);
            const auto symbol = semanticModel.declaredSymbol(declaration);
            if (!symbol.has_value()) continue;
            const auto found = functions.find(*symbol);
            if (found != functions.end()) lowerFunction(declaration, found->second);
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
        function.blocks.push_back(ir::BasicBlock { 0, "entry", {} });
        nextValue = static_cast<ir::ValueId>(function.parameters.size());
        const auto diagnosticStart = diagnostics.size();

        for (const auto& parameter : function.parameters) {
            recordValue(parameter.value, ValueOwnership::borrowed);
            if (parameter.isMutable) {
                if (parameter.symbol.has_value()) slots[*parameter.symbol] = parameter.value;
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
                emitRawStore(
                    parameter.value,
                    address,
                    parameter.span,
                    parameter.symbol,
                    true);
            if (parameter.symbol.has_value()) slots[*parameter.symbol] = address;
            if (parameter.isConsuming) registerOwnedStorage(address);
        }

        auto bodyValue = lowerBlock(declaration->body);
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            return;
        }
        if (function.returnsValue && bodyValue.has_value()) {
            bodyValue = coerce(*bodyValue, function.resultType, declaration->body->span);
            if (bodyValue.has_value() && requiresDestroy(bodyValue->type)) {
                bodyValue = acquireOwned(*bodyValue, declaration->body->span);
            }
            if (bodyValue.has_value()) {
                cleanupAndPopScope(declaration->body->span);
                auto instruction = makeInstruction(
                        ir::Opcode::returnValue,
                        declaration->body->span);
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
            emit(makeInstruction(ir::Opcode::unreachable, declaration->span));
        } else if (function.resultType == model->types().neverType()) {
            emit(makeInstruction(ir::Opcode::unreachable, declaration->span));
        } else {
            emit(makeInstruction(
                    ir::Opcode::returnVoid,
                    declaration->body == nullptr ? declaration->span : declaration->body->span));
        }
    }

    std::optional<ir::Value> lowerBlock(const syntax::BlockExprSyntax::Ptr& block) {
        if (block == nullptr) return std::nullopt;
        pushScope();
        std::optional<ir::Value> result;
        for (const auto& item : block->items) {
            if (currentBlockTerminated()) break;
            if (item->kind == syntax::Kind::bindingDecl) {
                lowerBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(item));
                result.reset();
            } else if (item->kind == syntax::Kind::whileStmt) {
                lowerWhile(std::static_pointer_cast<syntax::WhileStmtSyntax>(item));
                result.reset();
            } else if (isExpressionKind(item->kind)) {
                result = lowerExpression(std::static_pointer_cast<syntax::ExprSyntax>(item));
            }
        }
        if (currentBlockTerminated()) {
            discardScopeAfterTerminator();
            return std::nullopt;
        }
        if (result.has_value() && requiresDestroy(result->type)) {
            result = acquireOwned(*result, block->span);
            if (!result.has_value()) {
                cleanupAndPopScope(block->span);
                return std::nullopt;
            }
        }
        cleanupAndPopScope(block->span);
        if (result.has_value() && requiresDestroy(result->type)) {
            adoptInParentScope(*result);
        }
        return result;
    }

    void lowerBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        const auto type = symbol.has_value()
                ? model->typeOf(*symbol)
                : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) {
            report(
                    symbol.has_value() ? DiagnosticId::missingType : DiagnosticId::missingSymbol,
                    declaration->span,
                    "binding is missing a typed symbol");
            return;
        }
        const auto address = emitValue(
                ir::Opcode::stackAllocate,
                *type,
                ir::ValueCategory::address,
                {},
                declaration->span,
                symbol);
        slots[*symbol] = address;
        if (declaration->initializer == nullptr) {
            if (requiresDestroy(*type)) {
                auto initialize = makeInstruction(
                        ir::Opcode::zeroInitialize,
                        declaration->span);
                initialize.operands = { address.id };
                emit(std::move(initialize));
                registerOwnedStorage(address);
            }
            return;
        }
        auto initializer = lowerExpression(declaration->initializer);
        if (!initializer.has_value()) return;
        if (requiresDestroy(*type)) {
            initializer = acquireOwned(*initializer, declaration->initializer->span);
            if (!initializer.has_value()) return;
        }
        emitRawStore(*initializer, address, declaration->span, symbol);
        if (requiresDestroy(*type)) {
            registerOwnedStorage(address);
        }
    }

    std::optional<ir::Value> lowerExpression(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return std::nullopt;
        switch (expression->kind) {
            case syntax::Kind::literalExpr:
                return lowerLiteral(std::static_pointer_cast<syntax::LiteralExprSyntax>(expression));
            case syntax::Kind::nameExpr:
                return lowerName(expression);
            case syntax::Kind::parenthesizedExpr:
                return lowerExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::prefixExpr:
                return lowerPrefix(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression));
            case syntax::Kind::accessExpr:
                return lowerExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
            case syntax::Kind::binaryExpr:
                return lowerBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
            case syntax::Kind::assignmentExpr:
                lowerAssignment(std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression));
                return std::nullopt;
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
        const auto type = model->typeOf(expression);
        if (!type.has_value() || expression->literal == nullptr) {
            report(DiagnosticId::missingType, expression->span, "literal has no type");
            return std::nullopt;
        }

        ir::Opcode opcode;
        switch (expression->literal->kind) {
            case decimalLiteral: opcode = ir::Opcode::integerConstant; break;
            case booleanLiteral: opcode = ir::Opcode::booleanConstant; break;
            case stringLiteral: opcode = ir::Opcode::stringConstant; break;
            case byteLiteral: opcode = ir::Opcode::byteConstant; break;
            case nilLiteral: {
                const auto none = findEnumCase(*type, "None");
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
        } else if (expression->literal->kind != stringLiteral) {
            instruction.integerValue = expression->literal->intValue;
        } else {
            instruction.text = expression->literal->rawValue;
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

    std::optional<ir::Value> lowerName(const syntax::NodePtr& expression) {
        const auto symbol = model->referencedSymbol(expression);
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
        const auto operand = lowerExpression(expression->operand);
        const auto type = model->typeOf(expression);
        if (!operand.has_value() || !type.has_value()) return std::nullopt;
        if (expression->op == nullptr || expression->op->kind != minus) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "prefix operator is not supported by primitive IR lowering");
            return std::nullopt;
        }

        auto zero = makeInstruction(ir::Opcode::integerConstant, expression->op->span);
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
        const auto left = lowerExpression(expression->left);
        const auto right = lowerExpression(expression->right);
        const auto type = model->typeOf(expression);
        if (!left.has_value() || !right.has_value() || !type.has_value() ||
            expression->op == nullptr) {
            return std::nullopt;
        }
        const auto opcode = binaryOpcode(expression->op->kind);
        if (!opcode.has_value()) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "binary operator is not supported by primitive IR lowering");
            return std::nullopt;
        }
        return emitValue(
                *opcode,
                *type,
                ir::ValueCategory::value,
                { left->id, right->id },
                expression->span);
    }

    std::optional<ir::Opcode> binaryOpcode(TokenKind token) const {
        switch (token) {
            case plus: return ir::Opcode::add;
            case minus: return ir::Opcode::subtract;
            case multiply: return ir::Opcode::multiply;
            case less: return ir::Opcode::less;
            case lessEqual: return ir::Opcode::lessEqual;
            case greater: return ir::Opcode::greater;
            case greaterEqual: return ir::Opcode::greaterEqual;
            case equalEqual: return ir::Opcode::equal;
            case notEqual: return ir::Opcode::notEqual;
            case andAnd: return ir::Opcode::logicalAnd;
            default: return std::nullopt;
        }
    }

    void lowerAssignment(const syntax::AssignmentExprSyntax::Ptr& expression) {
        if (lowerDictionarySet(expression)) return;
        const auto address = lowerAddress(expression->target);
        auto value = lowerExpression(expression->value);
        if (address.has_value() && value.has_value()) {
            value = coerce(*value, address->type, expression->value->span);
            if (!value.has_value()) return;
            if (requiresDestroy(address->type)) {
                value = acquireOwned(*value, expression->value->span);
                if (!value.has_value()) return;
                emitDestroyStorage(*address, expression->target->span);
            }
            emitRawStore(*value, *address, expression->span);
        }
    }

    bool lowerDictionarySet(const syntax::AssignmentExprSyntax::Ptr& expression) {
        auto target = expression->target;
        while (target != nullptr &&
               (target->kind == syntax::Kind::accessExpr ||
                target->kind == syntax::Kind::parenthesizedExpr)) {
            target = target->kind == syntax::Kind::accessExpr
                    ? std::static_pointer_cast<syntax::AccessExprSyntax>(target)->operand
                    : std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(target)->expression;
        }
        if (target == nullptr || target->kind != syntax::Kind::subscriptExpr) return false;

        const auto subscript =
                std::static_pointer_cast<syntax::SubscriptExprSyntax>(target);
        const auto baseTypeId = model->typeOf(subscript->base);
        const auto* dictionaryType = baseTypeId.has_value()
                ? model->types().type(*baseTypeId)
                : nullptr;
        if (dictionaryType == nullptr ||
            dictionaryType->kind != typing::TypeKind::dictionary ||
            dictionaryType->arguments.size() != 2) {
            return false;
        }

        const auto dictionary = lowerAddress(subscript->base);
        auto key = lowerExpression(subscript->index);
        auto value = lowerExpression(expression->value);
        if (!dictionary.has_value() || !key.has_value() || !value.has_value()) {
            return true;
        }
        key = coerce(*key, dictionaryType->arguments[0], subscript->index->span);
        value = coerce(*value, dictionaryType->arguments[1], expression->value->span);
        if (!key.has_value() || !value.has_value()) return true;
        if (requiresDestroy(key->type)) {
            key = acquireOwned(*key, subscript->index->span);
        }
        if (requiresDestroy(value->type)) {
            value = acquireOwned(*value, expression->value->span);
        }
        if (!key.has_value() || !value.has_value()) return true;

        auto instruction = makeInstruction(ir::Opcode::dictionarySet, expression->span);
        instruction.operands = { dictionary->id, key->id, value->id };
        emit(std::move(instruction));
        return true;
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
                const auto type = model->typeOf(member);
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
            const auto base = lowerAddress(subscript->base);
            const auto index = lowerExpression(subscript->index);
            const auto type = model->typeOf(subscript);
            if (!base.has_value() || !index.has_value() || !type.has_value()) {
                return std::nullopt;
            }
            return emitValue(
                    ir::Opcode::subscriptAddress,
                    *type,
                    ir::ValueCategory::address,
                    { base->id, index->id },
                    subscript->span);
        }
        report(
                DiagnosticId::unsupportedSyntax,
                expression->span,
                "assignment target is not supported by primitive IR lowering");
        return std::nullopt;
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
        const auto type = model->typeOf(expression);
        if (semanticSymbol == nullptr || !type.has_value()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "member has no typed resolved symbol");
            return std::nullopt;
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
            return emitValue(
                    ir::Opcode::count,
                    *type,
                    ir::ValueCategory::value,
                    { base->id },
                    expression->span,
                    symbol);
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

    std::optional<ir::Value> lowerSubscript(
            const syntax::SubscriptExprSyntax::Ptr& expression) {
        const auto base = lowerExpression(expression->base);
        const auto index = lowerExpression(expression->index);
        const auto type = model->typeOf(expression);
        if (!base.has_value() || !index.has_value() || !type.has_value()) {
            return std::nullopt;
        }
        return emitValue(
                ir::Opcode::subscript,
                *type,
                ir::ValueCategory::value,
                { base->id, index->id },
                expression->span);
    }

    std::optional<ir::Value> lowerArray(const syntax::ArrayExprSyntax::Ptr& expression) {
        const auto type = model->typeOf(expression);
        if (!type.has_value()) return std::nullopt;
        const auto* arrayType = model->types().type(*type);
        if (arrayType == nullptr || arrayType->kind != typing::TypeKind::array ||
            arrayType->arguments.size() != 1) {
            report(DiagnosticId::missingType, expression->span, "array literal has no array type");
            return std::nullopt;
        }

        auto instruction = makeInstruction(ir::Opcode::constructArray, expression->span);
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        for (const auto& element : expression->elements) {
            const auto value = lowerExpression(element);
            if (!value.has_value()) return std::nullopt;
            auto converted = coerce(*value, arrayType->arguments[0], element->span);
            if (!converted.has_value()) return std::nullopt;
            if (requiresDestroy(arrayType->arguments[0])) {
                converted = acquireOwned(*converted, element->span);
                if (!converted.has_value()) return std::nullopt;
            }
            instruction.operands.push_back(converted->id);
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    std::optional<ir::Value> lowerDictionary(
            const syntax::DictionaryExprSyntax::Ptr& expression) {
        const auto type = model->typeOf(expression);
        if (!type.has_value()) return std::nullopt;
        const auto* dictionaryType = model->types().type(*type);
        if (dictionaryType == nullptr ||
            dictionaryType->kind != typing::TypeKind::dictionary ||
            dictionaryType->arguments.size() != 2) {
            report(
                    DiagnosticId::missingType,
                    expression->span,
                    "dictionary literal has no dictionary type");
            return std::nullopt;
        }

        auto instruction = makeInstruction(
                ir::Opcode::constructDictionary,
                expression->span);
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        for (const auto& entry : expression->entries) {
            const auto key = lowerExpression(entry->key);
            const auto value = lowerExpression(entry->value);
            if (!key.has_value() || !value.has_value()) return std::nullopt;
                auto convertedKey = coerce(
                    *key,
                    dictionaryType->arguments[0],
                    entry->key->span);
            auto convertedValue = coerce(
                    *value,
                    dictionaryType->arguments[1],
                    entry->value->span);
            if (!convertedKey.has_value() || !convertedValue.has_value()) {
                return std::nullopt;
            }
            if (requiresDestroy(dictionaryType->arguments[0])) {
                convertedKey = acquireOwned(*convertedKey, entry->key->span);
            }
            if (requiresDestroy(dictionaryType->arguments[1])) {
                convertedValue = acquireOwned(*convertedValue, entry->value->span);
            }
            if (!convertedKey.has_value() || !convertedValue.has_value()) {
                return std::nullopt;
            }
            instruction.operands.push_back(convertedKey->id);
            instruction.operands.push_back(convertedValue->id);
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    std::optional<ir::Value> lowerContextualCase(
            const syntax::ContextualCaseExprSyntax::Ptr& expression) {
        const auto caseSymbol = model->referencedSymbol(expression);
        const auto type = model->typeOf(expression);
        if (!caseSymbol.has_value() || !type.has_value()) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "contextual enum case has no typed target");
            return std::nullopt;
        }
        std::vector<ir::Value> payloads;
        for (const auto& argument : expression->arguments) {
            const auto payload = lowerExpression(argument->value);
            if (!payload.has_value()) return std::nullopt;
            payloads.push_back(*payload);
        }
        return emitEnumConstruction(*type, *caseSymbol, payloads, expression->span);
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

    std::optional<ir::Value> emitEnumConstruction(
            typing::TypeId type,
            semantic::SymbolId caseSymbol,
            const std::vector<ir::Value>& payloads,
            SourceSpan span) {
        const auto enumeration = std::find_if(
                module->enumerations.begin(),
                module->enumerations.end(),
                [type](const auto& candidate) { return candidate.type == type; });
        const auto enumCase = enumeration == module->enumerations.end()
                ? static_cast<const ir::EnumCaseDefinition*>(nullptr)
                : [&]() -> const ir::EnumCaseDefinition* {
                    const auto found = std::find_if(
                            enumeration->cases.begin(),
                            enumeration->cases.end(),
                            [caseSymbol](const auto& candidate) {
                                return candidate.symbol == caseSymbol;
                            });
                    return found == enumeration->cases.end() ? nullptr : &*found;
                }();
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

    std::optional<ir::Value> lowerMatch(const syntax::MatchExprSyntax::Ptr& expression) {
        const auto scrutinee = lowerExpression(expression->scrutinee);
        const auto resultType = model->typeOf(expression);
        if (!scrutinee.has_value() || !resultType.has_value()) return std::nullopt;

        const auto producesResult = *resultType != model->types().voidType() &&
                *resultType != model->types().neverType();
        const auto resultAddress = producesResult
                ? std::optional<ir::Value>(emitValue(
                        ir::Opcode::stackAllocate,
                        *resultType,
                        ir::ValueCategory::address,
                        {},
                        expression->span))
                : std::optional<ir::Value>();

        std::vector<ir::BlockId> armBlocks;
        for (size_t index = 0; index < expression->arms.size(); ++index) {
            armBlocks.push_back(createBlock("match.arm." + std::to_string(index)));
        }
        const auto mergeBlock = createBlock("match.merge");

        auto dispatch = makeInstruction(ir::Opcode::switchPattern, expression->span);
        dispatch.operands = { scrutinee->id };
        for (size_t index = 0; index < expression->arms.size(); ++index) {
            const auto pattern = lowerPattern(expression->arms[index]->pattern);
            if (!pattern.has_value()) return std::nullopt;
            dispatch.switchCases.push_back(ir::SwitchCase { *pattern, armBlocks[index] });
        }
        emit(std::move(dispatch));

        bool hasMergePredecessor = false;
        for (size_t index = 0; index < expression->arms.size(); ++index) {
            const auto& arm = expression->arms[index];
            switchToBlock(armBlocks[index]);
            pushScope();
            bindPattern(arm->pattern, *scrutinee);
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
        if (!resultAddress.has_value()) return std::nullopt;
        registerOwnedStorage(*resultAddress);
        return emitValue(
                ir::Opcode::load,
                resultAddress->type,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }

    std::optional<ir::Pattern> lowerPattern(const syntax::PatternPtr& pattern) {
        if (pattern == nullptr) return std::nullopt;
        const auto type = model->typeOf(pattern);
        if (!type.has_value()) {
            report(DiagnosticId::missingType, pattern->span, "pattern has no type");
            return std::nullopt;
        }

        ir::Pattern result;
        result.type = *type;
        switch (pattern->kind) {
            case syntax::Kind::wildcardPattern:
            case syntax::Kind::bindingPattern:
                result.kind = ir::PatternKind::wildcard;
                break;
            case syntax::Kind::literalPattern: {
                const auto literal = std::static_pointer_cast<syntax::LiteralPatternSyntax>(pattern);
                if (literal->literal == nullptr) return std::nullopt;
                switch (literal->literal->kind) {
                    case decimalLiteral:
                        result.kind = ir::PatternKind::integerLiteral;
                        result.integerValue = literal->literal->intValue;
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
                        const auto none = findEnumCase(*type, "None");
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

    void bindPattern(const syntax::PatternPtr& pattern, ir::Value value) {
        if (pattern == nullptr) return;
        if (pattern->kind == syntax::Kind::bindingPattern) {
            const auto symbol = model->semanticModel()->declaredSymbol(pattern);
            if (!symbol.has_value()) return;
            const auto address = emitValue(
                    ir::Opcode::stackAllocate,
                    value.type,
                    ir::ValueCategory::address,
                    {},
                    pattern->span,
                    symbol);
                    emitRawStore(value, address, pattern->span, symbol);
            slots[*symbol] = address;
            return;
        }
        if (pattern->kind != syntax::Kind::enumCasePattern) return;

        const auto enumPattern = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern);
        const auto caseSymbol = model->referencedSymbol(enumPattern);
        if (!caseSymbol.has_value()) return;
        for (size_t index = 0; index < enumPattern->arguments.size(); ++index) {
            const auto& payloadPattern = enumPattern->arguments[index]->pattern;
            const auto payloadType = model->typeOf(payloadPattern);
            if (!payloadType.has_value()) continue;
            auto instruction = makeInstruction(
                    ir::Opcode::extractPayload,
                    payloadPattern->span);
            instruction.result = makeValue(*payloadType, ir::ValueCategory::value);
            instruction.operands = { value.id };
            instruction.symbol = *caseSymbol;
            instruction.integerValue = static_cast<int64_t>(index);
            const auto payload = *instruction.result;
            emit(std::move(instruction));
            bindPattern(payloadPattern, payload);
        }
    }

    std::optional<ir::Value> lowerIf(const syntax::IfExprSyntax::Ptr& expression) {
        const auto condition = lowerExpression(expression->condition);
        const auto resultType = model->typeOf(expression);
        if (!condition.has_value() || !resultType.has_value()) return std::nullopt;

        const auto producesResult = *resultType != model->types().voidType() &&
                *resultType != model->types().neverType();
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
        if (!resultAddress.has_value()) return std::nullopt;
        registerOwnedStorage(*resultAddress);
        return emitValue(
                ir::Opcode::load,
                resultAddress->type,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }

    void lowerWhile(const syntax::WhileStmtSyntax::Ptr& statement) {
        const auto headerBlock = createBlock("while.header");
        const auto bodyBlock = createBlock("while.body");
        const auto exitBlock = createBlock("while.exit");
        emitBranch(headerBlock, statement->span);

        switchToBlock(headerBlock);
        pushScope();
        const auto condition = lowerExpression(statement->condition);
        if (condition.has_value()) {
            cleanupAndPopScope(statement->condition->span);
            emitConditionalBranch(
                    *condition,
                    bodyBlock,
                    exitBlock,
                    statement->condition->span);
        } else {
            cleanupAndPopScope(statement->condition->span);
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
    }

    std::optional<ir::Value> lowerCall(const syntax::CallExprSyntax::Ptr& expression) {
        const auto target = model->callTarget(expression);
        if (!target.has_value()) {
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
            const auto type = model->typeOf(expression);
            if (!type.has_value()) return std::nullopt;
            std::vector<ir::Value> payloads;
            for (const auto& argument : expression->arguments) {
                const auto payload = lowerExpression(argument->value);
                if (!payload.has_value()) return std::nullopt;
                payloads.push_back(*payload);
            }
            return emitEnumConstruction(*type, *target, payloads, expression->span);
        }
        if (targetSymbol != nullptr &&
            targetSymbol->kind == semantic::SymbolKind::builtinMember &&
            targetSymbol->name == "append") {
            return lowerArrayAppend(expression);
        }
        if (!functions.contains(*target)) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "call target has no IR function declaration");
            return std::nullopt;
        }

        std::vector<ir::ValueId> arguments;
        const auto& callee = module->functions[functions.at(*target)];
        for (size_t index = 0; index < expression->arguments.size(); ++index) {
            const auto& argument = expression->arguments[index];
            const auto expectsAddress = index < callee.parameters.size() &&
                callee.parameters[index].value.category == ir::ValueCategory::address;
            const auto consumesValue = index < callee.parameters.size() &&
                    callee.parameters[index].isConsuming;
            std::optional<ir::Value> value;
            if (expectsAddress) {
                value = lowerAddress(argument->value);
            } else if (consumesValue && isAddressable(argument->value)) {
                const auto address = lowerAddress(argument->value);
                if (address.has_value()) {
                    value = emitValue(
                            ir::Opcode::take,
                            address->type,
                            ir::ValueCategory::value,
                            { address->id },
                            argument->span);
                }
            } else {
                value = lowerExpression(argument->value);
            }
            if (!value.has_value()) return std::nullopt;
            if (!expectsAddress && index < callee.parameters.size() &&
                !callee.parameters[index].acceptsAnyType) {
                value = coerce(
                        *value,
                        callee.parameters[index].value.type,
                        argument->value->span);
                if (!value.has_value()) return std::nullopt;
            }
            if (consumesValue && requiresDestroy(value->type)) {
                value = acquireOwned(*value, argument->span);
                if (!value.has_value()) return std::nullopt;
            }
            arguments.push_back(value->id);
        }

        auto instruction = makeInstruction(ir::Opcode::call, expression->span);
        instruction.operands = std::move(arguments);
        instruction.callee = functions.at(*target);
        const auto type = model->typeOf(expression);
        if (type.has_value() && *type != model->types().voidType() &&
            *type != model->types().neverType()) {
            instruction.result = makeValue(*type, ir::ValueCategory::value);
        }
        const auto result = instruction.result;
        emit(std::move(instruction));
        if (result.has_value()) {
            recordValue(
                *result,
                requiresDestroy(result->type)
                    ? ValueOwnership::owned
                    : ValueOwnership::trivial);
        }
        return result;
    }

    std::optional<ir::Value> lowerArrayAppend(
            const syntax::CallExprSyntax::Ptr& expression) {
        if (expression->callee->kind != syntax::Kind::memberExpr ||
            expression->arguments.size() != 1) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "Array.append requires one element argument");
            return std::nullopt;
        }
        const auto member =
                std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee);
        const auto array = lowerAddress(member->base);
        const auto* arrayType = array.has_value()
                ? model->types().type(array->type)
                : nullptr;
        if (!array.has_value() || arrayType == nullptr ||
            arrayType->kind != typing::TypeKind::array ||
            arrayType->arguments.size() != 1) {
            report(
                    DiagnosticId::missingType,
                    expression->span,
                    "Array.append receiver has no concrete element type");
            return std::nullopt;
        }

        auto element = lowerExpression(expression->arguments[0]->value);
        if (!element.has_value()) return std::nullopt;
        element = coerce(
                *element,
                arrayType->arguments[0],
                expression->arguments[0]->value->span);
        if (!element.has_value()) return std::nullopt;
        if (requiresDestroy(element->type)) {
            element = acquireOwned(*element, expression->arguments[0]->value->span);
            if (!element.has_value()) return std::nullopt;
        }

        auto instruction = makeInstruction(ir::Opcode::arrayAppend, expression->span);
        instruction.operands = { array->id, element->id };
        emit(std::move(instruction));
        return std::nullopt;
    }

    std::optional<ir::Value> lowerStructConstruction(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId initializer) {
        const auto* initializerSymbol = model->semanticModel()->symbol(initializer);
        const auto type = model->typeOf(expression);
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
                [initializerSymbol](const auto& candidate) {
                    return candidate.symbol == *initializerSymbol->containingSymbol;
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
            const auto value = lowerExpression(argument->value);
            if (!value.has_value()) return std::nullopt;
            fieldValues[static_cast<size_t>(
                    std::distance(structure->fields.begin(), field))] = *value;
        }

        for (size_t index = 0; index < fieldValues.size(); ++index) {
            if (fieldValues[index].has_value()) continue;
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
            const auto value = lowerExpression(fieldDeclaration->initializer);
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
            auto converted = coerce(
                *fieldValues[index],
                structure->fields[index].type,
                expression->span);
            if (!converted.has_value()) return std::nullopt;
            if (requiresDestroy(structure->fields[index].type)) {
                converted = acquireOwned(*converted, expression->span);
                if (!converted.has_value()) return std::nullopt;
            }
            instruction.operands.push_back(converted->id);
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        recordValue(result, ValueOwnership::owned);
        return result;
    }

    void lowerReturn(const syntax::ReturnExprSyntax::Ptr& expression) {
        if (expression->value == nullptr) {
            emitAllScopeCleanupForReturn(expression->span);
            emit(makeInstruction(ir::Opcode::returnVoid, expression->span));
            return;
        }
        auto value = lowerExpression(expression->value);
        if (!value.has_value()) return;
        value = coerce(*value, currentFunction().resultType, expression->value->span);
        if (!value.has_value()) return;
        if (requiresDestroy(value->type)) {
            value = acquireOwned(*value, expression->value->span);
            if (!value.has_value()) return;
        }
        emitAllScopeCleanupForReturn(expression->span);
        auto instruction = makeInstruction(ir::Opcode::returnValue, expression->span);
        instruction.operands = { value->id };
        emit(std::move(instruction));
    }

    ir::Value emitValue(
            ir::Opcode opcode,
            typing::TypeId type,
            ir::ValueCategory category,
            std::vector<ir::ValueId> operands,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt,
            bool implicitCode = false) {
        auto instruction = makeInstruction(opcode, span, implicitCode);
        instruction.result = makeValue(type, category);
        instruction.operands = std::move(operands);
        instruction.symbol = symbol;
        const auto result = *instruction.result;
        emit(std::move(instruction));
        if (category == ir::ValueCategory::value) {
            auto valueOwnership = ValueOwnership::borrowed;
            if (opcode == ir::Opcode::copyValue || opcode == ir::Opcode::take ||
                (opcode == ir::Opcode::add && requiresDestroy(type))) {
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
        if (destinationType != nullptr &&
            destinationType->kind == typing::TypeKind::optional &&
            destinationType->arguments.size() == 1 &&
            destinationType->arguments[0] == value.type) {
            const auto some = findEnumCase(destination, "Some");
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
            ir::Opcode opcode,
            SourceSpan span,
            bool implicitCode = false) const {
        auto instruction = ir::Instruction { opcode };
        instruction.span = span;
        if (module->sourceInfo.has_value()) {
            instruction.debugLocation = ir::DebugLocation { span, implicitCode };
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
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
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
        std::optional<ir::SourceInfo> sourceInfo) const {
    return Builder().build(model, std::move(sourceName), std::move(sourceInfo));
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::unsupportedSyntax: return "ir-lowering.unsupported-syntax";
        case DiagnosticId::missingType: return "ir-lowering.missing-type";
        case DiagnosticId::missingSymbol: return "ir-lowering.missing-symbol";
        case DiagnosticId::missingReturn: return "ir-lowering.missing-return";
        case DiagnosticId::ownershipViolation: return "ir-lowering.ownership-violation";
        case DiagnosticId::verificationFailed: return "ir-lowering.verification-failed";
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
