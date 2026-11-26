#include "builder.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace joyeer::typing {

// Spec 20.6: a cycle of generic calls in which a type parameter is passed on inside a larger
// type would need infinitely many instances. Edges run from a caller's parameter to the
// callee's parameter it reaches; an edge is expanding when the argument strictly contains it.
// `typesOnly` runs right after the declarations are resolved, before any body: a type that
// expands forever must be reported before a copyability or linearity walk follows its layout.
void TypeCheckingBuilder::checkExpandingRecursion(bool typesOnly) {
    struct Edge {
        semantic::SymbolId from;
        semantic::SymbolId to;
        bool expanding;
        SourceSpan span;
        bool inType = false;
    };
    std::vector<Edge> edges;
    std::unordered_map<semantic::SymbolId, std::vector<size_t>> outgoing;
    std::vector<std::pair<semantic::NodeId, const GenericCall*>> calls;
    for (const auto& [node, call] : model->genericCalls) calls.emplace_back(node, &call);
    std::sort(calls.begin(), calls.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [node, call] : calls) {
        if (typesOnly || !call->caller.has_value()) continue;
        const auto* callerParameters = model->genericParameters(*call->caller);
        const auto* calleeParameters = model->genericParameters(call->callee);
        if (callerParameters == nullptr || calleeParameters == nullptr) continue;
        for (size_t index = 0; index < call->arguments.size() && index < calleeParameters->size(); ++index) {
            const auto argument = call->arguments[index];
            for (const auto parameter : *callerParameters) {
                if (!mentionsAny(argument, { parameter })) continue;
                const auto* record = model->typeContext.type(argument);
                const bool direct = record != nullptr && record->kind == TypeKind::typeParameter;
                outgoing[parameter].push_back(edges.size());
                edges.push_back({ parameter, (*calleeParameters)[index], !direct, call->span });
            }
        }
    }
    // A field or payload of a generic type that names a generic type passes the declaration's
    // parameters on, so the same rule applies to types: `struct S<T> { var next: S<Box<T>> }` has
    // infinitely many layouts (spec 20.12.4).
    std::vector<semantic::SymbolId> declarations;
    for (const auto& [symbolId, parameters] : model->genericParameterLists) {
        const auto* declared = symbolOf(symbolId);
        if (declared != nullptr && (declared->kind == semantic::SymbolKind::structure ||
                                    declared->kind == semantic::SymbolKind::enumeration)) {
            declarations.push_back(symbolId);
        }
    }
    std::sort(declarations.begin(), declarations.end());
    for (const auto declaration : declarations) {
        const auto* declared = symbolOf(declaration);
        const auto& parameters = model->genericParameterLists.at(declaration);
        if (!declared->declaration.has_value()) continue;
        const auto node = model->semanticModelValue->node(*declared->declaration);
        if (node == nullptr) continue;
        std::vector<TypeId> memberTypes;
        const auto addMember = [&](semantic::SymbolId member) {
            if (const auto type = model->typeOf(member)) memberTypes.push_back(*type);
        };
        if (node->kind == syntax::Kind::structDecl) {
            for (const auto& field : std::static_pointer_cast<syntax::StructDeclSyntax>(node)->fields) {
                if (const auto member = model->semanticModelValue->declaredSymbol(field)) addMember(*member);
            }
        } else if (node->kind == syntax::Kind::enumDecl) {
            for (const auto& enumCase : std::static_pointer_cast<syntax::EnumDeclSyntax>(node)->cases) {
                const auto caseSymbol = model->semanticModelValue->declaredSymbol(enumCase);
                const auto* signature = caseSymbol.has_value() ? model->callable(*caseSymbol) : nullptr;
                if (signature != nullptr) {
                    for (const auto payload : signature->parameters) memberTypes.push_back(payload);
                }
            }
        }
        std::vector<TypeId> pending = std::move(memberTypes);
        std::unordered_set<TypeId> seen;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (!seen.insert(current).second) continue;
            const auto* record = model->typeContext.type(current);
            if (record == nullptr) continue;
            const auto* targetParameters = (record->kind == TypeKind::structure ||
                                            record->kind == TypeKind::enumeration)
                    ? model->genericParameters(record->symbol) : nullptr;
            for (size_t index = 0; index < record->arguments.size(); ++index) {
                pending.push_back(record->arguments[index]);
                if (targetParameters == nullptr || index >= targetParameters->size()) continue;
                for (const auto parameter : parameters) {
                    if (!mentionsAny(record->arguments[index], { parameter })) continue;
                    const auto* argument = model->typeContext.type(record->arguments[index]);
                    const bool direct = argument != nullptr && argument->kind == TypeKind::typeParameter;
                    outgoing[parameter].push_back(edges.size());
                    edges.push_back({ parameter, (*targetParameters)[index], !direct, declared->span, true });
                }
            }
        }
    }
    // An expanding edge from `a` to `b` closes a cycle when `a` is reachable from `b`.
    for (const auto& edge : edges) {
        if (!edge.expanding) continue;
        std::vector<semantic::SymbolId> stack { edge.to };
        std::unordered_set<semantic::SymbolId> visited { edge.to };
        bool cycle = edge.to == edge.from;
        while (!stack.empty() && !cycle) {
            const auto current = stack.back();
            stack.pop_back();
            for (const auto next : outgoing[current]) {
                const auto target = edges[next].to;
                if (target == edge.from) { cycle = true; break; }
                if (visited.insert(target).second) stack.push_back(target);
            }
        }
        if (!cycle) continue;
        if (edge.inType) {
            report(TypeCheckingDiagnosticId::expandingGenericRecursion, edge.span,
                   "type parameter '" + symbolName(edge.from) + "' is passed on inside a larger type by a field or "
                   "case payload in a cycle of generic types, which would need infinitely many layouts (spec 20.12.4)",
                   "make the recursive field use the same type arguments");
            return;
        }
        report(TypeCheckingDiagnosticId::expandingGenericRecursion, edge.span,
               "this call passes type parameter '" + symbolName(edge.from) +
                       "' on inside a larger type in a cycle of generic calls, which would need "
                       "infinitely many instances (spec 20.6)",
               "make the recursion keep the same type arguments, or move it into a non-generic function");
        return;
    }
}

