#ifndef __joyeer_compiler_typechecking_h__
#define __joyeer_compiler_typechecking_h__

#include "semantic.h"
#include "nativebinding.h"
#include "compiler/diagnostics/diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joyeer::typing {

using TypeId = uint32_t;

inline constexpr TypeId invalidTypeId = std::numeric_limits<TypeId>::max();

enum class TypeKind {
    error,
    voidType,
    never,
    any,
    integer,
    boolean,
    string,
    uint8,
    structure,
    enumeration,
    array,
    dictionary,
    optional,
    result,
    int8,
    int16,
    int32,
    int64,
    uint16,
    uint32,
    uint64,
    unsignedInteger,
    float32,
    float64,
    // `JoinHandle<R>`: a running task whose result type is the one argument (spec 21).
    task,
    // `ChannelSender<T>` and `ChannelReceiver<T>`: the ends of a channel (decision 0019).
    channelSender,
    channelReceiver,
    // `(E1 T1, ..., En Tn): R` (spec 2.3.1, decision 0020): `arguments` holds the parameter types and
    // then the result; `effects` holds one access effect per parameter.
    function,
    // A type parameter of a generic function (spec 20.4), or `Self` inside a protocol: `symbol` is
    // the parameter or the protocol. It is distinct from every other type (spec 20.4) and never
    // reaches IR: each instance substitutes concrete types (decision 0021).
    typeParameter,
};

[[nodiscard]] constexpr bool isSignedInteger(TypeKind kind) {
    return kind == TypeKind::integer || kind == TypeKind::int8 ||
            kind == TypeKind::int16 || kind == TypeKind::int32 || kind == TypeKind::int64;
}

[[nodiscard]] constexpr bool isUnsignedInteger(TypeKind kind) {
    return kind == TypeKind::unsignedInteger || kind == TypeKind::uint8 ||
            kind == TypeKind::uint16 || kind == TypeKind::uint32 || kind == TypeKind::uint64;
}

[[nodiscard]] constexpr bool isInteger(TypeKind kind) {
    return isSignedInteger(kind) || isUnsignedInteger(kind);
}

[[nodiscard]] constexpr bool isFloating(TypeKind kind) {
    return kind == TypeKind::float32 || kind == TypeKind::float64;
}

[[nodiscard]] constexpr bool isNumeric(TypeKind kind) {
    return isInteger(kind) || isFloating(kind);
}

[[nodiscard]] constexpr bool isNativeScalar(TypeKind kind) {
    return isNumeric(kind) || kind == TypeKind::boolean;
}

[[nodiscard]] constexpr unsigned scalarBitWidth(TypeKind kind) {
    switch (kind) {
        case TypeKind::boolean: return 1;
        case TypeKind::int8:
        case TypeKind::uint8: return 8;
        case TypeKind::int16:
        case TypeKind::uint16: return 16;
        case TypeKind::int32:
        case TypeKind::uint32:
        case TypeKind::float32: return 32;
        case TypeKind::integer:
        case TypeKind::int64:
        case TypeKind::unsignedInteger:
        case TypeKind::uint64:
        case TypeKind::float64: return 64;
        default: return 0;
    }
}

[[nodiscard]] constexpr bool sameNativeRepresentation(TypeKind left, TypeKind right) {
    return left == right || (isInteger(left) && isInteger(right) &&
            isSignedInteger(left) == isSignedInteger(right) &&
            scalarBitWidth(left) == scalarBitWidth(right));
}

struct TypeRecord {
    TypeId id = invalidTypeId;
    TypeKind kind = TypeKind::error;
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::vector<TypeId> arguments;
    // Only for `TypeKind::function`: one access effect per parameter.
    std::vector<syntax::AccessEffect> effects;
};

enum class NativeBufferKind {
    none,
    string,
    bytes,
    strings,
    byteArrays,
};

