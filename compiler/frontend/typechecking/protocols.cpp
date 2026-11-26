#include "builder.h"

#include <algorithm>
#include <utility>

namespace joyeer::typing {

void TypeCheckingBuilder::resolveProtocol(const syntax::ProtocolDeclSyntax::Ptr& declaration) {
    const auto name = declaration->name == nullptr ? std::string() : declaration->name->rawValue;
    const auto declared = model->semanticModelValue->declaredSymbol(declaration);
    const auto* declaredSymbol = declared.has_value() ? symbolOf(*declared) : nullptr;
    const bool roleBound = declaredSymbol != nullptr &&
            declaredSymbol->libraryRole == semantic::LibraryRole::equatable &&
            model->semanticModelValue->isPreludeSource(declaration->span);
    if ((name == "Copyable" || name == "Display" || name == "Equatable" || name == "Comparable" ||
         name == "Any") && !roleBound) {
        report(TypeCheckingDiagnosticId::reservedProtocolName,
               declaration->name == nullptr ? declaration->span : declaration->name->span,
               "'" + name + "' is reserved and cannot be declared (spec 20.2)",
               name == "Copyable" ? std::optional<std::string>("'Copyable' is the builtin protocol of spec 20.5")
                                  : std::nullopt);
    }
    for (const auto& requirement : declaration->requirements) {
        if (requirement->body != nullptr) {
            report(TypeCheckingDiagnosticId::invalidProtocolMember, requirement->body->span,
                   "a protocol requirement has no body (spec 20.2)",
                   "implement the method in a conformance extension");
        }
        if (requirement->explicitVisibility) {
            report(TypeCheckingDiagnosticId::invalidProtocolMember, requirement->span,
                   "a protocol requirement has no visibility modifier; it has the protocol's");
        }
        resolveGenericParameters(requirement, TypeCheckingDiagnosticId::invalidProtocolMember);
        resolveFunction(requirement);
    }
}

std::optional<TypeCheckingBuilder::MethodShape> TypeCheckingBuilder::methodShape(
        semantic::SymbolId method,
        const std::unordered_map<semantic::SymbolId, TypeId>& substitution) {
    const auto* symbol = symbolOf(method);
    const auto* signature = model->callable(method);
    if (symbol == nullptr || signature == nullptr || !symbol->callable.has_value()) return std::nullopt;
    MethodShape shape;
    if (symbol->callable->receiver.has_value()) shape.receiver = symbol->callable->receiver->access;
    for (const auto& parameter : symbol->callable->parameters) {
        shape.labels.push_back(parameter.label);
        shape.effects.push_back(parameter.access);
    }
    for (const auto parameter : signature->parameters) {
        shape.parameters.push_back(model->typeContext.substitute(parameter, substitution));
    }
    shape.result = model->typeContext.substitute(signature->result, substitution);
    return shape;
}

const char* TypeCheckingBuilder::effectWord(syntax::AccessEffect effect) {
    switch (effect) {
        case syntax::AccessEffect::inout: return "inout";
        case syntax::AccessEffect::consuming: return "consuming";
        case syntax::AccessEffect::initializing: return "initializing";
        case syntax::AccessEffect::borrowing: return "borrowing";
    }
    return "borrowing";
}

// Why `witness` does not match `requirement`, or nullopt when it matches.
std::optional<std::string> TypeCheckingBuilder::shapeMismatch(const MethodShape& requirement, const MethodShape& witness) const {
    if (requirement.receiver != witness.receiver) {
        return std::string("its receiver is ") + effectWord(witness.receiver) + ", the requirement's is " +
                effectWord(requirement.receiver);
    }
    if (requirement.parameters.size() != witness.parameters.size()) {
        return "it takes " + std::to_string(witness.parameters.size()) + " parameter(s), the requirement " +
                std::to_string(requirement.parameters.size());
    }
    for (size_t index = 0; index < requirement.parameters.size(); ++index) {
        const auto position = "parameter " + std::to_string(index + 1);
        if (requirement.labels[index] != witness.labels[index]) {
            return position + " is labeled '" + witness.labels[index].value_or("") + "', the requirement's '" +
                    requirement.labels[index].value_or("") + "'";
        }
        if (requirement.effects[index] != witness.effects[index]) {
            return position + " is " + effectWord(witness.effects[index]) + ", the requirement's is " +
                    effectWord(requirement.effects[index]);
        }
        if (requirement.parameters[index] != witness.parameters[index]) {
            return position + " has type '" + model->typeContext.displayName(witness.parameters[index]) +
                    "', the requirement's '" + model->typeContext.displayName(requirement.parameters[index]) + "'";
        }
    }
    if (requirement.result != witness.result) {
        return "it returns '" + model->typeContext.displayName(witness.result) + "', the requirement '" +
                model->typeContext.displayName(requirement.result) + "'";
    }
    return std::nullopt;
}

std::vector<semantic::SymbolId> TypeCheckingBuilder::requirementsOf(semantic::SymbolId protocol) const {
    std::vector<semantic::SymbolId> requirements;
    const auto* symbol = symbolOf(protocol);
    if (symbol == nullptr || !symbol->declaration.has_value()) return requirements;
    const auto node = model->semanticModelValue->node(*symbol->declaration);
    if (node == nullptr || node->kind != syntax::Kind::protocolDecl) return requirements;
    for (const auto& requirement : std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node)->requirements) {
        const auto id = model->semanticModelValue->declaredSymbol(requirement);
        if (id.has_value()) requirements.push_back(*id);
    }
    return requirements;
}

