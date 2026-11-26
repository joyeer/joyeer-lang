#include "builder.h"
#include "compiler/frontend/intrinsics.h"

#include <algorithm>
#include <utility>

namespace joyeer::typing {

// A core unit (spec 19.4.6) is part of the runtime, so it may use only the
// types that never lower to a runtime call: Bool, the integer and floating
// types, Void and Never.
bool TypeCheckingBuilder::allowedInCoreUnit(TypeId type) const {
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return true;
    switch (record->kind) {
        case TypeKind::error:
        case TypeKind::voidType:
        case TypeKind::never:
        case TypeKind::boolean:
            return true;
        default:
            return isNumeric(record->kind);
    }
}

bool TypeCheckingBuilder::declaresLiteralAddress(const syntax::FunctionDeclSyntax& declaration) {
    for (const auto& attribute : declaration.attributes) {
        if (attribute.name == nullptr || attribute.name->rawValue != "c_extern" ||
            attribute.arguments.empty() || attribute.arguments.front()->value == nullptr ||
            attribute.arguments.front()->value->kind != syntax::Kind::literalExpr) {
            continue;
        }
        const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(
                attribute.arguments.front()->value);
        if (literal->literal != nullptr && intrinsic::readsLiteral(literal->literal->rawValue)) {
            return true;
        }
    }
    return false;
}

void TypeCheckingBuilder::requireCoreType(const syntax::NodePtr& node, TypeId type) {
    if (node == nullptr || coreCheckSuspended || node.get() == literalAddressArgument ||
        !model->semanticModelValue->isCoreSource(node->span) || allowedInCoreUnit(type)) {
        return;
    }
    report(TypeCheckingDiagnosticId::managedTypeInCoreUnit, node->span,
           "a runtime core unit may use only scalar types, but this has type '" +
                   model->typeContext.displayName(type) +
                   "', whose operations call into the runtime");
}

void TypeCheckingBuilder::resolveNativeBinding(
        const syntax::FunctionDeclSyntax::Ptr& declaration,
        semantic::SymbolId function) {
    if (declaration->attributes.empty()) return;
    const auto diagnosticStart = diagnostics.size();
    std::optional<NativeBinding> binding;
    for (const auto& attribute : declaration->attributes) {
        const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
        if (name == "compiler_role" || name == "compiler_transfer") {
            if (!model->semanticModelValue->isPreludeSource(attribute.span) ||
                (name == "compiler_transfer" && !attribute.arguments.empty()) ||
                (name == "compiler_role" && attribute.arguments.size() != 1)) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                       "compiler prelude metadata is reserved for compiler-bundled declarations");
            }
            continue;
        }
        if (name != "c_extern" && name != "c_export" && name != "c_data") {
            report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                   "unknown annotation '@" + name + "'");
            continue;
        }
        if (binding.has_value()) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                   "a function may have only one C binding annotation");
            continue;
        }
        binding = NativeBinding {
            name == "c_export" ? NativeBindingKind::cExport : NativeBindingKind::cImport,
            {},
            attribute.span,
        };
        binding->dataSymbol = name == "c_data";
        if (attribute.arguments.empty()) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                   "C annotations require a 'symbol:' string-literal argument");
            continue;
        }
        const auto& argument = attribute.arguments.front();
        const auto literal = argument->value != nullptr &&
                argument->value->kind == syntax::Kind::literalExpr
                ? std::static_pointer_cast<syntax::LiteralExprSyntax>(argument->value)
                : nullptr;
        if (argument->label == nullptr || argument->label->rawValue != "symbol" ||
            argument->accessMarker != nullptr || literal == nullptr ||
            literal->literal == nullptr || literal->literal->kind != stringLiteral) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, argument->span,
                   "C annotations require 'symbol:' followed by a string literal");
            continue;
        }
        binding->symbol = literal->literal->rawValue;
        if (!isCIdentifier(binding->symbol)) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, argument->span,
                   "native symbol must be a nonempty ASCII C identifier without linker decoration");
        }
        // Further arguments name `[UInt8]` parameters passed as a bare
        // data pointer: `pointer: "label"`, once per parameter.
        for (size_t position = 1; position < attribute.arguments.size(); ++position) {
            const auto& extra = attribute.arguments[position];
            const auto name = extra->value != nullptr && extra->value->kind == syntax::Kind::literalExpr
                    ? std::static_pointer_cast<syntax::LiteralExprSyntax>(extra->value)
                    : nullptr;
            if (extra->label == nullptr || extra->label->rawValue != "pointer" ||
                extra->accessMarker != nullptr || name == nullptr || name->literal == nullptr ||
                name->literal->kind != stringLiteral) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, extra->span,
                       "after 'symbol:', C annotations accept only 'pointer:' followed by a "
                       "parameter label string literal");
                continue;
            }
            const auto& parameters = declaration->parameters;
            const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
                return parameter->label != nullptr && parameter->label->rawValue == name->literal->rawValue;
            });
            if (found == parameters.end()) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, extra->span,
                       "'pointer:' names no parameter '" + name->literal->rawValue + "'");
                continue;
            }
            const auto index = static_cast<uint32_t>(found - parameters.begin());
            if (binding->passesPointer(index)) {
                report(TypeCheckingDiagnosticId::invalidAnnotation, extra->span,
                       "parameter '" + name->literal->rawValue + "' is already passed as a pointer");
                continue;
            }
            binding->pointerParameters.push_back(index);
        }
    }
    if (!binding.has_value()) return;
    if (binding->kind == NativeBindingKind::cExport && !binding->pointerParameters.empty()) {
        report(TypeCheckingDiagnosticId::invalidAnnotation, binding->span,
               "'pointer:' applies only to C imports");
    }
    if (!model->semanticModelValue->isRuntimeSource(declaration->span)) {
        report(TypeCheckingDiagnosticId::unauthorizedNativeBinding, binding->span,
               "C bindings require an explicitly selected runtime compilation unit");
    }
    const bool importing = binding->kind == NativeBindingKind::cImport;
    const bool hasBody = declaration->body != nullptr;
    if ((importing && hasBody) || (!importing && !hasBody)) {
        report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
               importing ? "@c_extern must not have a function body"
                         : "@c_export requires a function body");
    }
    if (declaration->returnType == nullptr) {
        report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
               "C bindings require an explicit result type, including Void");
    }
    if (binding->dataSymbol) {
        // @c_data names a data symbol, not a function: no parameters, and the
        // address it yields is an Int. The name must not be a startup symbol.
        const auto& shape = model->callables.at(function);
        const auto* result = model->typeContext.type(shape.result);
        if (!declaration->parameters.empty() || result == nullptr || result->kind != TypeKind::integer ||
            !binding->pointerParameters.empty()) {
            report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                   "@c_data declares 'func name(): Int', the address of a C data symbol");
        }
        if (isCEntrySymbol(binding->symbol)) {
            report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                   "@c_data must not name a toolchain executable entry point");
        }
    }
    const auto* symbol = model->semanticModelValue->symbol(function);
    // Only a core unit (spec 19.4.6) may export the platform's program entry
    // point; every other startup symbol, and every other unit, may not.
    const bool coreEntry = !importing && isCoreEntrySymbol(binding->symbol) &&
            model->semanticModelValue->isCoreSource(declaration->span);
    if ((symbol->isRootModule && symbol->name == "main") ||
        (!importing && isCEntrySymbol(binding->symbol) && !coreEntry)) {
        report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
               "C bindings must not replace a toolchain executable entry point");
    }
    const auto& signature = model->callables.at(function);
    for (size_t index = 0; index < declaration->parameters.size(); ++index) {
        const auto& parameter = declaration->parameters[index];
        const auto effect = parameter->accessEffect();
        const auto buffer = nativeBufferKind(signature.parameters[index],
                [&](TypeId id) { return model->typeContext.type(id); },
                [&](TypeId id) { return model->isArrayRoleType(id); });
        if (binding->passesPointer(static_cast<uint32_t>(index))) {
            if (!importing || buffer != NativeBufferKind::bytes ||
                (effect != syntax::AccessEffect::borrowing && effect != syntax::AccessEffect::inout)) {
                report(TypeCheckingDiagnosticId::unsupportedNativeSignature, parameter->span,
                       "'pointer:' applies only to borrowing or inout [UInt8] parameters of C imports");
            }
            continue;
        }
        const bool scalar = effect == syntax::AccessEffect::borrowing &&
                isNativeScalar(model->typeContext.type(signature.parameters[index])->kind);
        const bool nativeBuffer = importing && buffer != NativeBufferKind::none &&
                (effect == syntax::AccessEffect::borrowing ||
                 (effect == syntax::AccessEffect::initializing &&
                  buffer != NativeBufferKind::byteArrays));
        if (!scalar && !nativeBuffer) {
            report(TypeCheckingDiagnosticId::unsupportedNativeSignature, parameter->span,
                   "C bindings require borrowing scalars; C imports also accept borrowing "
                   "String, [UInt8], [String], [[UInt8]] and initializing String, [UInt8], [String]");
        }
    }
    // Never marks a C function that does not return (an import such as exit),
    // or an export that does not (the runtime's panic).
    if (!isNativeScalar(model->typeContext.type(signature.result)->kind) &&
        signature.result != model->typeContext.voidType() &&
        model->typeContext.type(signature.result)->kind != TypeKind::never) {
        report(TypeCheckingDiagnosticId::unsupportedNativeSignature,
               declaration->returnType == nullptr ? declaration->span : declaration->returnType->span,
               "C bindings require a scalar or Void result");
    }
    if (intrinsic::reserved(binding->symbol)) {
        // A reserved symbol names a compiler intrinsic (§19.4.5): it must be
        // a known one, imported, with exactly its signature.
        std::vector<TypeKind> kinds;
        std::vector<bool> pointers;
        std::vector<bool> mutables;
        for (size_t index = 0; index < signature.parameters.size(); ++index) {
            kinds.push_back(model->typeContext.type(signature.parameters[index])->kind);
            pointers.push_back(binding->passesPointer(static_cast<uint32_t>(index)));
            const auto effect = declaration->parameters[index]->accessEffect();
            mutables.push_back(effect == syntax::AccessEffect::inout || effect == syntax::AccessEffect::initializing);
        }
        const auto* descriptor = intrinsic::find(binding->symbol);
        if (!importing || descriptor == nullptr ||
            !intrinsic::matches(*descriptor, model->typeContext.type(signature.result)->kind,
                                kinds, pointers, mutables)) {
            report(TypeCheckingDiagnosticId::invalidNativeDeclaration, declaration->span,
                   "'" + binding->symbol + "' is not a compiler intrinsic with this signature");
        }
    }
    if (diagnostics.size() == diagnosticStart) {
        model->nativeBindings.emplace(function, std::move(*binding));
    }
}