// Both frontend and verified IR use this boundary; nested native storage is
// limited to borrowed lists of strings or byte arrays, never arbitrary values.
template<class Lookup>
[[nodiscard]] NativeBufferKind nativeBufferKind(TypeId id, const Lookup& lookup) {
    const auto* value = lookup(id);
    if (value == nullptr) return NativeBufferKind::none;
    if (value->kind == TypeKind::string) return NativeBufferKind::string;
    if (value->kind != TypeKind::array || value->arguments.size() != 1) {
        return NativeBufferKind::none;
    }
    const auto* element = lookup(value->arguments[0]);
    if (element == nullptr) return NativeBufferKind::none;
    if (element->kind == TypeKind::uint8) return NativeBufferKind::bytes;
    if (element->kind == TypeKind::string) return NativeBufferKind::strings;
    if (element->kind == TypeKind::array && element->arguments.size() == 1) {
        const auto* byte = lookup(element->arguments[0]);
        if (byte != nullptr && byte->kind == TypeKind::uint8) return NativeBufferKind::byteArrays;
    }
    return NativeBufferKind::none;
}

// Owns canonical compile-time types for one SemanticModel.
class TypeContext {
public:
    explicit TypeContext(const semantic::SemanticModel& model);

    [[nodiscard]] TypeId errorType() const;
    [[nodiscard]] TypeId voidType() const;
    [[nodiscard]] TypeId neverType() const;
    [[nodiscard]] TypeId anyType() const;
    [[nodiscard]] TypeId intType() const;
    [[nodiscard]] TypeId boolType() const;
    [[nodiscard]] TypeId stringType() const;
    [[nodiscard]] TypeId uint8Type() const;
    [[nodiscard]] TypeId doubleType() const;
    [[nodiscard]] TypeId ioErrorType() const;

    [[nodiscard]] TypeId arrayType(TypeId element);
    [[nodiscard]] TypeId dictionaryType(TypeId key, TypeId value);
    [[nodiscard]] TypeId optionalType(TypeId wrapped);
    [[nodiscard]] TypeId resultType(TypeId success, TypeId failure);
    [[nodiscard]] TypeId taskType(TypeId result);
    [[nodiscard]] TypeId channelSenderType(TypeId element);
    [[nodiscard]] TypeId channelReceiverType(TypeId element);
    // `Result<success, ChannelClosed>`, what `send` and `receive` give.
    [[nodiscard]] TypeId channelResultType(TypeId success);
    // The type of a generic parameter, or of `Self` in a protocol when `symbol` is the protocol.
    [[nodiscard]] TypeId typeParameterType(semantic::SymbolId symbol);
    // `type` with every type parameter in `substitution` replaced; parameters not in it stay.
    [[nodiscard]] TypeId substitute(
            TypeId type, const std::unordered_map<semantic::SymbolId, TypeId>& substitution);
    // True when `type` is or contains a type parameter.
    [[nodiscard]] bool mentionsTypeParameter(TypeId type) const;
    // How deeply `type` nests type arguments: 0 for a scalar or nominal type.
    [[nodiscard]] size_t nestingDepth(TypeId type) const;
    // A function type: `parameters` and `effects` have one entry per parameter (spec 2.3.1).
    [[nodiscard]] TypeId functionType(
            const std::vector<TypeId>& parameters,
            const std::vector<syntax::AccessEffect>& effects,
            TypeId result);

    // Resolves a type declaration symbol plus concrete built-in generic
    // arguments. Invalid arity and non-type symbols return nullopt.
    [[nodiscard]] std::optional<TypeId> typeForSymbol(
            semantic::SymbolId symbol,
            const std::vector<TypeId>& arguments = {});
    [[nodiscard]] std::optional<semantic::SymbolId> builtinSymbol(
            std::string_view name) const;
        [[nodiscard]] std::optional<size_t> typeArity(semantic::SymbolId symbol) const;

    [[nodiscard]] const TypeRecord* type(TypeId id) const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] std::string displayName(TypeId id) const;
    [[nodiscard]] bool isAssignable(TypeId source, TypeId destination) const;

