#ifndef __joyeer_compiler_typechecking_h__
#define __joyeer_compiler_typechecking_h__

#include "joyeer/compiler/semantic.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
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
};

struct TypeRecord {
    TypeId id = invalidTypeId;
    TypeKind kind = TypeKind::error;
    semantic::SymbolId symbol = semantic::invalidSymbolId;
    std::vector<TypeId> arguments;
};

// Owns canonical compile-time types for one SemanticModel. This is deliberately
// independent of the legacy runtime Type hierarchy used by the bytecode VM.
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

    [[nodiscard]] TypeId arrayType(TypeId element);
    [[nodiscard]] TypeId dictionaryType(TypeId key, TypeId value);
    [[nodiscard]] TypeId optionalType(TypeId wrapped);
    [[nodiscard]] TypeId resultType(TypeId success, TypeId failure);

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
            std::vector<TypeId> arguments = {});
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
};

struct TypedCallableSignature {
    semantic::CallableKind kind;
    bool acceptsArgumentClause = true;
    std::vector<TypeId> parameters;
    TypeId result = invalidTypeId;
};

enum class TypeCheckingDiagnosticId {
    invalidTypeArgumentCount,
    typeMismatch,
    missingContextualType,
};

struct TypeCheckingDiagnostic {
    TypeCheckingDiagnosticId id;
    SourceSpan span;
    std::string message;
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
    [[nodiscard]] const TypedCallableSignature* callable(semantic::SymbolId symbol) const;

private:
    friend class TypeCheckingBuilder;

    explicit TypeCheckedModel(semantic::SemanticModel::Ptr semanticModel);

    semantic::SemanticModel::Ptr semanticModelValue;
    TypeContext typeContext;
    std::unordered_map<semantic::NodeId, TypeId> nodeTypes;
    std::unordered_map<semantic::SymbolId, TypeId> symbolTypes;
    std::unordered_map<semantic::SymbolId, TypedCallableSignature> callables;
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