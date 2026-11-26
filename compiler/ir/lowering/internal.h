#ifndef __joyeer_ir_lowering_internal_h__
#define __joyeer_ir_lowering_internal_h__

// Private to IR lowering: compiler/ir/irlowering.cpp and compiler/ir/lowering/*.cpp. Nothing else
// includes this header; the public interface is compiler/ir/irlowering.h. Lowering is part of the
// compiler core only; the backend never compiles it.
//
// How lowering is organized
// -------------------------
// Lowerer::lower() creates one Builder and calls build(). The Builder walks the type-checked model
// once and appends to one ir::Module. Diagnostics are reported in walk order. A report abandons
// only the construct being lowered; later statements and functions are still lowered, so a program
// always reports the same diagnostics in the same order.
//
//   1. build() copies the source tables, then collects module-wide data before any code:
//      snapshotTypes() copies the type table; collectAggregateDefinitions() adds every struct and
//      enum definition, with one per concrete instance of a generic type. The destruction
//      analysis is built from those definitions.
//   2. collectFunctions() declares every IR function before any body is lowered, so a call can
//      name any function: the prelude externals, top-level functions (recording top-level
//      constants as it meets them), methods and `deinit`s, one function per generic instance
//      (decision 0021), and one derived `Equatable` witness per concrete type the program needs
//      (decision 0031). It then links each struct to its `deinit` (rebuilding the destruction
//      analysis) and evaluates the constants. The contracts prelude's functions and the String
//      role functions are the exception: demandFunction() declares one when lowering first calls
//      or names it.
//   3. lowerFunctions() lowers the bodies in the same order: top-level functions, methods,
//      generic instances, derived witnesses, then every prelude function lowering demanded,
//      including those first demanded by another prelude body. While an instance is lowered,
//      `currentInstance` is set, and typeOf() returns types with its type arguments substituted.
//   4. lowerFunction() spills by-value parameters to stack slots (an address parameter is its own
//      slot) and lowers the body block. Expressions go through lowerExpression(), which
//      dispatches on the syntax kind; places that are assigned, moved or returned go through
//      lowerAddress().
//   5. build() checks that lowering did not need a type the type checker never interned, and, if
//      nothing was reported, runs the IR verifier and reports its errors as diagnostics.
//
// Ownership runs through every part. Each value is recorded as trivial, borrowed or owned. Owned
// temporaries and owning storage are cleanups of the innermost scope (`scopes`); acquireOwned()
// transfers a temporary to a new owner by removing it from `liveOwnedTemporaries`, so its scope's
// cleanup skips it. Leaving a scope emits its remaining cleanups, newest first; `break`, `continue`
// and `return` emit those of every scope they leave. Destruction order is observable (a `deinit`
// runs code), so keep the order cleanups are registered.
//
// Where each part lives
// ---------------------
//   module.cpp       build(), the driver; diagnostics; types substituted for the current instance
//   definitions.cpp  the type table, struct and enum definitions, generic type instances
//   functions.cpp    function declarations, generic function instances, `deinit` links, bodies
//   constants.cpp    top-level `let` constants
//   debuginfo.cpp    debug scopes and debug variables
//   ownership.cpp    value ownership, owned operands, scope cleanups and destruction
//   statements.cpp   blocks, local bindings, `if`, `while`, `break`, `continue`, `return`
//   expressions.cpp  the expression dispatcher, literals, names, operators, member reads
//   equality.cpp     `Equatable` equality, `== nil`, and the derived witnesses
//   places.cpp       assignment and the addresses of locals, fields and elements (spec 4.4.3)
//   collections.cpp  array and dictionary literals, subscripts and element addresses
//   enums.cpp        enum case lookup, construction, payloads, `?` propagation
//   patterns.cpp     `match`, patterns and pattern bindings
//   calls.cpp        direct and indirect calls, argument conventions, struct construction
//   intrinsics.cpp   the layout intrinsics and `String.utf8`
//   emission.cpp     the instruction builder: values, blocks, branches, stores, conversions
//
// The member functions below are declared in the same groups and order, one group per file. The
// exceptions are build(), declared first as the public entry point and defined in module.cpp, and
// the template forEachMethod(), defined in this header.