void TypeCheckingBuilder::validateNativeSymbols() {
    std::unordered_map<std::string, semantic::SymbolId> declarations;
    std::unordered_set<std::string> exports;
    std::unordered_set<std::string> conflicting;
    for (const auto& symbol : model->semanticModelValue->symbols()) {
        const auto* binding = model->nativeBinding(symbol.id);
        if (binding == nullptr) continue;
        const auto [previous, inserted] = declarations.emplace(binding->symbol, symbol.id);
        if (!inserted && model->nativeBinding(previous->second)->dataSymbol != binding->dataSymbol) {
            if (conflicting.insert(binding->symbol).second) {
                report(TypeCheckingDiagnosticId::conflictingNativeSymbol, binding->span,
                       "native symbol '" + binding->symbol + "' is declared as both a function and a data symbol");
            }
        } else if (!inserted) {
            const auto& left = model->callables.at(previous->second);
            const auto& right = model->callables.at(symbol.id);
            const auto compatible = [&](TypeId a, TypeId b) {
                const auto lookup = [&](TypeId id) { return model->typeContext.type(id); };
                const auto isArray = [&](TypeId id) { return model->isArrayRoleType(id); };
                const auto leftBuffer = nativeBufferKind(a, lookup, isArray);
                const auto rightBuffer = nativeBufferKind(b, lookup, isArray);
                if (leftBuffer != NativeBufferKind::none || rightBuffer != NativeBufferKind::none) {
                    return leftBuffer == rightBuffer;
                }
                return sameNativeRepresentation(model->typeContext.type(a)->kind,
                        model->typeContext.type(b)->kind);
            };
            const auto* previousSymbol = model->semanticModelValue->symbol(previous->second);
            bool sameEffects = previousSymbol->callable->parameters.size() == symbol.callable->parameters.size();
            for (size_t index = 0; sameEffects && index < symbol.callable->parameters.size(); ++index) {
                sameEffects = previousSymbol->callable->parameters[index].access ==
                        symbol.callable->parameters[index].access;
            }
            // Repeated declarations must also agree on which buffers carry no count.
            auto left_pointers = model->nativeBinding(previous->second)->pointerParameters;
            auto right_pointers = binding->pointerParameters;
            std::sort(left_pointers.begin(), left_pointers.end());
            std::sort(right_pointers.begin(), right_pointers.end());
            sameEffects = sameEffects && left_pointers == right_pointers;
            if (left.parameters.size() != right.parameters.size() ||
                    !std::equal(left.parameters.begin(), left.parameters.end(),
                            right.parameters.begin(), right.parameters.end(), compatible) ||
                    !compatible(left.result, right.result) || !sameEffects) {
                if (conflicting.insert(binding->symbol).second) {
                    report(TypeCheckingDiagnosticId::conflictingNativeSymbol, binding->span,
                           "incompatible C signatures for native symbol '" + binding->symbol + "'");
                }
            }
        }
        if (binding->kind == NativeBindingKind::cExport &&
            !exports.insert(binding->symbol).second) {
            report(TypeCheckingDiagnosticId::conflictingNativeSymbol, binding->span,
                   "duplicate C export for native symbol '" + binding->symbol + "'");
        }
    }
}