std::string TypeCheckingBuilder::requirementTitle(semantic::SymbolId requirement) const {
    const auto* symbol = symbolOf(requirement);
    if (symbol == nullptr) return "<requirement>";
    std::string title = symbol->name + "(";
    if (symbol->callable.has_value()) {
        for (const auto& parameter : symbol->callable->parameters) title += parameter.label.value_or("_") + ":";
    }
    return title + ")";
}

// Two protocols of one constraint that require one name must require the same method
// (spec 20.5): witnesses are found by name.
void TypeCheckingBuilder::checkConstraintConflicts() {
    for (const auto parameter : constrainedParameters) {
        const auto& protocols = model->constraints(parameter);
        if (protocols.size() < 2) continue;
        const std::unordered_map<semantic::SymbolId, TypeId> none;
        std::unordered_map<std::string, std::pair<semantic::SymbolId, semantic::SymbolId>> seen;
        for (const auto protocol : protocols) {
            const std::unordered_map<semantic::SymbolId, TypeId> self {
                { protocol, model->typeContext.typeParameterType(parameter) } };
            for (const auto requirement : requirementsOf(protocol)) {
                const auto name = symbolName(requirement);
                const auto previous = seen.find(name);
                if (previous == seen.end()) {
                    seen.emplace(name, std::pair { protocol, requirement });
                    continue;
                }
                const std::unordered_map<semantic::SymbolId, TypeId> previousSelf {
                    { previous->second.first, model->typeContext.typeParameterType(parameter) } };
                const auto first = methodShape(previous->second.second, previousSelf);
                const auto second = methodShape(requirement, self);
                if (!first.has_value() || !second.has_value() || !shapeMismatch(*first, *second).has_value()) {
                    continue;
                }
                report(TypeCheckingDiagnosticId::invalidConformance, symbolOf(parameter)->span,
                       "protocols '" + symbolName(previous->second.first) + "' and '" + symbolName(protocol) +
                               "' both require '" + name + "' with different signatures, so no type can "
                               "satisfy the constraint of '" + symbolName(parameter) + "'");
            }
        }
    }
}

