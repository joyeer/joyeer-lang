#include "builder.h"

#include <algorithm>
#include <utility>

namespace joyeer::typing {

std::optional<TypeId> TypeCheckingBuilder::resolveType(const syntax::TypePtr& syntaxType) {
    if (syntaxType == nullptr) return std::nullopt;

    auto result = model->typeContext.errorType();
    switch (syntaxType->kind) {
        case syntax::Kind::errorType:
            break;
        case syntax::Kind::unitType:
            result = model->typeContext.voidType();
            break;
        case syntax::Kind::nominalType: {
            const auto nominal =
                    std::static_pointer_cast<syntax::NominalTypeSyntax>(syntaxType);
            std::vector<TypeId> arguments;
            arguments.reserve(nominal->arguments.size());
            for (const auto& argument : nominal->arguments) {
                arguments.push_back(resolveType(argument).value_or(
                        model->typeContext.errorType()));
            }

            const auto symbol = model->semanticModelValue->referencedSymbol(syntaxType);
            if (!symbol.has_value()) break;
            if (nominal->name != nullptr && nominal->name->rawValue == "Self" && arguments.empty() &&
                selfOwner.has_value() && *selfOwner == *symbol) {
                // `Self` in a generic type is the type applied to the parameters in scope.
                const auto selfType = model->typeContext.typeForSymbol(*symbol, selfArguments);
                if (selfType.has_value()) {
                    result = *selfType;
                    break;
                }
            }
            if (const auto* declaration = model->semanticModelValue->symbol(*symbol)) {
                if (declaration->kind == semantic::SymbolKind::protocol) {
                    if (nominal->name != nullptr && nominal->name->rawValue == "Self" && arguments.empty()) {
                        result = model->typeContext.typeParameterType(*symbol);
                        break;
                    }
                    report(TypeCheckingDiagnosticId::protocolUsedAsType, syntaxType->span,
                           "'" + declaration->name + "' is a protocol, not a type (spec 20.2)",
                           "declare a type parameter constrained to it, as in 'func f<T: " +
                                   declaration->name + ">(value: T)'");
                    break;
                }
                if (declaration->kind == semantic::SymbolKind::typeParameter && !arguments.empty()) {
                    report(TypeCheckingDiagnosticId::invalidGenericDeclaration, syntaxType->span,
                           "type parameter '" + declaration->name + "' takes no type arguments");
                    break;
                }
            }
            const auto resolved = model->typeContext.typeForSymbol(*symbol, arguments);
            if (resolved.has_value()) {
                result = *resolved;
                // `Array<Never>` is the same rejected type as `[Never]` and reads the same.
                if (arguments.size() == 1 && arguments[0] == model->typeContext.neverType()) {
                    if (const auto* arrayDeclaration = model->semanticModelValue->symbol(*symbol);
                        arrayDeclaration != nullptr &&
                        arrayDeclaration->libraryRole == semantic::LibraryRole::arrayType) {
                        result = rejectNeverElement(arguments[0], syntaxType->span);
                        break;
                    }
                }
                // `Dict<Never, V>` and `Dict<K, Never>` read the same as `[Never: V]` and `[K: Never]`.
                if (arguments.size() == 2 && nominal->arguments.size() == 2 &&
                    (arguments[0] == model->typeContext.neverType() ||
                     arguments[1] == model->typeContext.neverType())) {
                    if (const auto* dictionaryDeclaration = model->semanticModelValue->symbol(*symbol);
                        dictionaryDeclaration != nullptr &&
                        dictionaryDeclaration->libraryRole == semantic::LibraryRole::dictType) {
                        const auto key = rejectNeverDictionaryPart(arguments[0], nominal->arguments[0]->span,
                                                                   "key");
                        const auto value = rejectNeverDictionaryPart(arguments[1], nominal->arguments[1]->span,
                                                                     "value");
                        if (!genericLibraryKey(key, syntaxType->span)) {
                            rejectInvalidDictionaryKey(key, nominal->arguments[0]->span);
                        }
                        result = model->typeContext.dictionaryType(key, value);
                        break;
                    }
                }
                if (!arguments.empty()) {
                    if (const auto* generic = model->semanticModelValue->symbol(*symbol);
                        generic != nullptr && !generic->typeParameters.empty() &&
                        (generic->kind == semantic::SymbolKind::structure ||
                         generic->kind == semantic::SymbolKind::enumeration)) {
                        pendingTypeArguments.push_back({ syntaxType->span, *symbol, arguments });
                    }
                }
                const auto* record = model->typeContext.type(result);
                if (model->typeContext.isDictionary(record) && !nominal->arguments.empty() &&
                    !genericLibraryKey(record->arguments[0], syntaxType->span)) {
                    rejectInvalidDictionaryKey(record->arguments[0], nominal->arguments[0]->span);
                }
                break;
            }

            const auto arity = model->typeContext.typeArity(*symbol);
            const auto* declaration = model->semanticModelValue->symbol(*symbol);
            if (arity.has_value() && declaration != nullptr) {
                report(
                        TypeCheckingDiagnosticId::invalidTypeArgumentCount,
                        syntaxType->span,
                        "type '" + declaration->name + "' expects " +
                                std::to_string(*arity) + " type argument(s), but got " +
                                std::to_string(arguments.size()));
            }
            break;
        }
        case syntax::Kind::arrayType: {
            const auto array = std::static_pointer_cast<syntax::ArrayTypeSyntax>(syntaxType);
            result = model->typeContext.arrayType(rejectNeverElement(
                    resolveType(array->element).value_or(model->typeContext.errorType()), syntaxType->span));
            break;
        }
        case syntax::Kind::dictionaryType: {
            const auto dictionary =
                    std::static_pointer_cast<syntax::DictionaryTypeSyntax>(syntaxType);
            const auto key = rejectNeverDictionaryPart(resolveType(dictionary->key).value_or(
                    model->typeContext.errorType()), dictionary->key->span, "key");
            const auto value = rejectNeverDictionaryPart(resolveType(dictionary->value).value_or(
                    model->typeContext.errorType()), dictionary->value->span, "value");
            if (!genericLibraryKey(key, syntaxType->span)) rejectInvalidDictionaryKey(key, dictionary->key->span);
            result = model->typeContext.dictionaryType(key, value);
            break;
        }
        case syntax::Kind::optionalType: {
            const auto optional =
                    std::static_pointer_cast<syntax::OptionalTypeSyntax>(syntaxType);
            result = model->typeContext.optionalType(resolveType(optional->wrapped).value_or(
                    model->typeContext.errorType()));
            break;
        }
        case syntax::Kind::functionType: {
            const auto function = std::static_pointer_cast<syntax::FunctionTypeSyntax>(syntaxType);
            std::vector<TypeId> parameters;
            std::vector<syntax::AccessEffect> effects;
            for (const auto& parameter : function->parameters) {
                auto type = resolveType(parameter.type).value_or(model->typeContext.errorType());
                if (type == model->typeContext.neverType()) {
                    report(TypeCheckingDiagnosticId::invalidFunctionType, parameter.type->span,
                           "'Never' cannot be a function type's parameter type",
                           "a function taking a value that cannot exist could never be called");
                    type = model->typeContext.errorType();
                }
                parameters.push_back(type);
                effects.push_back(parameter.accessEffect());
            }
            const auto functionResult = resolveType(function->result).value_or(model->typeContext.errorType());
            result = model->typeContext.functionType(parameters, effects, functionResult);
            break;
        }
        default:
            break;
    }

    recordNodeType(syntaxType, result);
    return result;
}

void TypeCheckingBuilder::resolveTopLevelDeclaration(const syntax::NodePtr& node) {
    if (node == nullptr) return;
    switch (node->kind) {
        case syntax::Kind::bindingDecl:
            resolveBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(node));
            break;
        case syntax::Kind::functionDecl: {
            const auto declaration = std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
            resolveGenericParameters(declaration, std::nullopt);
            resolveFunction(declaration);
            break;
        }
        case syntax::Kind::protocolDecl:
            resolveProtocol(std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node));
            break;
        case syntax::Kind::structDecl: {
            const auto declaration = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
            resolveStructure(declaration);
            for (const auto& method : declaration->methods) resolveMethod(method);
            if (declaration->deinitializer != nullptr) {
                resolveFunction(declaration->deinitializer);
                // The `deinit` of a generic struct is an instance per instance of the struct.
                if (selfOwner.has_value() && !selfParameters.empty()) {
                    if (const auto deinit = model->semanticModelValue->declaredSymbol(declaration->deinitializer)) {
                        methodParameters[*deinit] = selfParameters;
                        model->genericParameterLists[*deinit] = selfParameters;
                    }
                }
            }
            selfOwner.reset();
            selfArguments.clear();
            break;
        }
        case syntax::Kind::enumDecl: {
            const auto declaration = std::static_pointer_cast<syntax::EnumDeclSyntax>(node);
            resolveEnumeration(declaration);
            for (const auto& method : declaration->methods) resolveMethod(method);
            selfOwner.reset();
            selfArguments.clear();
            break;
        }
        case syntax::Kind::extensionDecl: {
            const auto declaration = std::static_pointer_cast<syntax::ExtensionDeclSyntax>(node);
            enterExtension(declaration);
            if (!declaration->conformances.empty()) (void) resolveType(declaration->extendedType);
            for (const auto& method : declaration->methods) resolveMethod(method);
            selfOwner.reset();
            selfArguments.clear();
            break;
        }
        default:
            break;
    }
}