TypeId TypeCheckingBuilder::checkCallbackAddress(const syntax::CallExprSyntax::Ptr& expression) {
    const auto error = model->typeContext.errorType();
    if (expression->arguments.size() != 1) return error;
    checkArgumentAccess(syntax::AccessEffect::borrowing, *expression->arguments.front());
    auto entry = expression->arguments[0]->value;
    while (entry != nullptr && entry->kind == syntax::Kind::parenthesizedExpr) {
        entry = std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(entry)->expression;
    }
    if (entry != nullptr) markDeferredReferenceHandled(entry);
    const auto named = entry != nullptr &&
            (entry->kind == syntax::Kind::nameExpr || entry->kind == syntax::Kind::memberExpr)
            ? model->referencedSymbol(entry) : std::optional<semantic::SymbolId>();
    const auto* target = named.has_value() ? model->semanticModelValue->symbol(*named) : nullptr;
    const auto* parameters = named.has_value() ? model->genericParameters(*named) : nullptr;
    const auto* signature = named.has_value() ? model->callable(*named) : nullptr;
    const auto declaration = target != nullptr && target->declaration.has_value()
            ? model->semanticModelValue->node(*target->declaration) : syntax::NodePtr();
    const auto function = declaration != nullptr && declaration->kind == syntax::Kind::functionDecl
            ? std::static_pointer_cast<syntax::FunctionDeclSyntax>(declaration) : nullptr;
    if (target == nullptr || target->kind != semantic::SymbolKind::function ||
        function == nullptr || function->returnType == nullptr ||
        model->nativeBinding(target->id) != nullptr || parameters == nullptr ||
        signature == nullptr || signature->parameters != std::vector<TypeId> { model->typeContext.intType() } ||
        signature->result != model->typeContext.voidType() || !target->callable.has_value() ||
        target->callable->parameters.size() != 1 ||
        target->callable->parameters[0].access != syntax::AccessEffect::consuming) {
        report(TypeCheckingDiagnosticId::invalidTypeArgument,
               entry == nullptr ? expression->span : entry->span,
               "a callback address requires a named generic Joyeer function with exactly one "
               "'consuming Int' parameter and a 'Void' result",
               "write func entry<T>(address: consuming Int): Void { ... }");
        return error;
    }
    if (!model->semanticModelValue->isRuntimeSource(target->span)) {
        report(TypeCheckingDiagnosticId::unauthorizedIntrinsic, entry->span,
               "a runtime callback entry must be declared in an authorized runtime unit");
        return error;
    }
    if (expression->typeArguments.size() != parameters->size()) {
        report(TypeCheckingDiagnosticId::invalidTypeArgument, expression->span,
               "a callback address must supply every type argument of '" + target->name + "'");
        return error;
    }
    std::vector<TypeId> arguments;
    for (const auto& argument : expression->typeArguments) {
        arguments.push_back(resolveType(argument).value_or(error));
    }
    checkTypeArguments(expression, *target, *parameters, arguments);
    model->genericCalls[*model->semanticModelValue->nodeId(expression)] = GenericCall {
        target->id, arguments, currentFunction, expression->span };
    model->nodeTypes[*model->semanticModelValue->nodeId(entry)] = model->typeContext.functionType(
            { model->typeContext.intType() }, { syntax::AccessEffect::consuming }, model->typeContext.voidType());
    return model->typeContext.intType();
}

