#include "compiler/frontend/typechecking/builder.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <utility>

namespace joyeer::typing {

TypeCheckingResult TypeCheckingBuilder::build(const semantic::SemanticModel::Ptr& semanticModel) {
    assert(semanticModel != nullptr);
    model = TypeCheckedModel::Ptr(new TypeCheckedModel(semanticModel));
    resolveBuiltinSignatures();

    const auto& root = semanticModel->compilationRoot();
    if (root != nullptr) {
        // Collect signatures before checking uses: forward references and prelude roles
        // must see the same declarations regardless of source order.
        for (const auto& item : root->items) resolveTopLevelDeclaration(item);
        validatePreludeContracts();
        // A malformed prelude role must not cascade into errors in function bodies (decision 0024).
        // Declaration-time prelude errors (an extension of a role enum, a role annotation outside the
        // prelude) count as well as the shape errors.
        if (std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.id == TypeCheckingDiagnosticId::invalidPrelude;
            })) {
            return TypeCheckingResult { model, std::move(diagnostics) };
        }
        // Reject infinitely expanding layouts before any conformance walk follows their fields.
        const auto declarationDiagnostics = diagnostics.size();
        checkExpandingRecursion(true);
        if (diagnostics.size() != declarationDiagnostics) {
            return TypeCheckingResult { model, std::move(diagnostics) };
        }
        checkConstraintConflicts();
        for (const auto& item : root->items) {
            if (item->kind == syntax::Kind::extensionDecl) {
                checkConformances(std::static_pointer_cast<syntax::ExtensionDeclSyntax>(item));
            }
        }
        // Every conformance is known before a derived one is evaluated (spec 20.13.5).
        validateDerivedConformances();
        checkPendingTypeArguments();
        for (const auto& [type, span] : enumPayloadChecks) requireNoLinearDictionary(type, span);
        validateNativeSymbols();
        for (const auto& item : root->items) checkTopLevelBody(item);
        checkPendingTypeArguments();
    }
    validateDeferredReferences();
    // Instances are found only for a program that checked cleanly (decision 0021).
    if (diagnostics.empty()) checkExpandingRecursion();
    if (diagnostics.empty()) instantiate();
    return TypeCheckingResult { model, std::move(diagnostics) };
}

void TypeCheckingBuilder::report(
        TypeCheckingDiagnosticId id,
        SourceSpan span,
        std::string message,
        std::optional<std::string> help,
        std::optional<DiagnosticFixIt> fixIt,
        std::vector<DiagnosticSourceNote> notes) {
    diagnostics.push_back(TypeCheckingDiagnostic {
        id,
        span,
        std::move(message),
        std::move(help),
        std::move(fixIt),
        std::move(notes),
    });
}

void TypeCheckingBuilder::recordNodeType(const syntax::NodePtr& node, TypeId type) {
    const auto id = model->semanticModelValue->nodeId(node);
    if (id.has_value()) model->nodeTypes[*id] = type;
    requireCoreType(node, type);
}

void TypeCheckingBuilder::recordDeclaredType(const syntax::NodePtr& node, TypeId type) {
    const auto symbol = model->semanticModelValue->declaredSymbol(node);
    if (symbol.has_value()) model->symbolTypes[*symbol] = type;
    requireCoreType(node, type);
}

void TypeCheckingBuilder::recordResolvedReference(
        const syntax::NodePtr& node,
        semantic::SymbolId symbol) {
    const auto id = model->semanticModelValue->nodeId(node);
    if (id.has_value()) model->resolvedReferences[*id] = symbol;
}

void TypeCheckingBuilder::recordResolvedCallTarget(
        const syntax::NodePtr& node,
        semantic::SymbolId symbol) {
    const auto id = model->semanticModelValue->nodeId(node);
    if (id.has_value()) model->resolvedCallTargets[*id] = symbol;
}

void TypeCheckingBuilder::markDeferredReferenceHandled(const syntax::NodePtr& node) {
    if (node == nullptr || model->semanticModelValue->deferredReference(node) == nullptr) {
        return;
    }
    const auto id = model->semanticModelValue->nodeId(node);
    if (id.has_value()) handledDeferredReferences.insert(*id);
}

void TypeCheckingBuilder::validateDeferredReferences() {
    for (const auto& deferred : model->semanticModelValue->deferredReferences()) {
        if (handledDeferredReferences.contains(deferred.node)) continue;
        report(
                TypeCheckingDiagnosticId::unresolvedReference,
                deferred.span,
                "type checker did not consume deferred reference '" +
                        deferred.name + "'");
    }
}

TypeCheckingResult TypeChecker::check(
        const semantic::SemanticModel::Ptr& semanticModel) const {
    return TypeCheckingBuilder().build(semanticModel);
}

