// Functions: one IR function declaration per function, method, `deinit`, prelude external and
// generic instance; then each struct is linked to its `deinit` (and a library collection to its
// copy method and element addressor); then every body is lowered. The contracts prelude's
// functions and the String role functions are declared only when lowering first calls or names
// one, and lowered last. The derived `Equatable` witnesses are declared and lowered from here too,
// by equality.cpp.

#include "internal.h"

#include <algorithm>

namespace joyeer::lowering::detail {

void Builder::collectFunctions() {
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
    collectDerivedEqualities();
    linkDeinitializers(*root);
    evaluateConstants();
    // Lowering can collect more declarations without invalidating references to the functions.
    module->functions.reserve(module->functions.size() + deferredPreludeFunctions.size());
}

// The collections prelude's String role functions (decision 0028 stage 3) are collected when a String operation
// first calls one and lowered after every other function, so a module without String operations, such as a
// runtime core unit, has none of them. Only lowering calls them: prelude code must not call one by name.
bool Builder::isStringRoleFunction(semantic::SymbolId symbol) const {
    const auto* semanticSymbol = model->semanticModel()->symbol(symbol);
    return semanticSymbol != nullptr && model->stringRoleFunction(semanticSymbol->libraryRole) == symbol;
}

std::optional<ir::FunctionId> Builder::stringRoleFunction(semantic::LibraryRole role) {
    const auto symbol = model->stringRoleFunction(role);
    if (!symbol.has_value()) return std::nullopt;
    return demandFunction(*symbol);
}

std::optional<ir::FunctionId> Builder::demandFunction(semantic::SymbolId symbol) {
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
void Builder::collectInstances(const syntax::SourceFileSyntax& root) {
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

syntax::FunctionDeclSyntax::Ptr Builder::genericDeclaration(
        const syntax::SourceFileSyntax& root, semantic::SymbolId function) const {
    const auto* symbol = model->semanticModel()->symbol(function);
    if (symbol == nullptr || !symbol->declaration.has_value()) return nullptr;
    const auto node = model->semanticModel()->node(*symbol->declaration);
    if (node == nullptr || node->kind != syntax::Kind::functionDecl) return nullptr;
    (void) root;
    return std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
}

void Builder::enterInstance(size_t index) {
    currentInstanceIndex = index;
    currentInstance = &model->instances()[index];
    currentNoncopyableSpans = nullptr;
}

void Builder::leaveInstance() {
    currentInstanceIndex.reset();
    currentInstance = nullptr;
    currentNoncopyableSpans = nullptr;
}

// A `deinit` is collected as a method (see forEachMethod); the structure it belongs to
// records which function that is, so destruction can call it.
void Builder::linkDeinitializers(const syntax::SourceFileSyntax& root) {
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

void Builder::collectPrint() {
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

void Builder::collectByteConversions() {
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

void Builder::collectFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
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

void Builder::lowerFunctions() {
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
    // A derived witness may call a String role, so these come before the deferred prelude bodies are lowered.
    for (const auto type : model->derivedEqualities()) {
        const auto found = derivedEqualityFunctions.find(type);
        if (found != derivedEqualityFunctions.end()) lowerDerivedEquality(type, found->second);
    }
    for (size_t index = 0; index < pendingPreludeBodies.size(); ++index) {
        const auto [declaration, id] = pendingPreludeBodies[index];
        lowerFunction(declaration, id);
    }
}

void Builder::lowerFunction(
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

} // namespace joyeer::lowering::detail