const semantic::Symbol* TypeCheckingBuilder::symbolOf(semantic::SymbolId id) const {
    return model->semanticModelValue->symbol(id);
}

std::string TypeCheckingBuilder::symbolName(semantic::SymbolId id) const {
    const auto* symbol = symbolOf(id);
    return symbol == nullptr ? std::string("<unknown>") : symbol->name;
}

bool TypeCheckingBuilder::checkProtocolReference(const syntax::TypePtr& reference) {
    if (reference->kind == syntax::Kind::nominalType &&
        std::static_pointer_cast<syntax::NominalTypeSyntax>(reference)->arguments.empty()) return true;
    report(TypeCheckingDiagnosticId::invalidConformance, reference->span,
           "a protocol reference must name a protocol without type arguments (spec 20.2)");
    return false;
}

// Records `T: P & Q` for each parameter: its constraint protocols, and its type. Returns the
// parameter symbols in order.
std::vector<semantic::SymbolId> TypeCheckingBuilder::recordTypeParameters(
        const std::vector<syntax::GenericParameterSyntax::Ptr>& syntaxParameters) {
    std::vector<semantic::SymbolId> parameters;
    for (const auto& parameter : syntaxParameters) {
        const auto symbol = model->semanticModelValue->declaredSymbol(parameter);
        if (!symbol.has_value()) continue;
        parameters.push_back(*symbol);
        const auto* declared = model->semanticModelValue->symbol(*symbol);
        if (declared != nullptr && declared->mustTransferParameter && !parameter->constraints.empty()) {
            report(TypeCheckingDiagnosticId::invalidGenericDeclaration, parameter->span,
                   "a compiler-prelude transfer parameter cannot declare constraints");
        }
        if (parameter->name != nullptr && parameter->name->rawValue == "Copyable") {
            report(TypeCheckingDiagnosticId::reservedProtocolName, parameter->name->span,
                   "'Copyable' is reserved for the builtin protocol (spec 20.5)");
        }
        rejectEquatableName(parameter->name);
        std::vector<semantic::SymbolId> protocols;
        for (const auto& constraint : parameter->constraints) {
            if (!checkProtocolReference(constraint)) continue;
            const auto referenced = model->semanticModelValue->referencedSymbol(constraint);
            if (!referenced.has_value()) continue;
            const auto* protocol = symbolOf(*referenced);
            if (protocol == nullptr || protocol->kind != semantic::SymbolKind::protocol) {
                report(TypeCheckingDiagnosticId::invalidConformance, constraint->span,
                       "'" + symbolName(*referenced) + "' is not a protocol; a constraint lists protocols");
                continue;
            }
            if (std::find(protocols.begin(), protocols.end(), *referenced) != protocols.end()) {
                report(TypeCheckingDiagnosticId::invalidConformance, constraint->span,
                       "the constraint of '" + symbolName(*symbol) + "' names '" +
                               protocol->name + "' twice");
                continue;
            }
            protocols.push_back(*referenced);
        }
        model->constraintLists[*symbol] = std::move(protocols);
        (void) model->typeContext.typeParameterType(*symbol);
    }
    constrainedParameters.insert(constrainedParameters.end(), parameters.begin(), parameters.end());
    return parameters;
}