#include "compiler/ir/constants.h"
#include "compiler/ir/irlowering.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace joyeer::lowering::detail {

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

// Restores the current debug scope when a lowered block or match arm ends.
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

// Lowers one type-checked model to one IR module. One instance lowers one module.
class Builder {
public:
    // Lowers a type-checked model to a module, then verifies it if lowering reported nothing.
    Result build(
            const typing::TypeCheckedModel::Ptr& checkedModel,
            std::string sourceName,
            std::optional<ir::SourceInfo> sourceInfo,
            std::vector<ir::SourceInfo> sources);

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
    // The generated witness of each derived `Equatable` conformance the program needs (decision 0031).
    std::unordered_map<typing::TypeId, ir::FunctionId> derivedEqualityFunctions;
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
    const std::unordered_set<uint64_t>* currentNoncopyableSpans = nullptr;

    // An assignment target whose index operands are already evaluated but
    // whose element address is not yet computed. Element addresses point into
    // storage the right-hand side may reallocate (for example by appending to
    // the same array), so they are taken only after the right side is lowered.
    // With `ownKeys`, an index that needs destruction is held as an owned
    // temporary, so a later operand that consumes or reassigns the binding it
    // was read from cannot leave the deferred lookup with a dangling key.
    using DeferredAddress = std::function<std::optional<ir::Value>()>;

    // ---- module.cpp: the driver, diagnostics, and the substituted types lowering reads
    // Appends one diagnostic. Lowering continues after a report; `build` returns them all.
    void report(DiagnosticId id, SourceSpan span, std::string message);
    // Keys a semantic scope or symbol id by the current generic instance (slot 0 outside one).
    uint64_t instanceKey(uint32_t id) const;
    // `type` with the current instance's type arguments substituted; unchanged outside an instance.
    typing::TypeId substituted(typing::TypeId type) const;
    // The type of a syntax node, substituted for the current instance.
    std::optional<typing::TypeId> typeOf(const syntax::NodePtr& node) const;
    // The type of a symbol, substituted for the current instance.
    std::optional<typing::TypeId> typeOf(semantic::SymbolId symbol) const;
    // Whether a value of `node` is copied: in an instance the generic body's type decides (0021).
    bool copyableInBody(const syntax::NodePtr& node, typing::TypeId concrete) const;
    // Packs a source span into one key for `noncopyableSpans`.
    static uint64_t spanKey(SourceSpan span);
    // The spans of a generic body whose type is noncopyable there; computed once per function.
    const std::unordered_set<uint64_t>& noncopyableSpans(const syntax::FunctionDeclSyntax::Ptr& declaration);

    // ---- definitions.cpp: the type table and the struct and enum definitions
    // Copies the type checker's type table into the module, one IR type per type id.
    void snapshotTypes();
    // Collects every struct and enum definition, generic instances and builtin enums included.
    void collectAggregateDefinitions();
    // Collects one definition per concrete instance of each generic struct or enum.
    void collectGenericTypeInstances(const syntax::SourceFileSyntax& root);
    // Collects one struct definition (or one instance's), with its role and its live flag.
    void collectStructure(
            const syntax::StructDeclSyntax::Ptr& declaration,
            std::optional<typing::TypeId> instance = std::nullopt);
    // Collects one enum definition (or one instance's), with its Optional or Result role cases.
    void collectEnumeration(
            const syntax::EnumDeclSyntax::Ptr& declaration,
            std::optional<typing::TypeId> instance = std::nullopt);
    // Collects a builtin enum, whose cases come from its member scope in symbol order.
    void collectBuiltinEnumeration(const typing::TypeRecord& type);