// Spec 20.3: each requirement has exactly one matching, accessible witness, and at most one
// conformance of a type to a protocol exists.
void TypeCheckingBuilder::checkConformances(const syntax::ExtensionDeclSyntax::Ptr& extension) {
    if (extension->conformances.empty()) return;
    const auto extended = model->typeOf(extension->extendedType);
    const auto typeSymbolId = model->semanticModelValue->referencedSymbol(extension->extendedType);
    if (!extended.has_value() || !typeSymbolId.has_value() || *extended == model->typeContext.errorType()) return;
    const auto* typeSymbol = symbolOf(*typeSymbolId);
    if (typeSymbol == nullptr) return;
    // `extension Box<U>` states the conformance of `Box<T>`, the declaration's own instance: the
    // header's parameters are renamed to the declaration's (spec 20.12.2).
    std::unordered_map<semantic::SymbolId, TypeId> toDeclaration;
    const auto& headerParameters = model->semanticModelValue->extensionTypeParameters(extension);
    if (const auto* declared = model->genericParameters(*typeSymbolId)) {
        for (size_t index = 0; index < headerParameters.size() && index < declared->size(); ++index) {
            toDeclaration.emplace(headerParameters[index], model->typeContext.typeParameterType((*declared)[index]));
        }
    }
    const auto subject = toDeclaration.empty() ? *extended : model->typeContext.substitute(*extended, toDeclaration);
    const auto copyable = model->copyableProtocol();
    const auto module = model->semanticModelValue->moduleOf(extension->span);
    std::vector<semantic::SymbolId> listed;
    for (const auto& conformance : extension->conformances) {
        if (!checkProtocolReference(conformance)) continue;
        const auto referenced = model->semanticModelValue->referencedSymbol(conformance);
        if (!referenced.has_value()) continue;
        const auto* protocol = symbolOf(*referenced);
        if (protocol == nullptr || protocol->kind != semantic::SymbolKind::protocol) {
            report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                   "'" + symbolName(*referenced) + "' is not a protocol; an extension can only conform to protocols");
            continue;
        }
        if (copyable.has_value() && *referenced == *copyable) {
            report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                   "'Copyable' cannot be declared; a type is copyable exactly when spec 4.1.1 says so");
            continue;
        }
        if (std::find(listed.begin(), listed.end(), *referenced) != listed.end()) {
            report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                   "'" + protocol->name + "' is listed twice");
            continue;
        }
        listed.push_back(*referenced);
        if (const auto equatable = model->equatableProtocol();
            equatable.has_value() && *referenced == *equatable && model->isLinear(subject)) {
            report(TypeCheckingDiagnosticId::invalidConformance, conformance->span,
                   "the linear type '" + typeSymbol->name + "' cannot conform to 'Equatable' (spec 20.13.3)",
                   "a linear value must be ended exactly once, and its value equality has no meaning; compare "
                   "a field instead");
            continue;
        }
        if (module != typeSymbol->moduleId && module != protocol->moduleId) {
            report(TypeCheckingDiagnosticId::orphanConformance, conformance->span,
                   "a conformance of '" + typeSymbol->name + "' to '" + protocol->name +
                           "' must be declared in the module of the type or of the protocol (spec 20.3)");
            continue;
        }
        const auto duplicate = std::find_if(model->conformanceList.begin(), model->conformanceList.end(),
                [&](const Conformance& existing) {
                    return model->conformanceSubjectMatches(existing.type, subject) &&
                            existing.protocol == *referenced;
                });
        if (duplicate != model->conformanceList.end()) {
            report(TypeCheckingDiagnosticId::duplicateConformance, conformance->span,
                   "'" + typeSymbol->name + "' already conforms to '" + protocol->name + "'",
                   std::nullopt, std::nullopt,
                   { sourceNote(duplicate->span, "the first conformance is here") });
            continue;
        }
        // With no member named `equals`, an `Equatable` conformance is derived (spec 20.13.3, 20.13.5).
        if (const auto equatable = model->equatableProtocol();
            equatable.has_value() && *referenced == *equatable && !lookupMember(*extended, "equals").has_value()) {
            Conformance derived { subject, *referenced, {}, conformance->span };
            derived.derived = true;
            model->conformanceList.push_back(std::move(derived));
            derivedConformanceChecks.push_back({ subject, conformance->span, model->conformanceList.size() - 1 });
            continue;
        }
        Conformance result { subject, *referenced, {}, conformance->span };
        const auto conformanceVisibility = std::min(protocol->visibility, typeSymbol->visibility);
        const std::unordered_map<semantic::SymbolId, TypeId> self { { *referenced, subject } };
        bool valid = true;
        for (const auto requirement : requirementsOf(*referenced)) {
            const auto witness = lookupMember(*extended, symbolName(requirement));
            const auto* witnessSymbol = witness.has_value() ? symbolOf(*witness) : nullptr;
            if (witnessSymbol == nullptr || witnessSymbol->kind != semantic::SymbolKind::method) {
                report(TypeCheckingDiagnosticId::missingWitness, conformance->span,
                       "'" + typeSymbol->name + "' does not implement '" + requirementTitle(requirement) +
                               "' required by '" + protocol->name + "'",
                       witnessSymbol == nullptr
                               ? std::optional<std::string>("declare the method in this extension")
                               : witnessSymbol->kind == semantic::SymbolKind::builtinMember
                               ? std::optional<std::string>("a builtin member is not a declared method and cannot be a witness")
                               : std::optional<std::string>("'" + witnessSymbol->name + "' is not a method, and only "
                                                            "a declared method can be a witness; rename it"));
                valid = false;
                continue;
            }
            const auto required = methodShape(requirement, self);
            // A witness declared in the type or in an extension names that declaration's own
            // parameters; compare it as the declaration's instance.
            std::unordered_map<semantic::SymbolId, TypeId> witnessToDeclaration;
            if (const auto* own = parametersOfMember(*witness)) {
                if (const auto* declared = model->genericParameters(*typeSymbolId)) {
                    for (size_t index = 0; index < own->size() && index < declared->size(); ++index) {
                        witnessToDeclaration.emplace(
                                (*own)[index], model->typeContext.typeParameterType((*declared)[index]));
                    }
                }
            }
            const auto provided = methodShape(*witness, witnessToDeclaration);
            if (!required.has_value() || !provided.has_value()) { valid = false; continue; }
            if (const auto reason = shapeMismatch(*required, *provided)) {
                report(TypeCheckingDiagnosticId::witnessMismatch, witnessSymbol->span,
                       "'" + typeSymbol->name + "." + witnessSymbol->name + "' does not match '" +
                               requirementTitle(requirement) + "' of '" + protocol->name + "': " + *reason,
                       std::nullopt, std::nullopt,
                       { sourceNote(symbolOf(requirement)->span, "the requirement is here") });
                valid = false;
                continue;
            }
            if (std::min(witnessSymbol->visibility, typeSymbol->visibility) < conformanceVisibility ||
                !model->semanticModelValue->isAccessible(*witness, conformance->span)) {
                report(TypeCheckingDiagnosticId::witnessNotAccessible, witnessSymbol->span,
                       "'" + typeSymbol->name + "." + witnessSymbol->name +
                               "' is less accessible than the conformance to '" + protocol->name + "'",
                       "give the method the visibility of the protocol and the type");
                valid = false;
                continue;
            }
            result.witnesses.emplace(requirement, *witness);
        }
        // An invalid conformance is still recorded, so a call does not also report it as missing.
        (void) valid;
        model->conformanceList.push_back(std::move(result));
    }
}