// Records `T: P & Q`: the parameters of a top-level generic function and each parameter's
// constraint. A method, `init` or requirement with type parameters is reported by the caller's
// `misplaced` diagnostic, but its parameters still get constraints so their types resolve.
void TypeCheckingBuilder::resolveGenericParameters(
        const syntax::FunctionDeclSyntax::Ptr& declaration,
        std::optional<TypeCheckingDiagnosticId> misplaced) {
    if (declaration->genericParameters.empty()) return;
    const auto function = model->semanticModelValue->declaredSymbol(declaration);
    if (!function.has_value()) return;
    const auto clause = coveringSpan(declaration->genericParameters.front()->span,
                                     declaration->genericParameters.back()->span);
    if (misplaced.has_value()) {
        report(*misplaced, clause,
               *misplaced == TypeCheckingDiagnosticId::invalidProtocolMember
                       ? "a protocol requirement cannot declare type parameters"
                       : "only a top-level function can declare type parameters (spec 20.4)",
               "generic methods, structs and enums are not supported yet (spec 20.10)");
    }
    for (const auto& attribute : declaration->attributes) {
        const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
        if (name == "c_extern" || name == "c_export" || name == "c_data") {
            report(TypeCheckingDiagnosticId::genericNativeBinding, clause,
                   "a C binding cannot declare type parameters (spec 20.7)",
                   "write a non-generic binding and call it from generic code");
            break;
        }
    }
    auto parameters = recordTypeParameters(declaration->genericParameters);
    const auto* functionSymbol = symbolOf(*function);
    if (!misplaced.has_value() && functionSymbol != nullptr && functionSymbol->isRootModule &&
        functionSymbol->name == "main" && functionSymbol->kind == semantic::SymbolKind::function) {
        report(TypeCheckingDiagnosticId::invalidGenericDeclaration, clause,
               "the program entry point 'main' cannot declare type parameters",
               "move the work into a generic function that 'main' calls");
    }
    if (!misplaced.has_value()) model->genericParameterLists[*function] = std::move(parameters);
}