    // ---- functions.cpp: function declarations, generic instances, and function bodies
    // Declares every IR function and derived witness before any body is lowered (deferring the
    // contracts prelude and String roles), then links `deinit`s and evaluates the constants.
    void collectFunctions();
    // Whether `symbol` is a String role function, declared only when lowering first calls it.
    bool isStringRoleFunction(semantic::SymbolId symbol) const;
    // The IR function of a String role (decision 0028 stage 3), declared on first use.
    std::optional<ir::FunctionId> stringRoleFunction(semantic::LibraryRole role);
    // The IR function of `symbol`; a deferred prelude function is declared now, and lowered last
    // if it has a body.
    std::optional<ir::FunctionId> demandFunction(semantic::SymbolId symbol);
    // Declares one IR function per instance of a generic function.
    void collectInstances(const syntax::SourceFileSyntax& root);
    // The declaration a generic instance is lowered from.
    syntax::FunctionDeclSyntax::Ptr genericDeclaration(
            const syntax::SourceFileSyntax& root, semantic::SymbolId function) const;
    // Makes instance `index` current: types are substituted and requirement calls reach witnesses.
    void enterInstance(size_t index);
    // Leaves the current generic instance.
    void leaveInstance();
    // Records each struct's `deinit` (and a collection's copier and element addressor),
    // then rebuilds the destruction analysis.
    void linkDeinitializers(const syntax::SourceFileSyntax& root);
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
    // Declares the prelude's external `print`.
    void collectPrint();
    // Declares the prelude's external `byteToInt` and `byteToString`.
    void collectByteConversions();
    // Declares one IR function: its name, contexts, inlining, result and parameters.
    void collectFunction(const syntax::FunctionDeclSyntax::Ptr& declaration);
    // Lowers every body: top-level functions, methods, generic instances, derived witnesses, then
    // the prelude functions lowering demanded.
    void lowerFunctions();
    // Lowers one function body: spills by-value parameters to slots (an address parameter is its
    // own slot; a String role's are bound to their values), lowers the body block, returns.
    void lowerFunction(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            ir::FunctionId functionId);

    // ---- constants.cpp: top-level `let` constants
    // Whether a top-level constant may have `type`: a scalar, String, or array of those.
    bool isConstantType(typing::TypeId id) const;
    // Checks a top-level binding and records it as a constant to evaluate.
    void collectConstant(const syntax::BindingDeclSyntax::Ptr& declaration);
    // Evaluates every constant once, so an invalid one is reported even when unused.
    void evaluateConstants();
    // Lowers a use of a constant: a folded literal, or a module constant for an array.
    std::optional<ir::Value> lowerConstant(semantic::SymbolId symbol, SourceSpan span);

    // ---- debuginfo.cpp: debug scopes and debug variables
    // The debug scope of a function, block or match-arm scope, and its parents, made on first use.
    std::optional<ir::DebugScopeId> ensureDebugScope(
            semantic::ScopeId semanticScopeId,
            ir::FunctionId functionId);
    // The debug scope of the semantic scope `node` introduces, if it introduces one.
    std::optional<ir::DebugScopeId> debugScopeForNode(
            const syntax::NodePtr& node,
            ir::FunctionId functionId);
    // The debug variable of a parameter, local or pattern binding, made on first use.
    std::optional<ir::DebugVariableId> ensureDebugVariable(
            semantic::SymbolId symbolId,
            ir::FunctionId functionId,
            std::optional<uint32_t> parameterIndex = std::nullopt);