private:
    struct TypeKey {
        TypeKind kind;
        semantic::SymbolId symbol;
        std::vector<TypeId> arguments;
        std::vector<syntax::AccessEffect> effects;

        bool operator==(const TypeKey& other) const = default;
    };

    struct TypeKeyHash {
        size_t operator()(const TypeKey& key) const noexcept;
    };

    struct GenericConstructor {
        TypeKind kind;
        size_t arity;
    };

    [[nodiscard]] TypeId intern(
            TypeKind kind,
            semantic::SymbolId symbol,
            std::vector<TypeId> arguments = {},
            std::vector<syntax::AccessEffect> effects = {});
    [[nodiscard]] semantic::SymbolId requireBuiltinSymbol(const std::string& name);
    void registerConcreteBuiltin(const std::string& name, TypeKind kind, TypeId& destination);
    void registerGenericBuiltin(const std::string& name, TypeKind kind, size_t arity);

    const semantic::SemanticModel& model;
    std::vector<TypeRecord> types;
    std::unordered_map<TypeKey, TypeId, TypeKeyHash> internedTypes;
    std::unordered_map<std::string, semantic::SymbolId> builtinSymbols;
    std::unordered_map<semantic::SymbolId, TypeId> zeroArgumentTypes;
    std::unordered_map<semantic::SymbolId, GenericConstructor> genericConstructors;

    TypeId errorTypeId = invalidTypeId;
    TypeId voidTypeId = invalidTypeId;
    TypeId neverTypeId = invalidTypeId;
    TypeId anyTypeId = invalidTypeId;
    TypeId intTypeId = invalidTypeId;
    TypeId boolTypeId = invalidTypeId;
    TypeId stringTypeId = invalidTypeId;
    TypeId uint8TypeId = invalidTypeId;
    TypeId ioErrorTypeId = invalidTypeId;
    TypeId doubleTypeId = invalidTypeId;
};

struct TypedCallableSignature {
    semantic::CallableKind kind;
    bool acceptsArgumentClause = true;
    std::vector<TypeId> parameters;
    TypeId result = invalidTypeId;
    // Set for a method: the type of its implicit `self`, which is not in `parameters`.
    std::optional<TypeId> receiverType;
};

enum class TypeCheckingDiagnosticId {
    invalidTypeArgumentCount,
    typeMismatch,
    missingContextualType,
    invalidOperatorOperands,
    invalidPropagationOperand,
    invalidPropagationContext,
    mismatchedPropagationError,
    unknownMember,
    inaccessibleDeclaration,
    notSubscriptable,
    unknownEnumCase,
    enumCaseArgumentMismatch,
    nonExhaustiveMatch,
    assignmentToImmutable,
    invalidAccessMarker,
    invalidInoutArgument,
    invalidConsumeArgument,
    invalidInitializingArgument,
    overlappingAccess,
    notCallable,
    invalidCallArguments,
    invalidLoopControl,
    notIterable,
    unresolvedReference,
    invalidAnnotation,
    invalidNativeDeclaration,
    unsupportedNativeSignature,
    unauthorizedNativeBinding,
    conflictingNativeSymbol,
    invalidNumericLiteral,
    managedTypeInCoreUnit,
    copyOfNoncopyable,
    assignmentToSelfInDeinit,
    linearReserved,
    linearInDeinitType,
    linearCollectionElement,
    linearTopLevelBinding,
    linearStructWithoutFields,
    spawnTargetNotFunction,
    spawnTargetUnsupported,
    spawnTargetParameters,
    spawnTargetNever,
    channelEndTypes,
    channelCapacity,
    linearChannelElement,
    channelElementType,
    // Function values (decision 0020).
    invalidFunctionType,
    notAFunctionValue,
    labeledFunctionValueCall,
    // Protocols and generic functions (spec 20.8, decision 0021).
    protocolUsedAsType,
    invalidProtocolMember,
    reservedProtocolName,
    invalidConformance,
    missingWitness,
    witnessMismatch,
    witnessNotAccessible,
    orphanConformance,
    duplicateConformance,
    invalidGenericDeclaration,
    unsatisfiedConstraint,
    cannotInferTypeParameter,
    conflictingTypeArgument,
    linearTypeArgument,
    invalidTypeArgument,
    unknownMemberOnTypeParameter,
    genericNativeBinding,
    expandingGenericRecursion,
    instantiationLimit,
};