DiagnosticSourceNote TypeCheckingBuilder::sourceNote(SourceSpan span, std::string message) {
    return DiagnosticSourceNote { span.offset, span.length, std::move(message) };
}

SourceSpan TypeCheckingBuilder::coveringSpan(SourceSpan first, SourceSpan last) {
    if (first.sourceId != last.sourceId || last.offset < first.offset) return first;
    first.length = last.offset + last.length - first.offset;
    return first;
}

// `Equatable` names the prelude protocol in every constraint (spec 20.2, 20.13): no user type or type parameter
// may take the name. The role-bound protocol declaration is the one exemption (`resolveProtocol`).
void TypeCheckingBuilder::rejectEquatableName(const Token::Ptr& name) {
    if (name == nullptr || name->rawValue != "Equatable") return;
    report(TypeCheckingDiagnosticId::reservedProtocolName, name->span,
           "'Equatable' is reserved for the protocol of '==' and '!=' (spec 20.13)",
           "choose another name; 'Equatable' in a constraint always means the prelude protocol");
}

void TypeCheckingBuilder::resolveBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
    const auto type = resolveType(declaration->annotation);
    if (type.has_value() && containsFunctionType(*type)) {
        report(TypeCheckingDiagnosticId::invalidFunctionType, declaration->annotation->span,
               "a top-level constant cannot have the function type '" + model->typeContext.displayName(*type) + "'",
               "name the function directly, or bind the value to a local 'let'");
    }
    if (type.has_value()) recordDeclaredType(declaration, *type);
}