bool TypeCheckingBuilder::validateChannelReceiverOwnership(
        TypeId element, SourceSpan span, std::optional<size_t> requiredBy) {
    struct Component {
        TypeId type;
        size_t parent;
        std::string member;
    };
    std::vector<Component> components { { element, 0, {} } };
    std::unordered_set<TypeId> seen { element };
    for (size_t index = 0; index < components.size(); ++index) {
        if (components.size() > maximumTypeOccurrences) {
            report(TypeCheckingDiagnosticId::instantiationLimit, span,
                   "cannot check channel receiver ownership in more than " +
                           std::to_string(maximumTypeOccurrences) + " stored types",
                   std::nullopt, std::nullopt, instantiationChain(requiredBy));
            return false;
        }
        const auto type = components[index].type;
        const auto* record = model->typeContext.type(type);
        if (record == nullptr) continue;
        const auto symbol = record->symbol;
        const auto* declaration = symbolOf(symbol);
        const auto role = declaration == nullptr ? semantic::LibraryRole::none : declaration->libraryRole;
        if (role == semantic::LibraryRole::channelReceiver) {
            if (record->arguments.size() == 1 && record->arguments.front() == element) {
                std::vector<std::string> path;
                for (auto node = index; node != 0; node = components[node].parent) {
                    path.push_back(components[node].member);
                }
                const auto name = model->typeContext.displayName(element);
                std::string component = name;
                for (auto part = path.rbegin(); part != path.rend(); ++part) component += *part;
                report(TypeCheckingDiagnosticId::channelReceiverCycle, span,
                       "'" + component + "' can own a receiver for a channel of '" + name + "'",
                       "keep that receiver out of the message or use a different reply element type",
                       std::nullopt, instantiationChain(requiredBy));
                return false;
            }
            continue;
        }
        if (role == semantic::LibraryRole::channelSender) continue;

        const auto append = [&](TypeId child, std::string member) {
            if (seen.insert(child).second) components.push_back({ child, index, std::move(member) });
        };
        if (model->isCollectionRoleType(type)) {
            for (size_t item = 0; item < record->arguments.size(); ++item) {
                append(record->arguments[item], model->isArrayRoleType(type)
                        ? "[element]" : (item == 0 ? "[key]" : "[value]"));
            }
        } else if (const auto structure = model->structureDeclaration(type)) {
            for (const auto& field : structure->fields) {
                const auto id = model->semanticModelValue->declaredSymbol(field);
                const auto fieldType = id.has_value() ? model->typeOf(*id) : std::nullopt;
                if (fieldType.has_value()) {
                    append(model->memberTypeOf(type, *fieldType), "." + field->name->rawValue);
                }
            }
        } else if (record->kind == TypeKind::enumeration) {
            if (declaration == nullptr || !declaration->declaration.has_value()) continue;
            const auto& node = model->semanticModelValue->node(*declaration->declaration);
            if (node == nullptr || node->kind != syntax::Kind::enumDecl) continue;
            for (const auto& enumCase : std::static_pointer_cast<syntax::EnumDeclSyntax>(node)->cases) {
                const auto id = model->semanticModelValue->declaredSymbol(enumCase);
                const auto* signature = id.has_value() ? model->callable(*id) : nullptr;
                if (signature == nullptr) continue;
                for (size_t payload = 0; payload < signature->parameters.size(); ++payload) {
                    append(model->memberTypeOf(type, signature->parameters[payload]),
                           "." + enumCase->name->rawValue + "(" + std::to_string(payload + 1) + ")");
                }
            }
        }
    }
    return true;
}