// Whether `type` conforms to `Equatable` (spec 20.13.3, 20.13.5). A derived conformance holds when every member
// conforms; while a type is being decided, a nested question about it is answered yes, so recursive types
// terminate (nothing provisional is cached). A type parameter conforms when its constraint lists `Equatable`, or,
// with `assumeParameters`, always: that asks whether the type can conform for some arguments.
bool TypeCheckingBuilder::satisfiesEquatable(TypeId type, std::vector<TypeId>& inProgress, bool assumeParameters) const {
    if (type == model->typeContext.errorType()) return true;
    if (model->isBuiltinEquatable(type)) return true;
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return false;
    if (record->kind == TypeKind::typeParameter) {
        if (assumeParameters) return true;
        const auto& protocols = model->constraints(record->symbol);
        const auto equatable = model->equatableProtocol();
        return equatable.has_value() && std::find(protocols.begin(), protocols.end(), *equatable) != protocols.end();
    }
    // `[X]` conforms when `X` does, through the `array_equal` witness (spec 20.13.3, item 4).
    if (model->isArrayRoleType(type)) {
        const auto arguments = record->arguments;
        return model->arrayEqualFunction().has_value() && !arguments.empty() &&
                satisfiesEquatable(arguments[0], inProgress, assumeParameters);
    }
    if (model->isDictRoleType(type)) return false;
    const bool genericType = !record->arguments.empty() &&
            (record->kind == TypeKind::structure || record->kind == TypeKind::enumeration);
    if (model->typeContext.mentionsTypeParameter(type) && !genericType) return false;
    const auto* conformance = model->equatableConformance(type);
    if (conformance == nullptr) return false;
    if (!conformance->derived || conformance->invalid) return true;
    if (std::find(inProgress.begin(), inProgress.end(), type) != inProgress.end()) return true;
    // Once every conformance is known, `false` is cached for good: it holds whatever is assumed. `true` is kept
    // provisionally and kept for good only when the outermost question (asked with nothing in progress) is
    // answered yes, since it may rest on an assumption about a type in progress.
    const auto mode = assumeParameters ? 1 : 0;
    const bool outermost = inProgress.empty();
    if (equatableCacheEnabled) {
        if (equatableFalse[mode].contains(type)) return false;
        if (equatableTrue[mode].contains(type) || equatableProvisional[mode].contains(type)) return true;
    }
    inProgress.push_back(type);
    bool conforms = true;
    for (const auto& member : model->equatableMembers(type)) {
        // `Never` has no value, so a member of that type never compares (spec 20.13.5).
        if (member.type == model->typeContext.neverType()) continue;
        if (!satisfiesEquatable(member.type, inProgress, assumeParameters)) { conforms = false; break; }
    }
    inProgress.pop_back();
    if (equatableCacheEnabled) {
        if (!conforms) equatableFalse[mode].insert(type);
        else equatableProvisional[mode].insert(type);
        if (outermost) {
            if (conforms) equatableTrue[mode].insert(equatableProvisional[mode].begin(), equatableProvisional[mode].end());
            equatableProvisional[mode].clear();
        }
    }
    return conforms;
}