void TypeCheckingBuilder::resolveMethod(const syntax::FunctionDeclSyntax::Ptr& declaration) {
    if (selfOwner.has_value()) {
        if (const auto method = model->semanticModelValue->declaredSymbol(declaration)) {
            methodParameters[*method] = selfParameters;
        }
    }
    resolveGenericParameters(declaration, TypeCheckingDiagnosticId::invalidGenericDeclaration);
    resolveFunction(declaration);
    // A method of a generic type is instantiated per receiver type arguments (spec 20.12), like a
    // generic function: its parameters are those of the type or extension that declares it.
    if (selfOwner.has_value() && !selfParameters.empty()) {
        if (const auto method = model->semanticModelValue->declaredSymbol(declaration)) {
            model->genericParameterLists[*method] = selfParameters;
        }
    }
}

void TypeCheckingBuilder::resolveFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
    const auto function = model->semanticModelValue->declaredSymbol(declaration);
    if (!function.has_value()) return;

    TypedCallableSignature signature {
        semantic::CallableKind::function,
        true,
        {},
        model->typeContext.voidType(),
    };
    signature.parameters.reserve(declaration->parameters.size());
    const bool literalAddress = declaresLiteralAddress(*declaration);
    for (const auto& parameter : declaration->parameters) {
        // The literal-address intrinsic's own String parameter is not a
        // value a core unit builds; every other declaration is checked.
        coreCheckSuspended = literalAddress;
        auto type = resolveType(parameter->type).value_or(
                model->typeContext.errorType());
        if (type == model->typeContext.neverType() && !parameter->isReceiver) {
            report(TypeCheckingDiagnosticId::invalidFunctionType, parameter->type->span,
                   "a parameter cannot have type 'Never'",
                   "a function taking a value that cannot exist could never be called");
            type = model->typeContext.errorType();
        }
        recordDeclaredType(parameter, type);
        coreCheckSuspended = false;
        if (parameter->isReceiver) {
            signature.receiverType = type;
            continue;
        }
        signature.parameters.push_back(type);
    }
    if (declaration->returnType != nullptr) {
        signature.result = resolveType(declaration->returnType).value_or(
                model->typeContext.errorType());
        requireCoreType(declaration->returnType, signature.result);
    }

    model->symbolTypes[*function] = signature.result;
    model->callables[*function] = std::move(signature);
    resolveNativeBinding(declaration, *function);
}