void TypeCheckingBuilder::instantiate() {
    std::vector<std::pair<semantic::NodeId, const GenericCall*>> calls;
    for (const auto& [node, call] : model->genericCalls) {
        const auto source = model->semanticModelValue->node(node);
        // The arguments are checked, but a diverging argument prevents the call from reaching its instance.
        if (source != nullptr && source->kind == syntax::Kind::callExpr &&
            std::any_of(std::static_pointer_cast<syntax::CallExprSyntax>(source)->arguments.begin(),
                        std::static_pointer_cast<syntax::CallExprSyntax>(source)->arguments.end(),
                        [&](const auto& argument) {
                            return model->typeOf(argument->value) == model->typeContext.neverType();
                        })) {
            continue;
        }
        calls.emplace_back(node, &call);
    }
    std::sort(calls.begin(), calls.end(), [](const auto& left, const auto& right) {
        return left.second->span.sourceId != right.second->span.sourceId
                ? left.second->span.sourceId < right.second->span.sourceId
                : left.second->span.offset < right.second->span.offset;
    });
    std::unordered_map<semantic::SymbolId, std::vector<const GenericCall*>> callsIn;
    std::vector<const GenericCall*> roots;
    for (const auto& [node, call] : calls) {
        if (call->caller.has_value() && model->genericParameters(*call->caller) != nullptr) {
            callsIn[*call->caller].push_back(call);
        }
        else roots.push_back(call);
    }
    // Breadth first, so an instance's "required by" is the shortest chain from a root.
    std::vector<size_t> queue;
    const auto require = [&](const GenericCall& call, std::vector<TypeId> arguments,
                             std::optional<size_t> requiredBy) -> bool {
        for (const auto argument : arguments) {
            std::vector<TypeId> pending { argument };
            size_t occurrences = 0;
            while (!pending.empty() && occurrences <= maximumTypeOccurrences) {
                const auto* type = model->typeContext.type(pending.back());
                pending.pop_back();
                ++occurrences;
                if (type != nullptr) pending.insert(pending.end(), type->arguments.begin(), type->arguments.end());
            }
            if (occurrences > maximumTypeOccurrences) {
                report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                       "this call needs an instance whose type arguments contain more than " +
                               std::to_string(maximumTypeOccurrences) + " type occurrences",
                       std::nullopt, std::nullopt, instantiationChain(requiredBy));
                return false;
            }
            if (model->typeContext.nestingDepth(argument) > maximumTypeNesting) {
                report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                       "this call needs an instance of '" + symbolName(call.callee) +
                               "' whose type arguments nest more than " + std::to_string(maximumTypeNesting) +
                               " levels deep",
                       std::nullopt, std::nullopt, instantiationChain(requiredBy));
                return false;
            }
        }
        const auto depth = requiredBy.has_value() ? model->instanceList[*requiredBy].depth + 1 : 1;
        if (const auto existing = model->instanceIndices.find({call.callee, arguments});
            existing != model->instanceIndices.end()) {
            auto& instance = model->instanceList[existing->second];
            const auto before = call.span.sourceId < instance.requiredAt.sourceId ||
                    (call.span.sourceId == instance.requiredAt.sourceId &&
                     call.span.offset < instance.requiredAt.offset);
            if (depth < instance.depth || (depth == instance.depth && before)) {
                instance.depth = depth;
                instance.requiredAt = call.span;
                instance.requiredBy = requiredBy;
                instance.requiredByFunction = requiredBy.has_value()
                        ? std::optional<semantic::SymbolId>(model->instanceList[*requiredBy].function)
                        : call.caller;
            }
            return true;
        }
        if (depth > maximumInstanceDepth) {
            report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                   "this call needs an instantiation chain longer than " +
                           std::to_string(maximumInstanceDepth),
                   std::nullopt, std::nullopt, instantiationChain(requiredBy));
            return false;
        }
        const auto* callee = symbolOf(call.callee);
        if (callee != nullptr && callee->libraryRole == semantic::LibraryRole::channelCreate &&
            arguments.size() == 1 && !model->typeContext.mentionsTypeParameter(arguments.front()) &&
            !validateChannelReceiverOwnership(arguments.front(), call.span, requiredBy)) {
            return false;
        }
        if (model->instanceList.size() >= maximumInstances) {
            report(TypeCheckingDiagnosticId::instantiationLimit, call.span,
                   "the program needs more than " + std::to_string(maximumInstances) +
                           " instances of generic functions",
                   std::nullopt, std::nullopt, instantiationChain(requiredBy));
            return false;
        }
        Instance instance;
        instance.function = call.callee;
        instance.arguments = arguments;
        const auto* parameters = model->genericParameters(call.callee);
        std::string arguments_text;
        for (size_t index = 0; parameters != nullptr && index < parameters->size(); ++index) {
            instance.substitution.emplace((*parameters)[index], arguments[index]);
            if (index != 0) arguments_text += ", ";
            arguments_text += model->typeContext.displayName(arguments[index], true);
        }
        // A method or `deinit` of a generic type is named for the receiver's instance:
        // `Box<Int>.get` (decision 0022).
        auto name = model->functionDisplayName(call.callee);
        const auto* owner = callee != nullptr && callee->containingSymbol.has_value()
                ? symbolOf(*callee->containingSymbol) : nullptr;
        const auto ownerEnd = owner == nullptr ? std::string::npos : name.rfind(owner->name + ".");
        if (ownerEnd != std::string::npos && !owner->typeParameters.empty()) {
            name.insert(ownerEnd + owner->name.size(), "<" + arguments_text + ">");
            instance.name = name;
        } else {
            instance.name = name + "<" + arguments_text + ">";
        }
        instance.requiredAt = call.span;
        instance.requiredBy = requiredBy;
        instance.depth = depth;
        instance.requiredByFunction = requiredBy.has_value()
                ? std::optional<semantic::SymbolId>(model->instanceList[*requiredBy].function)
                : call.caller;
        model->instanceIndices.emplace(std::pair { call.callee, arguments }, model->instanceList.size());
        queue.push_back(model->instanceList.size());
        model->instanceList.push_back(std::move(instance));
        return true;
    };
    for (const auto* call : roots) {
        if (!require(*call, call->arguments, std::nullopt)) return;
    }
    // The witness `==` needs on a concrete type (decision 0031): an instance of a generic hand-written witness,
    // or a derived witness, which needs its members' witnesses in turn.
    const auto equatableRequirement = model->equatableRequirement();
    std::function<bool(TypeId, std::optional<semantic::SymbolId>, SourceSpan, std::optional<size_t>)>
            requireEquality = [&](TypeId type, std::optional<semantic::SymbolId> caller, SourceSpan span,
                                  std::optional<size_t> requiredBy) -> bool {
        if (!equatableRequirement.has_value() || model->isBuiltinEquatable(type) ||
            model->typeContext.mentionsTypeParameter(type)) {
            return true;
        }
        if (model->isArrayRoleType(type)) {
            // The instance's own `==` on its element reaches the element's witness.
            const auto function = model->arrayEqualFunction();
            const auto* record = model->typeContext.type(type);
            if (!function.has_value() || record == nullptr || record->arguments.empty()) return true;
            const std::vector<TypeId> arguments { record->arguments[0] };
            return require(GenericCall { *function, arguments, caller, span }, arguments, requiredBy);
        }
        const auto* conformance = model->equatableConformance(type);
        if (conformance == nullptr) return true;
        if (!conformance->derived) {
            const auto found = conformance->witnesses.find(*equatableRequirement);
            const auto* receiver = model->typeContext.type(type);
            if (found == conformance->witnesses.end() || receiver == nullptr || receiver->arguments.empty() ||
                model->genericParameters(found->second) == nullptr) {
                return true;
            }
            const auto witness = found->second;
            const auto arguments = receiver->arguments;
            return require(GenericCall { witness, arguments, caller, span }, arguments, requiredBy);
        }
        if (!model->derivedEqualitySet.insert(type).second) return true;
        model->derivedEqualityList.push_back(type);
        for (const auto& member : model->equatableMembers(type)) {
            if (member.type == model->typeContext.neverType()) continue;
            if (!requireEquality(member.type, caller, span, requiredBy)) return false;
        }
        return true;
    };
    // A root in a generic body is reached only when that body is instantiated (spec 20.6).
    std::unordered_map<semantic::SymbolId, std::vector<const EqualityRoot*>> equalityRootsIn;
    for (const auto& root : equalityRoots) {
        if (root.caller.has_value() && model->genericParameters(*root.caller) != nullptr) {
            equalityRootsIn[*root.caller].push_back(&root);
            continue;
        }
        if (!requireEquality(root.type, root.caller, root.span, std::nullopt)) return;
    }
    // The `deinit` of every concrete instance of a generic struct is an instance too (spec 20.12);
    // finding one can intern new types, so the two are run until neither adds anything.
    const auto requireDeinitInstances = [&]() -> std::optional<bool> {
        bool added = false;
        for (TypeId id = 0; id < model->typeContext.size(); ++id) {
            const auto* record = model->typeContext.type(id);
            if (record == nullptr || record->kind != TypeKind::structure || record->arguments.empty() ||
                model->typeContext.mentionsTypeParameter(id)) {
                continue;
            }
            const auto* symbol = symbolOf(record->symbol);
            if (symbol == nullptr || !symbol->declaration.has_value()) continue;
            const auto node = model->semanticModelValue->node(*symbol->declaration);
            if (node == nullptr || node->kind != syntax::Kind::structDecl) continue;
            const auto deinitializer = std::static_pointer_cast<syntax::StructDeclSyntax>(node)->deinitializer;
            if (deinitializer == nullptr) continue;
            const auto deinit = model->semanticModelValue->declaredSymbol(deinitializer);
            if (!deinit.has_value() || model->genericParameters(*deinit) == nullptr) continue;
            const auto arguments = record->arguments;
            // The role-lowered uses of a library array reach its element addressor and, when copyable,
            // its copy method with no source call (decision 0025).
            std::vector<semantic::SymbolId> required { *deinit };
            if (model->isArrayRoleType(id)) {
                // A validated role always has all three methods; skip a malformed one rather than assume.
                if (const auto element = model->arrayRoleMethod(id, semantic::LibraryRole::arrayElement)) {
                    required.push_back(*element);
                }
                if (const auto copy = model->arrayRoleMethod(id, semantic::LibraryRole::arrayCopy);
                    copy.has_value() && model->isCopyable(id)) {
                    required.push_back(*copy);
                }
                if (const auto literal = model->arrayRoleMethod(id, semantic::LibraryRole::arrayLiteral)) {
                    required.push_back(*literal);
                }
                // `s.utf8()` builds a `[UInt8]` (decision 0028 stage 3); no other array needs the method.
                if (const auto utf8 = model->arrayRoleMethod(id, semantic::LibraryRole::stringUtf8);
                    utf8.has_value() && arguments == std::vector<TypeId> { model->typeContext.uint8Type() }) {
                    required.push_back(*utf8);
                }
            }
            if (model->isDictRoleType(id)) {
                // The role-lowered uses of a dictionary reach its addressor, `dict_set`, its literal method and,
                // when copyable, its copy method; `dict_get` only through user calls (decision 0027).
                for (const auto role : { semantic::LibraryRole::dictElement, semantic::LibraryRole::dictSet,
                                         semantic::LibraryRole::dictLiteral }) {
                    if (const auto method = model->collectionRoleMethod(id, role)) required.push_back(*method);
                }
                if (const auto copy = model->collectionRoleMethod(id, semantic::LibraryRole::dictCopy);
                    copy.has_value() && model->isCopyable(id)) {
                    required.push_back(*copy);
                }
                // The entry has no `deinit`, so this loop would not intern it; a literal packs it.
                // `record` may be stale: `isCopyable` above can intern types and grow the type table.
                const auto found = model->collectionRoles.find(symbol->id);
                if (!model->dictEntryTypes.contains(id) && found != model->collectionRoles.end() &&
                    found->second.entry.has_value()) {
                    if (const auto entry = model->typeContext.typeForSymbol(*found->second.entry, arguments)) {
                        model->dictEntryTypes[id] = *entry;
                    }
                }
            }
            for (const auto method : required) {
                if (model->genericParameters(method) == nullptr ||
                    model->instanceIndex(method, arguments).has_value()) {
                    continue;
                }
                const GenericCall call { method, arguments, std::nullopt, deinitializer->span };
                if (!require(call, arguments, std::nullopt)) return std::nullopt;
                added = true;
            }
        }
        return added;
    };
    size_t next = 0;
    while (true) {
    for (; next < queue.size(); ++next) {
        const auto index = queue[next];
        const auto function = model->instanceList[index].function;
        for (const auto* call : callsIn[function]) {
            std::vector<TypeId> arguments;
            for (const auto argument : call->arguments) {
                arguments.push_back(model->typeContext.substitute(argument, model->instanceList[index].substitution));
            }
            if (!require(*call, std::move(arguments), index)) return;
        }
        // A requirement call whose receiver this instance fixes to an instance of a generic type
        // reaches an instance of that type's witness.
        for (const auto& requirementCall : requirementCalls) {
            if (requirementCall.caller != function) continue;
            const auto concrete = model->typeContext.substitute(
                    requirementCall.base, model->instanceList[index].substitution);
            if (equatableRequirement.has_value() && requirementCall.requirement == *equatableRequirement) {
                if (!requireEquality(concrete, function, requirementCall.span, index)) return;
                continue;
            }
            const auto witness = model->witness(concrete, requirementCall.requirement);
            const auto* receiver = model->typeContext.type(concrete);
            if (!witness.has_value() || receiver == nullptr || receiver->arguments.empty() ||
                model->genericParameters(*witness) == nullptr) {
                continue;
            }
            const GenericCall call { *witness, receiver->arguments, function, requirementCall.span };
            if (!require(call, receiver->arguments, index)) return;
        }
        if (const auto found = equalityRootsIn.find(function); found != equalityRootsIn.end()) {
            for (const auto* root : found->second) {
                if (!requireEquality(root->type, function, root->span, index)) return;
            }
        }
    }
    internInstanceTypes();
    const auto more = requireDeinitInstances();
    if (!more.has_value()) return;
    if (!*more) break;
    }
}