// A conformance of a concrete type to a protocol (spec 20.3), with the witness of each requirement.
struct Conformance {
    TypeId type = invalidTypeId;
    semantic::SymbolId protocol = semantic::invalidSymbolId;
    std::unordered_map<semantic::SymbolId, semantic::SymbolId> witnesses;
    SourceSpan span;
};

// A call of a generic function, with its inferred type arguments in parameter order. Inside another
// generic function they may mention that function's parameters (`caller`).
struct GenericCall {
    semantic::SymbolId callee = semantic::invalidSymbolId;
    std::vector<TypeId> arguments;
    std::optional<semantic::SymbolId> caller;
    SourceSpan span;
};

// One instance of a generic function (spec 20.6, decision 0021): the function, its concrete type
// arguments, and what required it, for the instantiation report.
struct Instance {
    semantic::SymbolId function = semantic::invalidSymbolId;
    std::vector<TypeId> arguments;
    std::unordered_map<semantic::SymbolId, TypeId> substitution;
    // `totalArea<Square>`: the name in debug information and in the report.
    std::string name;
    // The call that first required it, and the instance that call is in, if it is in one.
    SourceSpan requiredAt;
    std::optional<size_t> requiredBy;
    std::optional<semantic::SymbolId> requiredByFunction;
};

struct TypeCheckingDiagnostic {
    TypeCheckingDiagnosticId id;
    SourceSpan span;
    std::string message;
    std::optional<std::string> help;
    std::optional<DiagnosticFixIt> fixIt;
    std::vector<DiagnosticSourceNote> notes;
};

class TypeCheckingBuilder;

class TypeCheckedModel {
public:
    using Ptr = std::shared_ptr<TypeCheckedModel>;

    [[nodiscard]] const semantic::SemanticModel::Ptr& semanticModel() const;
    [[nodiscard]] TypeContext& types();
    [[nodiscard]] const TypeContext& types() const;
    [[nodiscard]] std::optional<TypeId> typeOf(const syntax::NodePtr& node) const;
    [[nodiscard]] std::optional<TypeId> typeOf(semantic::SymbolId symbol) const;
    // Every typed node, for lowering's per-instance checks of a generic body.
    [[nodiscard]] const std::unordered_map<semantic::NodeId, TypeId>& nodeTypeTable() const { return nodeTypes; }
    [[nodiscard]] const TypedCallableSignature* callable(semantic::SymbolId symbol) const;
    [[nodiscard]] const NativeBinding* nativeBinding(semantic::SymbolId symbol) const;
        [[nodiscard]] std::optional<semantic::SymbolId> referencedSymbol(
            const syntax::NodePtr& node) const;
        [[nodiscard]] std::optional<semantic::SymbolId> callTarget(
            const syntax::NodePtr& node) const;