    // ---- ownership.cpp: value ownership, owned operands, and scope cleanups
    // Whether a value of `type` needs destruction (the destruction analysis decides).
    bool requiresDestroy(typing::TypeId type) const;
    // Opens a cleanup scope. Every push is matched by a cleanup-and-pop or a discard.
    void pushScope();
    // Registers storage that owns its value, destroyed when the innermost scope ends.
    void registerOwnedStorage(ir::Value address);
    // Records a new value's ownership; an owned one becomes a live temporary of the innermost scope.
    void recordValue(ir::Value value, ValueOwnership valueOwnership);
    // A value's recorded ownership; an unrecorded value that needs destruction is borrowed.
    ValueOwnership ownershipOf(ir::Value value) const;
    // Makes a block's owned result a temporary of the scope that is now innermost.
    void adoptInParentScope(ir::Value value);
    // Readies a value to be owned: a live owned temporary as is, else a copy. Reports use after
    // transfer and a copy of a value that is noncopyable (in the generic body, in an instance).
    std::optional<ir::Value> prepareOwned(ir::Value value, SourceSpan span);
    // Prepares an owned value and marks it no longer live, so no scope cleanup destroys it.
    std::optional<ir::Value> acquireOwned(ir::Value value, SourceSpan span);
    // Lowers `consume place`: moves the value out and leaves the place empty.
    std::optional<ir::Value> lowerTake(const syntax::ExprPtr& expression);
    ir::Value takeFromPlace(ir::Value address, SourceSpan span);
    // Whether a call argument is written with `consume`.
    static bool movesArgument(const syntax::CallArgumentSyntax& argument);
    // Lowers an operand, moved if `moved`, coerced to `destination` and readied to be owned.
    std::optional<ir::Value> prepareOperand(
            const syntax::ExprPtr& expression,
            typing::TypeId destination,
            bool moved = false);
    struct ConstructionOperand {
        ir::Value value;
        SourceSpan span;
        bool deferredTake;
    };
    std::optional<ConstructionOperand> prepareConstructionOperand(
            const syntax::ExprPtr& expression, typing::TypeId destination, bool marked = false);
    std::optional<ir::Value> commitConstructionOperand(
            ConstructionOperand operand, typing::TypeId destination);
    // Acquires each operand and appends it to the instruction; false if one cannot be owned.
    bool transferOperands(
            ir::Instruction& instruction,
            const std::vector<ir::Value>& operands,
            SourceSpan span);
    // Destroys the value stored at `address`, if its type needs destruction.
    void emitDestroyStorage(ir::Value address, SourceSpan span);
    // Destroys a temporary value by spilling it to a stack slot and destroying the slot.
    void emitDestroyTemporary(ir::Value value, SourceSpan span);
    // Destroys a scope's live cleanups in reverse order; `mutateState` marks them destroyed.
    void emitScopeCleanup(const ScopeFrame& scope, SourceSpan span, bool mutateState);
    // Emits the innermost scope's cleanups and closes it.
    void cleanupAndPopScope(SourceSpan span);
    // Closes the innermost scope without cleanups, after a terminator left it.
    void discardScopeAfterTerminator();
    // Emits every open scope's cleanups, innermost first, before a return.
    void emitAllScopeCleanupForReturn(SourceSpan span);

    // ---- statements.cpp: blocks, bindings, `if`, loops, and `return`
    // Lowers a block expression in its own scope; its value is its trailing expression.
    std::optional<ir::Value> lowerBlock(const syntax::BlockExprSyntax::Ptr& block);
    // Lowers a local `let` or `var` into a stack slot.
    void lowerBinding(const syntax::BindingDeclSyntax::Ptr& declaration);
    // Lowers `if` to then, else and merge blocks.
    std::optional<ir::Value> lowerIf(const syntax::IfExprSyntax::Ptr& expression);
    // Lowers `break` or `continue`, destroying the loop body's live cleanups first.
    void lowerLoopControl(const syntax::NodePtr& statement);
    // Lowers `while` to header, body and exit blocks.
    void lowerWhile(const syntax::WhileStmtSyntax::Ptr& statement);
    // Lowers `return`, with or without a value.
    void lowerReturn(const syntax::ReturnExprSyntax::Ptr& expression);
    // Lowers a returned value; a noncopyable place rooted in a local is moved out, not copied.
    std::optional<ir::Value> lowerReturnedValue(const syntax::ExprPtr& expression);
    // Emits the return of `value` after every open scope's cleanups.
    void emitReturnValue(ir::Value value, SourceSpan span, SourceSpan valueSpan);
    // Whether a syntax kind is an expression rather than a declaration or statement.
    bool isExpressionKind(syntax::Kind kind) const;