// `joyeer_intrinsic_size_of<T>()` and the other four layout intrinsics (spec 20.12.5). They exist
// only in a toolchain-authorized unit; the type argument is recorded for lowering, which
// substitutes it per instance.
TypeId TypeCheckingBuilder::checkLayoutIntrinsic(const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol& target) {
    const auto error = model->typeContext.errorType();
    const auto& name = target.name;
    if (!model->semanticModelValue->isRuntimeSource(expression->span) &&
        !model->semanticModelValue->isPreludeSource(expression->span)) {
        report(TypeCheckingDiagnosticId::unauthorizedIntrinsic, expression->span,
               "'" + name + "' is available only in a toolchain-authorized runtime unit",
               "write a checked Joyeer wrapper in a unit selected with --runtime-module");
        return error;
    }
    if (name == "joyeer_intrinsic_callback_address") return checkCallbackAddress(expression);
    if (expression->typeArguments.size() != 1) {
        report(TypeCheckingDiagnosticId::invalidTypeArgument, expression->span,
               "'" + name + "' takes exactly one explicit type argument",
               "write " + name + "<T>(...)");
        return error;
    }
    const auto written = resolveType(expression->typeArguments[0]);
    const auto type = written.value_or(error);
    std::optional<TypeId> result;
    if (type != error) {
        const bool transfer = model->semanticModelValue->isPreludeSource(expression->span) &&
                name != "joyeer_intrinsic_destroy_at" && name != "joyeer_intrinsic_needs_destroy";
        if (type == model->typeContext.neverType() || (model->isLinear(type) && !transfer)) {
            report(TypeCheckingDiagnosticId::invalidTypeArgument, expression->typeArguments[0]->span,
                   "'" + name + "' cannot take " + (type == model->typeContext.neverType() ? "'Never'" :
                           "the linear type '" + model->typeContext.displayName(type) + "'"),
                   "a layout intrinsic needs a type with a representation that is not linear (spec 20.12.5)");
        } else if (name == "joyeer_intrinsic_clone_at" && !model->typeContext.mentionsTypeParameter(type) &&
                   !model->isCopyable(type)) {
            report(TypeCheckingDiagnosticId::invalidTypeArgument, expression->typeArguments[0]->span,
                   "'" + name + "' needs a copyable type, but '" + model->typeContext.displayName(type) +
                           "' is not",
                   "copy only what an implicit copy of the type could produce");
        } else {
            model->layoutIntrinsicTypes[*model->semanticModelValue->nodeId(expression)] = type;
        }
    }
    const auto expectedAddress = model->typeContext.intType();
    const auto checkAddress = [&](size_t index) {
        const auto& argument = expression->arguments[index];
        const auto actual = checkExpression(argument->value, expectedAddress);
        if (actual.has_value()) requireAssignable(*actual, expectedAddress, argument->value->span);
        checkCallArgumentAccess(target, index, *argument);
    };
    if (name == "joyeer_intrinsic_needs_destroy") return model->typeContext.boolType();
    if (name == "joyeer_intrinsic_key_kind") {
        // Only the four admitted key types have a key kind; a generic one is checked per instance in lowering.
        const auto* written = model->typeContext.type(type);
        const bool parameter = written != nullptr && written->kind == TypeKind::typeParameter;
        if (type != error && type != model->typeContext.neverType() && !parameter && !admittedKeyKind(type)) {
            report(TypeCheckingDiagnosticId::invalidTypeArgument, expression->typeArguments[0]->span,
                   "'" + name + "' needs a dictionary key type, but '" + model->typeContext.displayName(type) +
                           "' is not one",
                   "keys are restricted to Int, Bool, UInt8 and String (spec 20.5)");
        }
        return model->typeContext.intType();
    }
    if (name == "joyeer_intrinsic_size_of" || name == "joyeer_intrinsic_align_of" ||
        name == "joyeer_intrinsic_stack_slot" || name == "joyeer_intrinsic_clone_callback" ||
        name == "joyeer_intrinsic_destroy_callback") {
        return model->typeContext.intType();
    }
    if (expression->arguments.empty()) return error;
    if (name == "joyeer_intrinsic_borrow_address") {
        // `borrow_address<T>(value: place)`: the read-only address of a named readable place (a local, any
        // parameter, or a field path of those), valid for the call it is a direct argument of; an rvalue has no
        // place. Read-only is the library author's duty, as `place_address`'s direct-argument rule is checked
        // here and not in the verifier (decision 0027).
        if (expression->arguments.size() != 1) return error;
        if (!permittedPlaceAddress.contains(expression.get())) {
            report(TypeCheckingDiagnosticId::invalidCallArguments, expression->span,
                   "'" + name + "' may only be a direct argument of a call",
                   "write f(address: " + name + "<T>(value: place)), not a binding or a return value");
        }
        const auto& placeArgument = expression->arguments[0];
        const auto actual = checkExpression(placeArgument->value,
                type == error ? std::nullopt : std::optional<TypeId>(type));
        if (actual.has_value() && type != error) requireAssignable(*actual, type, placeArgument->value->span);
        requireExactPlaceType(actual, type, name, placeArgument->value->span);
        if (!isNamedLocalPlace(placeArgument->value)) {
            report(TypeCheckingDiagnosticId::invalidCallArguments, placeArgument->value->span,
                   "'" + name + "' needs a named place: a local, a parameter or a field path of those",
                   "bind the value to a local first");
        }
        checkCallArgumentAccess(target, 0, *placeArgument);
        return model->typeContext.intType();
    }
    if (name == "joyeer_intrinsic_place_address") {
        // `place_address<T>(&place)`: the address of an `inout` place, as an `Int`, for passing the place to a
        // runtime function. It must be a direct argument of a call, so that call holds the `inout` access for
        // its duration; a stored or returned address would outlive the access (decision 0026).
        if (expression->arguments.size() != 1) return error;
        if (!permittedPlaceAddress.contains(expression.get())) {
            report(TypeCheckingDiagnosticId::invalidCallArguments, expression->span,
                   "'" + name + "' may only be a direct argument of a call",
                   "write f(address: " + name + "<T>(value: &place)), not a binding or a return value");
        }
        const auto& placeArgument = expression->arguments[0];
        const auto actual = checkExpression(placeArgument->value,
                type == error ? std::nullopt : std::optional<TypeId>(type));
        if (actual.has_value() && type != error) requireAssignable(*actual, type, placeArgument->value->span);
        requireExactPlaceType(actual, type, name, placeArgument->value->span);
        checkCallArgumentAccess(target, 0, *placeArgument);
        return model->typeContext.intType();
    }
    checkAddress(0);
    if (name == "joyeer_intrinsic_stack_block") {
        // `stack_block<T>(count: N)`: contiguous stack storage for `N` slots, valid until the invocation
        // returns and never destroyed. The count is a literal so the storage is sized at compile time and is
        // allocated once per invocation, not on a loop's back edge (decision 0026).
        if (expression->arguments.size() != 1) return error;
        const auto& countArgument = expression->arguments[0];
        const auto literal = countArgument->value != nullptr && countArgument->value->kind == syntax::Kind::literalExpr
                ? std::static_pointer_cast<syntax::LiteralExprSyntax>(countArgument->value) : nullptr;
        if (literal == nullptr || literal->literal == nullptr || literal->literal->kind != decimalLiteral) {
            report(TypeCheckingDiagnosticId::invalidCallArguments, countArgument->value->span,
                   "the count of a stack block must be a non-negative integer literal",
                   "write " + name + "<T>(count: 8)");
            return error;
        }
        return model->typeContext.intType();
    }
    if (name == "joyeer_intrinsic_clone_at" || name == "joyeer_intrinsic_destroy_range" ||
        name == "joyeer_intrinsic_slot_address") {
        if (expression->arguments.size() != 2) return error;
        checkAddress(1);
        return name == "joyeer_intrinsic_slot_address" ? model->typeContext.intType()
                                                       : model->typeContext.voidType();
    }
    if (name == "joyeer_intrinsic_move_in") {
        if (expression->arguments.size() != 2) return error;
        const auto& argument = expression->arguments[1];
        const auto actual = checkExpression(argument->value, type == error ? std::nullopt : std::optional<TypeId>(type));
        if (actual.has_value() && type != error) requireAssignable(*actual, type, argument->value->span);
        checkCallArgumentAccess(target, 1, *argument);
        return model->typeContext.voidType();
    }
    return name == "joyeer_intrinsic_move_out" ? type : model->typeContext.voidType();
}

} // namespace joyeer::typing