    // Copyability (spec 4.1.1, decision 0015). A struct that declares `deinit` is noncopyable,
    // and so is anything that holds one: a struct field, an enum payload, or the element, key,
    // value, payload or error type of Array, Dict, Optional and Result.
    [[nodiscard]] bool isCopyable(TypeId type) const;
    // Why `type` is noncopyable, or nullopt when it is copyable.
    [[nodiscard]] std::optional<std::string> noncopyableReason(TypeId type) const;
    // The `deinit` declaration of a struct type, or nullptr when `type` has none. Only the type
    // that declares it qualifies; a struct that merely holds one is not a `deinit` type.
    [[nodiscard]] syntax::FunctionDeclSyntax::Ptr deinitializerOf(TypeId type) const;
    // Linear types (decision 0015, "Linear types"): never destroyed implicitly, so a binding must
    // be ended before its scope exits. A struct declared `linear` is linear, and so is anything that
    // holds one (a struct field, an enum payload, or the element, key, value, payload or error type
    // of Array, Dict, Optional and Result). Every linear type is noncopyable.
    [[nodiscard]] bool isLinear(TypeId type) const;
    // The field paths a binding of `type` must consume to be ended. A linear aggregate (a struct
    // that is linear only because a field is) ends field by field, so each linear field path is
    // one entry; any other linear type is a single leaf, the empty path. Empty when not linear.
    [[nodiscard]] std::vector<std::vector<semantic::SymbolId>> linearLeafPaths(TypeId type) const;
    // True for a struct declared with the `linear` modifier.
    [[nodiscard]] bool isDeclaredLinear(TypeId type) const;
    // Generic functions (spec 20, decision 0021).
    // The type parameters of a generic function in declaration order; null for any other function.
    [[nodiscard]] const std::vector<semantic::SymbolId>* genericParameters(semantic::SymbolId function) const;
    // The protocols a type parameter is constrained to, `Copyable` included.
    [[nodiscard]] const std::vector<semantic::SymbolId>& constraints(semantic::SymbolId typeParameter) const;
    [[nodiscard]] std::optional<semantic::SymbolId> copyableProtocol() const;
    // The generic call `node` makes, or null.
    [[nodiscard]] const GenericCall* genericCall(const syntax::NodePtr& node) const;
    // The witness of `requirement` for the concrete `type`, or nullopt without a conformance.
    [[nodiscard]] std::optional<semantic::SymbolId> witness(TypeId type, semantic::SymbolId requirement) const;
    [[nodiscard]] const std::vector<Conformance>& conformances() const { return conformanceList; }
    // Every instance the program needs, in the order they were found.
    [[nodiscard]] const std::vector<Instance>& instances() const { return instanceList; }
    [[nodiscard]] std::optional<size_t> instanceIndex(
            semantic::SymbolId function, const std::vector<TypeId>& arguments) const;
    // Where the program starts a task and where it calls System's `runProcess` (spec 21.6).
    [[nodiscard]] const std::vector<SourceSpan>& spawnCalls() const { return spawnCallSpans; }
    [[nodiscard]] const std::vector<SourceSpan>& runProcessCalls() const { return runProcessCallSpans; }

private:
    [[nodiscard]] syntax::StructDeclSyntax::Ptr structureDeclaration(TypeId type) const;
    [[nodiscard]] bool isLinear(
            TypeId type, std::vector<TypeId>& visiting, std::unordered_set<TypeId>& clean) const;
    friend class TypeCheckingBuilder;

    [[nodiscard]] std::optional<std::string> noncopyableReason(
            TypeId type, std::vector<TypeId>& visiting, std::unordered_set<TypeId>& clean) const;

    explicit TypeCheckedModel(semantic::SemanticModel::Ptr semanticModel);

    semantic::SemanticModel::Ptr semanticModelValue;
    TypeContext typeContext;
    // Where the program starts a task and calls System's `runProcess`; the two cannot meet (spec 21.6).
    std::vector<SourceSpan> spawnCallSpans;
    std::vector<SourceSpan> runProcessCallSpans;
    std::unordered_map<semantic::NodeId, TypeId> nodeTypes;
    std::unordered_map<semantic::SymbolId, TypeId> symbolTypes;
    std::unordered_map<semantic::SymbolId, TypedCallableSignature> callables;
    std::unordered_map<semantic::SymbolId, NativeBinding> nativeBindings;
    std::unordered_map<semantic::NodeId, semantic::SymbolId> resolvedReferences;
    std::unordered_map<semantic::NodeId, semantic::SymbolId> resolvedCallTargets;
    std::unordered_map<semantic::SymbolId, std::vector<semantic::SymbolId>> genericParameterLists;
    std::unordered_map<semantic::SymbolId, std::vector<semantic::SymbolId>> constraintLists;
    std::unordered_map<semantic::NodeId, GenericCall> genericCalls;
    std::vector<Conformance> conformanceList;
    std::vector<Instance> instanceList;
    std::map<std::pair<semantic::SymbolId, std::vector<TypeId>>, size_t> instanceIndices;
};

struct TypeCheckingResult {
    TypeCheckedModel::Ptr model;
    std::vector<TypeCheckingDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

class TypeChecker {
public:
    [[nodiscard]] TypeCheckingResult check(
            const semantic::SemanticModel::Ptr& semanticModel) const;
};

[[nodiscard]] const char* typeKindName(TypeKind kind);
[[nodiscard]] const char* diagnosticName(TypeCheckingDiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<TypeCheckingDiagnostic>& diagnostics);

} // namespace joyeer::typing

#endif