// `extension Box<U>` (spec 20.12.2): the header's parameters stand for the declaration's, with its
// constraints, and `Self` is `Box<U>`.
void TypeCheckingBuilder::enterExtension(const syntax::ExtensionDeclSyntax::Ptr& declaration) {
    selfOwner.reset();
    selfArguments.clear();
    const auto owner = model->semanticModelValue->referencedSymbol(declaration->extendedType);
    if (!owner.has_value()) return;
    const auto* ownerSymbol = symbolOf(*owner);
    // The one extension the role enums admit is the empty `Equatable` conformance (decision 0031, amending 0024).
    const auto onlyEquatable = [&]() {
        const auto equatable = model->equatableProtocol();
        if (!equatable.has_value() || !declaration->methods.empty() || declaration->conformances.size() != 1) {
            return false;
        }
        return model->semanticModelValue->referencedSymbol(declaration->conformances.front()) == equatable;
    };
    const bool roleEnum = ownerSymbol != nullptr &&
            (ownerSymbol->libraryRole == semantic::LibraryRole::optional ||
             ownerSymbol->libraryRole == semantic::LibraryRole::result);
    if (ownerSymbol != nullptr &&
        ((roleEnum && !onlyEquatable()) ||
         ownerSymbol->libraryRole == semantic::LibraryRole::arrayType ||
         ownerSymbol->libraryRole == semantic::LibraryRole::dictType ||
         ownerSymbol->libraryRole == semantic::LibraryRole::dictEntry) &&
        model->semanticModelValue->isPreludeSource(declaration->span)) {
        // The role enums are closed: an extension would add members the role's shape rules never saw.
        report(TypeCheckingDiagnosticId::invalidPrelude, declaration->span,
               "the compiler prelude may not extend the " + ownerSymbol->name + " role type");
    }
    if (ownerSymbol == nullptr || ownerSymbol->typeParameters.empty()) return;
    const auto& declared = model->semanticModelValue->extensionTypeParameters(declaration);
    checkExtensionHeader(declaration, *ownerSymbol, declared);
    selfOwner = *owner;
    selfParameters.clear();
    for (size_t index = 0; index < declared.size() && index < ownerSymbol->typeParameters.size(); ++index) {
        selfParameters.push_back(declared[index]);
        model->constraintLists[declared[index]] = model->constraints(ownerSymbol->typeParameters[index]);
        selfArguments.push_back(model->typeContext.typeParameterType(declared[index]));
        constrainedParameters.push_back(declared[index]);
    }
}

// Spec 20.12.2: the header lists exactly the declaration's parameters, once each, as fresh names.
void TypeCheckingBuilder::checkExtensionHeader(
        const syntax::ExtensionDeclSyntax::Ptr& declaration,
        const semantic::Symbol& owner,
        const std::vector<semantic::SymbolId>& declared) {
    const auto& extended = declaration->extendedType;
    if (extended == nullptr || extended->kind != syntax::Kind::nominalType) return;
    const auto& arguments = std::static_pointer_cast<syntax::NominalTypeSyntax>(extended)->arguments;
    const auto reason = [&](const std::string& text) {
        report(TypeCheckingDiagnosticId::invalidExtensionHeader, extended->span, text,
               "write exactly the type's parameters as new names, as in 'extension " + owner.name + "<" +
                       (owner.typeParameters.empty() ? std::string("T") : symbolName(owner.typeParameters[0])) +
                       ">' (spec 20.12.2)");
    };
    if (arguments.size() != owner.typeParameters.size()) {
        reason("'" + owner.name + "' has " + std::to_string(owner.typeParameters.size()) +
               " type parameter(s), but this extension header lists " + std::to_string(arguments.size()));
        return;
    }
    std::unordered_set<std::string> names;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto referenced = model->semanticModelValue->referencedSymbol(arguments[index]);
        const auto* symbol = referenced.has_value() ? symbolOf(*referenced) : nullptr;
        if (symbol == nullptr || symbol->kind != semantic::SymbolKind::typeParameter ||
            std::find(declared.begin(), declared.end(), *referenced) == declared.end()) {
            reason("an extension of a generic type cannot name a concrete type argument; "
                   "that would be a specialization");
            return;
        }
        if (!names.insert(symbol->name).second) {
            reason("the type parameter '" + symbol->name + "' is listed twice");
            return;
        }
    }
}

// The type parameters a generic struct or enum declares, recorded with their constraints; the
// type's own parameters are the arguments of `Self` in its body.
std::vector<TypeId> TypeCheckingBuilder::enterGenericType(
        semantic::SymbolId type, const std::vector<syntax::GenericParameterSyntax::Ptr>& declared) {
    selfOwner.reset();
    selfArguments.clear();
    if (declared.empty()) return {};
    const auto parameters = recordTypeParameters(declared);
    model->genericParameterLists[type] = parameters;
    selfOwner = type;
    selfParameters = parameters;
    for (const auto parameter : parameters) {
        selfArguments.push_back(model->typeContext.typeParameterType(parameter));
    }
    return selfArguments;
}