const char* typeKindName(TypeKind kind) {
    switch (kind) {
        case TypeKind::error: return "error";
        case TypeKind::voidType: return "void";
        case TypeKind::never: return "never";
        case TypeKind::any: return "any";
        case TypeKind::integer: return "integer";
        case TypeKind::boolean: return "boolean";
        case TypeKind::string: return "string";
        case TypeKind::uint8: return "uint8";
        case TypeKind::structure: return "structure";
        case TypeKind::enumeration: return "enumeration";
        case TypeKind::function: return "function";
        case TypeKind::typeParameter: return "type-parameter";
        case TypeKind::int8: return "int8";
        case TypeKind::int16: return "int16";
        case TypeKind::int32: return "int32";
        case TypeKind::int64: return "int64";
        case TypeKind::uint16: return "uint16";
        case TypeKind::uint32: return "uint32";
        case TypeKind::uint64: return "uint64";
        case TypeKind::unsignedInteger: return "uint";
        case TypeKind::float32: return "float32";
        case TypeKind::float64: return "float64";
    }
    return "unknown";
}

const char* diagnosticName(TypeCheckingDiagnosticId id) {
    switch (id) {
        case TypeCheckingDiagnosticId::invalidTypeArgumentCount:
            return "type-checking.invalid-type-argument-count";
        case TypeCheckingDiagnosticId::typeMismatch:
            return "type-checking.type-mismatch";
        case TypeCheckingDiagnosticId::missingContextualType:
            return "type-checking.missing-contextual-type";
        case TypeCheckingDiagnosticId::invalidOperatorOperands:
            return "type-checking.invalid-operator-operands";
        case TypeCheckingDiagnosticId::invalidPropagationOperand:
            return "type-checking.invalid-propagation-operand";
        case TypeCheckingDiagnosticId::invalidPropagationContext:
            return "type-checking.invalid-propagation-context";
        case TypeCheckingDiagnosticId::mismatchedPropagationError:
            return "type-checking.mismatched-propagation-error";
        case TypeCheckingDiagnosticId::unknownMember:
            return "type-checking.unknown-member";
        case TypeCheckingDiagnosticId::inaccessibleDeclaration:
            return "type-checking.inaccessible-declaration";
        case TypeCheckingDiagnosticId::notSubscriptable:
            return "type-checking.not-subscriptable";
        case TypeCheckingDiagnosticId::unknownEnumCase:
            return "type-checking.unknown-enum-case";
        case TypeCheckingDiagnosticId::enumCaseArgumentMismatch:
            return "type-checking.enum-case-argument-mismatch";
        case TypeCheckingDiagnosticId::nonExhaustiveMatch:
            return "type-checking.non-exhaustive-match";
        case TypeCheckingDiagnosticId::assignmentToImmutable:
            return "type-checking.assignment-to-immutable";
        case TypeCheckingDiagnosticId::invalidAccessMarker:
            return "type-checking.invalid-access-marker";
        case TypeCheckingDiagnosticId::invalidInoutArgument:
            return "type-checking.invalid-inout-argument";
        case TypeCheckingDiagnosticId::copyOfNoncopyable:
            return "type-checking.copy-of-noncopyable";
        case TypeCheckingDiagnosticId::assignmentToSelfInDeinit:
            return "type-checking.assignment-to-self-in-deinit";
        case TypeCheckingDiagnosticId::linearReserved:
            return "type-checking.linear-reserved";
        case TypeCheckingDiagnosticId::linearInDeinitType:
            return "type-checking.linear-in-deinit-type";
        case TypeCheckingDiagnosticId::linearCollectionElement:
            return "type-checking.linear-collection-element";
        case TypeCheckingDiagnosticId::linearTopLevelBinding:
            return "type-checking.linear-top-level-binding";
        case TypeCheckingDiagnosticId::linearStructWithoutFields:
            return "type-checking.linear-struct-without-fields";
        case TypeCheckingDiagnosticId::spawnTargetNotFunction:
            return "type-checking.spawn-target-not-function";
        case TypeCheckingDiagnosticId::spawnTargetUnsupported:
            return "type-checking.spawn-target-unsupported";
        case TypeCheckingDiagnosticId::spawnTargetParameters:
            return "type-checking.spawn-target-parameters";
        case TypeCheckingDiagnosticId::spawnTargetNever:
            return "type-checking.spawn-target-never";
        case TypeCheckingDiagnosticId::channelEndTypes:
            return "type-checking.channel-end-types";
        case TypeCheckingDiagnosticId::channelCapacity:
            return "type-checking.channel-capacity";
        case TypeCheckingDiagnosticId::linearChannelElement:
            return "type-checking.linear-channel-element";
        case TypeCheckingDiagnosticId::channelReceiverCycle:
            return "type-checking.channel-receiver-cycle";
        case TypeCheckingDiagnosticId::channelElementType:
            return "type-checking.channel-element-type";
        case TypeCheckingDiagnosticId::invalidFunctionType:
            return "type-checking.invalid-function-type";
        case TypeCheckingDiagnosticId::notAFunctionValue:
            return "type-checking.not-a-function-value";
        case TypeCheckingDiagnosticId::labeledFunctionValueCall:
            return "type-checking.labeled-function-value-call";
        case TypeCheckingDiagnosticId::neverFieldType:
            return "type-checking.never-field-type";
        case TypeCheckingDiagnosticId::protocolUsedAsType:
            return "type-checking.protocol-used-as-type";
        case TypeCheckingDiagnosticId::invalidProtocolMember:
            return "type-checking.invalid-protocol-member";
        case TypeCheckingDiagnosticId::reservedProtocolName:
            return "type-checking.reserved-protocol-name";
        case TypeCheckingDiagnosticId::invalidConformance:
            return "type-checking.invalid-conformance";
        case TypeCheckingDiagnosticId::nonEquatableMember:
            return "type-checking.non-equatable-member";
        case TypeCheckingDiagnosticId::missingWitness:
            return "type-checking.missing-witness";
        case TypeCheckingDiagnosticId::witnessMismatch:
            return "type-checking.witness-mismatch";
        case TypeCheckingDiagnosticId::witnessNotAccessible:
            return "type-checking.witness-not-accessible";
        case TypeCheckingDiagnosticId::orphanConformance:
            return "type-checking.orphan-conformance";
        case TypeCheckingDiagnosticId::duplicateConformance:
            return "type-checking.duplicate-conformance";
        case TypeCheckingDiagnosticId::invalidGenericDeclaration:
            return "type-checking.invalid-generic-declaration";
        case TypeCheckingDiagnosticId::unsatisfiedConstraint:
            return "type-checking.unsatisfied-constraint";
        case TypeCheckingDiagnosticId::cannotInferTypeParameter:
            return "type-checking.cannot-infer-type-parameter";
        case TypeCheckingDiagnosticId::conflictingTypeArgument:
            return "type-checking.conflicting-type-argument";
        case TypeCheckingDiagnosticId::linearTypeArgument:
            return "type-checking.linear-type-argument";
        case TypeCheckingDiagnosticId::invalidTypeArgument:
            return "type-checking.invalid-type-argument";
        case TypeCheckingDiagnosticId::unknownMemberOnTypeParameter:
            return "type-checking.unknown-member-on-type-parameter";
        case TypeCheckingDiagnosticId::genericNativeBinding:
            return "type-checking.generic-native-binding";
        case TypeCheckingDiagnosticId::expandingGenericRecursion:
            return "type-checking.expanding-generic-recursion";
        case TypeCheckingDiagnosticId::invalidExtensionHeader:
            return "type-checking.invalid-extension-header";
        case TypeCheckingDiagnosticId::instantiationLimit:
            return "type-checking.instantiation-limit";
        case TypeCheckingDiagnosticId::unauthorizedIntrinsic:
            return "type-checking.unauthorized-intrinsic";
        case TypeCheckingDiagnosticId::invalidPrelude:
            return "type-checking.invalid-prelude";
        case TypeCheckingDiagnosticId::invalidConsumeArgument:
            return "type-checking.invalid-consume-argument";
        case TypeCheckingDiagnosticId::invalidInitializingArgument:
            return "type-checking.invalid-initializing-argument";
        case TypeCheckingDiagnosticId::overlappingAccess:
            return "type-checking.overlapping-access";
        case TypeCheckingDiagnosticId::notCallable:
            return "type-checking.not-callable";
        case TypeCheckingDiagnosticId::invalidCallArguments:
            return "type-checking.invalid-call-arguments";
        case TypeCheckingDiagnosticId::invalidLoopControl:
            return "type-checking.invalid-loop-control";
        case TypeCheckingDiagnosticId::notIterable:
            return "type-checking.not-iterable";
        case TypeCheckingDiagnosticId::unresolvedReference:
            return "type-checking.unresolved-reference";
        case TypeCheckingDiagnosticId::invalidAnnotation:
            return "type-checking.invalid-annotation";
        case TypeCheckingDiagnosticId::invalidNativeDeclaration:
            return "type-checking.invalid-native-declaration";
        case TypeCheckingDiagnosticId::unsupportedNativeSignature:
            return "type-checking.unsupported-native-signature";
        case TypeCheckingDiagnosticId::unauthorizedNativeBinding:
            return "type-checking.unauthorized-native-binding";
        case TypeCheckingDiagnosticId::conflictingNativeSymbol:
            return "type-checking.conflicting-native-symbol";
        case TypeCheckingDiagnosticId::invalidNumericLiteral:
            return "type-checking.invalid-numeric-literal";
        case TypeCheckingDiagnosticId::managedTypeInCoreUnit:
            return "type-checking.managed-type-in-core-unit";
    }
    return "type-checking.unknown";
}

std::string dump(const std::vector<TypeCheckingDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length
            << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::typing