    // ---- expressions.cpp: the expression dispatcher, operators, names and members
    // Lowers any expression; dispatches on its syntax kind.
    std::optional<ir::Value> lowerExpression(const syntax::ExprPtr& expression);
    // Lowers a literal to a constant, or `nil` to Optional's absent case.
    std::optional<ir::Value> lowerLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression);
    // Lowers a function named as a value (spec 5.6.1) to its address.
    std::optional<ir::Value> lowerFunctionAddress(semantic::SymbolId symbol, const syntax::NodePtr& expression);
    // Whether `expression` names a function used as a value rather than called.
    bool namesFunctionValue(const syntax::NodePtr& expression, std::optional<semantic::SymbolId> symbol) const;
    // Lowers a name: a function value, a constant, a bound parameter, or a load from a local slot.
    std::optional<ir::Value> lowerName(const syntax::NodePtr& expression);
    // Lowers a prefix operator or a conversion (`as`, `&as`, `as?`).
    std::optional<ir::Value> lowerPrefix(const syntax::PrefixExprSyntax::Ptr& expression);
    // Lowers a binary operator: `&&` and `||`, presence tests against `nil`, and `==`/`!=` on a
    // non-builtin conformer go to their own lowering; the rest to `lowerBuiltinBinary`.
    std::optional<ir::Value> lowerBinary(const syntax::BinaryExprSyntax::Ptr& expression);
    // Lowers a binary operator on builtin operands, String operators included (spec 20.13.2).
    std::optional<ir::Value> lowerBuiltinBinary(
            TokenKind op, const syntax::ExprPtr& leftOperand, const syntax::ExprPtr& rightOperand,
            typing::TypeId type, SourceSpan span);
    // Whether `type` is the builtin String.
    bool isString(typing::TypeId type) const;
    // Calls the String role function for `role` with borrowed operands; a result that needs
    // destruction is owned.
    std::optional<ir::Value> emitStringRole(
            semantic::LibraryRole role, const std::vector<ir::Value>& operands, typing::TypeId type,
            SourceSpan span);
    // Lowers `+`, `==`, `!=` or an ordering on two Strings through the String role functions.
    std::optional<ir::Value> emitStringOperator(
            TokenKind token, ir::Value left, ir::Value right, typing::TypeId type, SourceSpan span);
    // Lowers `x as? T` to a range test and an Ok or Err result.
    std::optional<ir::Value> lowerTryConversion(
            const syntax::PrefixExprSyntax::Ptr& expression,
            ir::Value operand,
            typing::TypeId resultType);
    // Lowers `&&` and `||` with short-circuit blocks.
    std::optional<ir::Value> lowerLogical(
            const syntax::BinaryExprSyntax::Ptr& expression);
    // The IR operation (opcode and modifier) of a binary operator token, if it has one.
    std::optional<Operation> binaryOpcode(TokenKind token) const;
    // Lowers a member read: a field, `count`, an enum case, a function value or a constant.
    std::optional<ir::Value> lowerMember(const syntax::MemberExprSyntax::Ptr& expression);

    // ---- equality.cpp: `Equatable` equality, presence tests, and derived witnesses
    // Declares one derived `Equatable` witness per concrete type the program needs (0031).
    void collectDerivedEqualities();
    // Lowers a derived witness: fields, or tags then payloads, compared in declaration order.
    void lowerDerivedEquality(typing::TypeId type, ir::FunctionId functionId);
    // Returns `value` from a derived witness, which has no scope cleanups to emit.
    void emitDerivedReturn(ir::Value value, SourceSpan span);
    // Compares two borrowed member values: a builtin operation or the member type's witness.
    std::optional<ir::Value> emitMemberEquality(ir::Value left, ir::Value right, typing::TypeId type, SourceSpan span);
    // The function `==` calls for a non-builtin conformer: a derived, `array_equal` or written witness.
    std::optional<ir::FunctionId> equalityCallee(typing::TypeId type);
    // Lowers `x == nil` or `x != nil` to a tag test of the borrowed optional (spec 20.13.4).
    std::optional<ir::Value> lowerPresenceTest(const syntax::ExprPtr& operand, bool equal, SourceSpan span);
    // Lowers `==` or `!=` on a non-builtin conformer (derived, array or hand-written) to a call
    // of the witness `equalityCallee` picks, with both operands borrowed.
    std::optional<ir::Value> lowerWitnessEquality(
            const syntax::ExprPtr& leftOperand, const syntax::ExprPtr& rightOperand, typing::TypeId operandType,
            bool equal, typing::TypeId type, SourceSpan span);
    // Lowers `t.equals(other:)` on a builtin, derived or array conformer; nothing for
    // other calls.
    std::optional<std::optional<ir::Value>> lowerBuiltinEqualsRequirement(
            const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol* targetSymbol);

    // ---- places.cpp: assignment and the addresses of names, fields and elements
    // Lowers `target = value`; a dictionary element target goes to `lowerDictionarySet`.
    std::optional<ir::Value> lowerAssignment(
            const syntax::AssignmentExprSyntax::Ptr& expression);
    // Lowers `target op= value`.
    std::optional<ir::Value> lowerCompoundAssignment(
            const syntax::AssignmentExprSyntax::Ptr& expression);
    // Whether evaluating an expression provably cannot change storage read before it.
    static bool cannotDisturb(const syntax::ExprPtr& expression);
    // Whether no index operand along an assignment target's path can disturb.
    static bool targetIndexesCannotDisturb(const syntax::ExprPtr& target);
    // The address of a place: a local, a field or a collection element.
    std::optional<ir::Value> lowerAddress(const syntax::ExprPtr& expression);
    // Evaluates a place's index operands now and returns how to compute its address later.
    std::optional<DeferredAddress> prepareAddress(const syntax::ExprPtr& expression, bool ownKeys = false);
    // Whether an expression names a place rooted in a local slot.
    bool isAddressable(const syntax::ExprPtr& expression) const;

    // ---- collections.cpp: array and dictionary literals, subscripts and element addresses
    // Lowers `&d[k] = v` on a library dictionary to a `dict_set` call.
    std::optional<ir::Value> lowerDictionarySet(
            const syntax::AssignmentExprSyntax::Ptr& expression,
            const syntax::SubscriptExprSyntax::Ptr& subscript,
            typing::TypeId dictionaryType);
    // Whether `type` is subscripted through a role addressor: a library array or dictionary.
    bool isRoleSubscriptBase(typing::TypeId type) const;
    // Calls the role addressor and turns the returned `Int` into the element's typed address.
    std::optional<ir::Value> emitRoleElementAddress(
            typing::TypeId arrayType, const ir::Value& baseAddress, const ir::Value& index,
            typing::TypeId elementType, SourceSpan span);
    // Lowers an element read: a collection element through its addressor, or a String byte.
    std::optional<ir::Value> lowerSubscript(
            const syntax::SubscriptExprSyntax::Ptr& expression);
    // Lowers an array literal through the array role's literal method.
    std::optional<ir::Value> lowerArray(const syntax::ArrayExprSyntax::Ptr& expression);
    // Lowers a dictionary literal through the dictionary role's literal method.
    std::optional<ir::Value> lowerDictionary(
            const syntax::DictionaryExprSyntax::Ptr& expression);

    // ---- enums.cpp: enum case lookup, construction, payloads, and `?` propagation
    // Lowers `.Case(...)`, whose enum comes from the context.
    std::optional<ir::Value> lowerContextualCase(
            const syntax::ContextualCaseExprSyntax::Ptr& expression);
    // The case an Optional or Result role binds (decision 0024); never found by spelling.
    std::optional<semantic::SymbolId> findRoleCase(
            typing::TypeId type,
            ir::EnumerationRole role,
            bool second) const;
    // Finds an enum case by name, for cases the language names, such as `OutOfRange`.
    std::optional<semantic::SymbolId> findEnumCase(
            typing::TypeId type,
            const std::string& name) const;
    // The collected definition of a case of the enum `type`.
    const ir::EnumCaseDefinition* enumCaseDefinition(
            typing::TypeId type,
            semantic::SymbolId caseSymbol) const;
    // Constructs an enum value from payloads that are already lowered; they are moved in.
    std::optional<ir::Value> emitEnumConstruction(
            typing::TypeId type,
            semantic::SymbolId caseSymbol,
            const std::vector<ir::Value>& payloads,
            SourceSpan span);
    // Lowers a case's payload arguments, then constructs the value.
    std::optional<ir::Value> lowerEnumArguments(
            typing::TypeId type,
            semantic::SymbolId caseSymbol,
            const std::vector<syntax::CallArgumentSyntax::Ptr>& arguments,
            SourceSpan span);
    // Extracts one payload of a known case from an enum value.
    ir::Value emitExtractPayload(
            typing::TypeId payloadType,
            ir::ValueId operandId,
            semantic::SymbolId caseSymbol,
            int64_t payloadIndex,
            SourceSpan span);
    // Lowers `x?`: the success payload continues; the failure returns from the function.
    std::optional<ir::Value> lowerPropagation(
            const syntax::PropagateExprSyntax::Ptr& expression);

    // ---- patterns.cpp: `match`, patterns, and pattern bindings
    // Noncopyable bindings must move; bindings needing destruction can also move from a temporary.
    bool patternBindsMovablePayload(const syntax::PatternPtr& pattern, bool moveCopyable) const;
    // Lowers `match` to a pattern switch, one block per arm up to the first that matches every
    // value, and a merge block.
    std::optional<ir::Value> lowerMatch(const syntax::MatchExprSyntax::Ptr& expression);
    // Whether a top-level arm pattern matches every value, so no later arm can run.
    static bool catchesEveryValue(const syntax::PatternPtr& pattern);
    // Lowers a syntax pattern to the IR pattern the switch tests.
    std::optional<ir::Pattern> lowerPattern(const syntax::PatternPtr& pattern);
    // Stores an owned value into a new slot for a pattern binding, which owns it to scope end.
    void storeBinding(
            const syntax::PatternPtr& pattern,
            semantic::SymbolId symbol,
            ir::Value owned);
    // Takes selected payloads out of the enum stored at `address`.
    void bindMovedPayloads(const syntax::PatternPtr& pattern, ir::Value address, bool moveCopyable);
    // Binds a matched pattern's names, copying payloads, or moving them when `address` is given.
    void bindPattern(
            const syntax::PatternPtr& pattern,
            ir::Value value,
            std::optional<ir::Value> address = std::nullopt,
            bool moveCopyable = false);

    // ---- calls.cpp: direct and indirect calls, arguments, and struct construction
    // Lowers a call: special targets first, then the receiver and arguments, then the call.
    std::optional<ir::Value> lowerCall(const syntax::CallExprSyntax::Ptr& expression);
    // The IR function of `requirement`'s witness for `base`: an instance for a generic witness.
    std::optional<ir::FunctionId> witnessFunction(typing::TypeId base, semantic::SymbolId requirement);
    // The IR function a direct call reaches: the target, a generic instance or a witness.
    std::optional<ir::FunctionId> calledFunction(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId target,
            const semantic::Symbol* targetSymbol);
    // Lowers one argument by its parameter's convention: address, moved value or value.
    std::optional<ir::Value> lowerCallOperand(
            const syntax::ExprPtr& operand,
            SourceSpan span,
            bool expectsAddress,
            bool consumesValue,
            std::optional<typing::TypeId> parameterType,
            std::vector<ir::Value>& consumedArguments,
            bool movesSource = true);
    // Lowers a call through a function value (spec 5.6.1) to `callIndirect`.
    std::optional<ir::Value> lowerFunctionValueCall(
            const syntax::CallExprSyntax::Ptr& expression,
            typing::TypeId calleeType);
    // Emits a lowered call: acquires consumed arguments, then gives the result a value.
    std::optional<ir::Value> finishCall(
            const syntax::CallExprSyntax::Ptr& expression,
            ir::Instruction instruction,
            std::vector<ir::ValueId> arguments,
            const std::vector<ir::Value>& consumedArguments);
    // Lowers a struct initializer call: labeled arguments, field defaults, the live flag.
    std::optional<ir::Value> lowerStructConstruction(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId initializer);

    // ---- intrinsics.cpp: layout intrinsics and `String.utf8`
    // Lowers one layout intrinsic (spec 20.12.5) for the current instance's type.
    std::optional<ir::Value> lowerLayoutIntrinsic(
            const syntax::CallExprSyntax::Ptr& expression, const std::string& name);
    // Lowers `s.utf8()` to a call of the library array's `string_utf8` method; the String is borrowed.
    std::optional<ir::Value> lowerStringUtf8(
            const syntax::CallExprSyntax::Ptr& expression);

    // ---- emission.cpp: instructions, values, blocks, branches and conversions
    // Stores a value with no conversion or ownership transfer; the caller handles both.
    void emitRawStore(
            ir::Value value,
            ir::Value address,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt,
            bool implicitCode = false,
            std::optional<ir::DebugVariableId> debugVariable = std::nullopt);
    // A Boolean constant.
    ir::Value emitBoolLiteral(bool value, SourceSpan span);
    // The unit value.
    ir::Value emitUnit(SourceSpan span);
    // Emits an instruction with a result and records the result's ownership.
    ir::Value emitValue(
            Operation operation,
            typing::TypeId type,
            ir::ValueCategory category,
            std::vector<ir::ValueId> operands,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt,
            bool implicitCode = false,
            std::optional<ir::DebugVariableId> debugVariable = std::nullopt);
    // Stores a value into storage, coercing it and acquiring it first.
    void emitStore(
            ir::Value value,
            ir::Value address,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt);
    // Converts a value to `destination`: unchanged if it matches or is `Any`, else into Optional's present case.
    std::optional<ir::Value> coerce(
            ir::Value value,
            typing::TypeId destination,
            SourceSpan span);
    // Appends a new basic block to the current function.
    ir::BlockId createBlock(std::string name);
    // Makes `block` the block new instructions go to.
    void switchToBlock(ir::BlockId block);
    // Emits an unconditional branch.
    void emitBranch(ir::BlockId target, SourceSpan span);
    // Emits a two-way branch on a Boolean.
    void emitConditionalBranch(
            ir::Value condition,
            ir::BlockId trueBlock,
            ir::BlockId falseBlock,
            SourceSpan span);
    // A new instruction for `operation` at `span`, with a debug location if the module has source tables.
    ir::Instruction makeInstruction(
            Operation operation,
            SourceSpan span,
            bool implicitCode = false) const;
    // A new value id of the current function.
    ir::Value makeValue(typing::TypeId type, ir::ValueCategory category);
    // Appends an instruction to the current block.
    void emit(ir::Instruction instruction);
    // Whether the current block already ends in a terminator.
    bool currentBlockTerminated() const;
    // The function being lowered.
    ir::Function& currentFunction();
    // The function being lowered.
    const ir::Function& currentFunction() const;
    // The block new instructions go to.
    ir::BasicBlock& currentBlock();
    // The block new instructions go to.
    const ir::BasicBlock& currentBlock() const;
};

} // namespace joyeer::lowering::detail

#endif