void TypeCheckingBuilder::clearEquatableCache() const {
    for (int mode = 0; mode < 2; ++mode) {
        equatableTrue[mode].clear();
        equatableFalse[mode].clear();
        equatableProvisional[mode].clear();
    }
}

// The first member path of a derived conformer that keeps it from conforming, and that member's type:
// `value.run` and `(Int): Int` for a `Box<Holder>` whose `Holder` has a function field.
std::optional<std::pair<std::string, TypeId>> TypeCheckingBuilder::nonEquatablePath(TypeId type, size_t depth) const {
    const auto* conformance = model->equatableConformance(type);
    if (conformance == nullptr || !conformance->derived || conformance->invalid) return std::nullopt;
    for (const auto& member : model->equatableMembers(type)) {
        if (member.type == model->typeContext.neverType()) continue;
        std::vector<TypeId> inProgress { type };
        const bool conforms = satisfiesEquatable(member.type, inProgress, false);
        equatableProvisional[0].clear();
        if (conforms) continue;
        if (member.type != type && depth < 32) {
            if (const auto inner = nonEquatablePath(member.type, depth + 1)) {
                return std::pair { member.name + "." + inner->first, inner->second };
            }
        }
        return std::pair { member.name, member.type };
    }
    return std::nullopt;
}