// Lowering substitutes every type it reads, and the IR type table is a snapshot taken before
// it starts, so each instance's concrete types are interned here.
void TypeCheckingBuilder::internInstanceTypes() {
    const auto known = model->typeContext.size();
    for (TypeId id = 0; id < known; ++id) {
        if (!model->typeContext.mentionsTypeParameter(id)) continue;
        for (size_t index = 0; index < model->instanceList.size(); ++index) {
            (void) model->typeContext.substitute(id, model->instanceList[index].substitution);
        }
    }
    internInstanceMemberTypes();
}

// Each concrete instance of a generic struct or enum needs its field and payload types
// interned before lowering snapshots the table (spec 20.12). Interning one can create another
// instance (`Box<Box<Int>>` holds a `Box<Int>`), so this runs to a fixpoint; the expanding
// recursion rule makes it finite, and the cap is a backstop.
void TypeCheckingBuilder::internInstanceMemberTypes() {
    size_t visited = 0;
    while (visited < model->typeContext.size()) {
        const auto id = static_cast<TypeId>(visited++);
        const auto* record = model->typeContext.type(id);
        if (record == nullptr || record->arguments.empty() || model->typeContext.mentionsTypeParameter(id) ||
            (record->kind != TypeKind::structure && record->kind != TypeKind::enumeration)) {
            continue;
        }
        const auto* symbol = symbolOf(record->symbol);
        if (symbol == nullptr || !symbol->declaration.has_value()) continue;
        const auto node = model->semanticModelValue->node(*symbol->declaration);
        if (node == nullptr) continue;
        if (node->kind == syntax::Kind::structDecl) {
            for (const auto& field : std::static_pointer_cast<syntax::StructDeclSyntax>(node)->fields) {
                const auto fieldSymbol = model->semanticModelValue->declaredSymbol(field);
                const auto declared = fieldSymbol.has_value() ? model->typeOf(*fieldSymbol) : std::nullopt;
                if (declared.has_value()) (void) model->memberTypeOf(id, *declared);
            }
        } else if (node->kind == syntax::Kind::enumDecl) {
            for (const auto& enumCase : std::static_pointer_cast<syntax::EnumDeclSyntax>(node)->cases) {
                const auto caseSymbol = model->semanticModelValue->declaredSymbol(enumCase);
                const auto* signature = caseSymbol.has_value() ? model->callable(*caseSymbol) : nullptr;
                if (signature == nullptr) continue;
                for (const auto payload : signature->parameters) (void) model->memberTypeOf(id, payload);
            }
        }
        if (model->typeContext.size() > maximumTypeTableSize) {
            report(TypeCheckingDiagnosticId::instantiationLimit, symbol->span,
                   "instantiating '" + symbol->name + "' needs more than " +
                           std::to_string(maximumTypeTableSize) + " distinct types");
            return;
        }
    }
}

std::vector<DiagnosticSourceNote> TypeCheckingBuilder::instantiationChain(std::optional<size_t> instance) const {
    std::vector<DiagnosticSourceNote> chain;
    while (instance.has_value()) {
        const auto& current = model->instanceList[*instance];
        chain.push_back(sourceNote(current.requiredAt, "required for '" + current.name + "'"));
        instance = current.requiredBy;
    }
    // The first three and the last three steps are enough to see the shape of a chain.
    if (chain.size() > 6) chain.erase(chain.begin() + 3, chain.end() - 3);
    return chain;
}

} // namespace joyeer::typing