void TypeCheckingBuilder::resolveStructure(const syntax::StructDeclSyntax::Ptr& declaration) {
    const auto structure = model->semanticModelValue->declaredSymbol(declaration);
    if (!structure.has_value()) return;
    rejectEquatableName(declaration->name);
    for (const auto& attribute : declaration->attributes) {
        const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
        if ((name == "compiler_role" || name == "compiler_transfer") &&
            !model->semanticModelValue->isPreludeSource(attribute.span)) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                   "compiler prelude metadata is reserved for compiler-bundled declarations");
        }
    }
    const auto structureType = model->typeContext
            .typeForSymbol(*structure, enterGenericType(*structure, declaration->genericParameters))
            .value_or(model->typeContext.errorType());
    model->symbolTypes[*structure] = structureType;
    recordNodeType(declaration, structureType);

    TypedCallableSignature initializer {
        semantic::CallableKind::structureInitializer,
        true,
        {},
        structureType,
    };
    initializer.parameters.reserve(declaration->fields.size());
    for (const auto& field : declaration->fields) {
        auto fieldType = resolveType(field->type).value_or(
                model->typeContext.errorType());
        if (fieldType == model->typeContext.neverType()) {
            report(TypeCheckingDiagnosticId::neverFieldType, field->type->span,
                   "a stored field cannot have type 'Never'",
                   "a structure holding a value that cannot exist could never be constructed (spec 2.9)");
            fieldType = model->typeContext.errorType();
        }
        recordDeclaredType(field, fieldType);
        initializer.parameters.push_back(fieldType);
    }

    const auto* symbol = model->semanticModelValue->symbol(*structure);
    if (symbol != nullptr && symbol->synthesizedInitializer.has_value()) {
        model->symbolTypes[*symbol->synthesizedInitializer] = structureType;
        model->callables[*symbol->synthesizedInitializer] = std::move(initializer);
        if (const auto* parameters = model->genericParameters(*structure)) {
            model->genericParameterLists[*symbol->synthesizedInitializer] = *parameters;
        }
    }
}

void TypeCheckingBuilder::resolveEnumeration(const syntax::EnumDeclSyntax::Ptr& declaration) {
    const auto enumeration = model->semanticModelValue->declaredSymbol(declaration);
    if (!enumeration.has_value()) return;
    rejectEquatableName(declaration->name);
    for (const auto& attribute : declaration->attributes) {
        const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
        if ((name == "compiler_role" || name == "compiler_transfer") &&
            !model->semanticModelValue->isPreludeSource(attribute.span)) {
            report(TypeCheckingDiagnosticId::invalidAnnotation, attribute.span,
                   "compiler prelude metadata is reserved for compiler-bundled declarations");
        }
    }
    const auto enumerationType = model->typeContext
            .typeForSymbol(*enumeration, enterGenericType(*enumeration, declaration->genericParameters))
            .value_or(model->typeContext.errorType());
    model->symbolTypes[*enumeration] = enumerationType;
    recordNodeType(declaration, enumerationType);

    for (const auto& enumCase : declaration->cases) {
        const auto caseSymbol = model->semanticModelValue->declaredSymbol(enumCase);
        if (!caseSymbol.has_value()) continue;

        TypedCallableSignature signature {
            semantic::CallableKind::enumCase,
            enumCase->hasPayloadClause,
            {},
            enumerationType,
        };
        signature.parameters.reserve(enumCase->associatedTypes.size());
        for (const auto& associatedType : enumCase->associatedTypes) {
            signature.parameters.push_back(resolveType(associatedType->type).value_or(
                    model->typeContext.errorType()));
            enumPayloadChecks.emplace_back(signature.parameters.back(), associatedType->span);
        }
        model->symbolTypes[*caseSymbol] = enumerationType;
        model->callables[*caseSymbol] = std::move(signature);
        if (const auto* parameters = model->genericParameters(*enumeration)) {
            model->genericParameterLists[*caseSymbol] = *parameters;
        }
    }
}

} // namespace joyeer::typing