// A derived conformance whose member can never conform, for any type arguments, is rejected at its extension
// (spec 20.13.5); one that conforms only for some arguments is checked where it is used.
void TypeCheckingBuilder::validateDerivedConformances() {
    // Every conformance is recorded, so answers can be cached; a rejected conformance changes them, so the
    // rejections are applied together at the end and the cache restarts.
    equatableCacheEnabled = true;
    const auto failingMembers = [&](TypeId subject) {
        std::vector<EquatableMember> failing;
        for (const auto& member : model->equatableMembers(subject)) {
            if (member.type == model->typeContext.neverType()) continue;
            std::vector<TypeId> inProgress { subject };
            const bool conforms = satisfiesEquatable(member.type, inProgress, true);
            equatableProvisional[1].clear();
            if (!conforms) failing.push_back(member);
        }
        return failing;
    };
    // First every failing conformance is found, then all are marked rejected together; a rejected conformance
    // counts as satisfied, so a member whose own conformance is the mistake is not reported a second time.
    std::vector<size_t> rejected;
    for (const auto& check : derivedConformanceChecks) {
        if (!failingMembers(check.subject).empty()) rejected.push_back(check.conformance);
    }
    for (const auto index : rejected) model->conformanceList[index].invalid = true;
    clearEquatableCache();
    for (const auto& [subject, span, conformanceIndex] : derivedConformanceChecks) {
        if (!model->conformanceList[conformanceIndex].invalid) continue;
        for (const auto& member : failingMembers(subject)) {
            const auto memberType = model->typeContext.displayName(member.type);
            const auto help = equatableHelp(member.type, span).value_or("") +
                    "; or write 'equals(other:)' by hand";
            report(TypeCheckingDiagnosticId::nonEquatableMember, span,
                   "'" + model->typeContext.displayName(subject) + "' cannot derive 'Equatable': its member '" +
                           member.name + "' has the type '" + memberType + "', which does not conform",
                   help);
        }
    }
    clearEquatableCache();
}

// Whether `type` satisfies `protocol` (spec 20.4.2). Inside a generic body a type that mentions
// a parameter satisfies a user protocol only when it is that parameter and its constraint
// lists the protocol.
bool TypeCheckingBuilder::satisfies(TypeId type, semantic::SymbolId protocol) const {
    if (type == model->typeContext.errorType()) return true;
    const auto copyable = model->copyableProtocol();
    if (copyable.has_value() && protocol == *copyable) return model->isCopyable(type);
    const auto equatable = model->equatableProtocol();
    if (equatable.has_value() && protocol == *equatable) {
        std::vector<TypeId> inProgress;
        return satisfiesEquatable(type, inProgress, false);
    }
    const auto* record = model->typeContext.type(type);
    if (record == nullptr) return false;
    if (record->kind == TypeKind::typeParameter) {
        const auto& protocols = model->constraints(record->symbol);
        return std::find(protocols.begin(), protocols.end(), protocol) != protocols.end();
    }
    const bool genericType = !record->arguments.empty() &&
            (record->kind == TypeKind::structure || record->kind == TypeKind::enumeration);
    // An explicit extension of a generic type conforms every instance, including one built from a
    // parameter (spec 20.12.2); a builtin container built from a parameter satisfies nothing.
    if (model->typeContext.mentionsTypeParameter(type) && !genericType) return false;
    return std::any_of(model->conformanceList.begin(), model->conformanceList.end(),
            [&](const Conformance& conformance) {
                return model->conformanceSubjectMatches(conformance.type, type) &&
                        conformance.protocol == protocol;
            });
}

} // namespace joyeer::typing
