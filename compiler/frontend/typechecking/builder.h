#ifndef JOYEER_FRONTEND_TYPECHECKING_BUILDER_H
#define JOYEER_FRONTEND_TYPECHECKING_BUILDER_H

#include "compiler/frontend/typechecking.h"

namespace joyeer::typing {

// One instance per check. Only this builder writes the typed overlay; later stages query
// TypeCheckedModel rather than retaining the traversal state below.
class TypeCheckingBuilder {
public:
    TypeCheckingResult build(const semantic::SemanticModel::Ptr& semanticModel);

private:
    // The marker written before a method call: `&receiver.method()` or
    // `consume receiver.method()`.
    enum class ReceiverMarker {
        none,
        inout,
        consume,
    };

    TypeCheckedModel::Ptr model;
    std::vector<TypeCheckingDiagnostic> diagnostics;
    std::optional<TypeId> currentReturnType;
    size_t loopDepth = 0;
    std::unordered_set<semantic::NodeId> handledDeferredReferences;
    // Enum case payload types, checked for linear dictionaries and channel elements after resolution.
    std::vector<std::pair<TypeId, SourceSpan>> enumPayloadChecks;
    // `consume` access expressions that appear where a value is stored (see permitMove).
    std::unordered_set<const syntax::Node*> permittedMoves;
    bool insideDeinit = false;
    // Calls that are direct arguments of another call, where `place_address` may appear.
    std::unordered_set<const syntax::Node*> permittedPlaceAddress;
    // Type parameters with constraints, checked for conflicting requirements once all are known.
    std::vector<semantic::SymbolId> constrainedParameters;
    // A call of a protocol requirement on a type parameter, inside a generic function or method. When
    // an instance fixes the parameter to an instance of a generic type, the witness is itself an
    // instance of that type's method (spec 20.12), found while instantiating.
    struct RequirementCall {
        semantic::SymbolId caller;
        semantic::SymbolId requirement;
        TypeId base;
        SourceSpan span;
    };
    std::vector<RequirementCall> requirementCalls;
    // `==` on a concrete conformer other than a builtin: the witness it needs, found with the instances.
    struct EqualityRoot {
        TypeId type;
        std::optional<semantic::SymbolId> caller;
        SourceSpan span;
    };
    std::vector<EqualityRoot> equalityRoots;
    struct DerivedConformanceCheck {
        TypeId subject;
        SourceSpan span;
        size_t conformance;
    };
    std::vector<DerivedConformanceCheck> derivedConformanceChecks;
    // Answers of `satisfiesEquatable` for derived conformers, by `assumeParameters` (see there).
    mutable bool equatableCacheEnabled = false;
    mutable std::unordered_set<TypeId> equatableTrue[2];
    mutable std::unordered_set<TypeId> equatableFalse[2];
    mutable std::unordered_set<TypeId> equatableProvisional[2];
    // While the methods of a struct, enum or extension are resolved: the type they belong to and the
    // type arguments `Self` stands for, which are the parameters in scope (spec 20.12).
    std::optional<semantic::SymbolId> selfOwner;
    std::vector<TypeId> selfArguments;
    // The parameters in scope for a member, for substituting a receiver's arguments into its signature:
    // the declaration's own, or those of the extension that declares the method (spec 20.12.2).
    std::vector<semantic::SymbolId> selfParameters;
    std::unordered_map<semantic::SymbolId, std::vector<semantic::SymbolId>> methodParameters;
    std::optional<semantic::SymbolId> currentFunction;

    // The string-literal argument of joyeer_intrinsic_literal_address, which is
    // read as constant data and never becomes a String value, is the one managed
    // value a core unit may write.
    const syntax::Node* literalAddressArgument = nullptr;
    // The callee of the call being checked: a function named there is called, not a function value.
    const syntax::Node* currentCallee = nullptr;
    // The base of the member expression being checked: a type named there is not a value.
    const syntax::Node* currentMemberBase = nullptr;
    // Set while resolving the parameter of the literal-address intrinsic itself.
    bool coreCheckSuspended = false;

    // What must match between a requirement and its witness (spec 20.3), with `Self` replaced.
    struct MethodShape {
        syntax::AccessEffect receiver = syntax::AccessEffect::borrowing;
        std::vector<std::optional<std::string>> labels;
        std::vector<syntax::AccessEffect> effects;
        std::vector<TypeId> parameters;
        TypeId result = invalidTypeId;
    };

    enum class ArgumentInformation { typed, literal, none };

    struct Inference {
        const std::vector<semantic::SymbolId>& parameters;
        std::unordered_map<semantic::SymbolId, TypeId> bindings;
        std::unordered_set<semantic::SymbolId> conflicted;
        // Parameters bound by the expected type of a construction, not by an argument.
        std::unordered_set<semantic::SymbolId> fromExpected;
    };

    // The explicit type arguments of a generic type written in a type position (spec 20.12) are
    // checked once every declaration's constraints are known.
    struct PendingTypeArguments {
        SourceSpan span;
        semantic::SymbolId type;
        std::vector<TypeId> arguments;
    };
    std::vector<PendingTypeArguments> pendingTypeArguments;

    // Annotations inside bodies add to the list while the bodies are checked, so this runs again
    // afterwards over the entries it has not seen.
    size_t checkedPendingTypeArguments = 0;

    // Decision 0021: the instances reachable from non-generic code, each once, with a cap as a
    // backstop for programs the expanding-recursion rule allows but that are still huge.
    static constexpr size_t maximumInstances = 10000;
    static constexpr size_t maximumTypeNesting = 64;
    static constexpr size_t maximumInstanceDepth = 64;
    static constexpr size_t maximumTypeOccurrences = 4096;
    static constexpr size_t maximumTypeTableSize = 200000;

    struct AccessPath {
        semantic::SymbolId root = semantic::invalidSymbolId;
        // A field has a symbol; a subscript has no statically distinguished element.
        std::vector<std::optional<semantic::SymbolId>> projections;
    };

    struct CallAccess {
        AccessPath path;
        syntax::AccessEffect effect = syntax::AccessEffect::borrowing;
        SourceSpan span;
        // Sustained accesses remain live while later arguments are evaluated.
        bool sustained = true;
    };

    struct StorageAccess {
        bool writable = true;
        bool requiresMarker = false;
        std::string immutableName;
        // The place is the `count` of a library array or dictionary, read-only outside the collection's own file.
        bool libraryReadOnly = false;
        bool dictionaryCount = false;
        // The place is a byte of a String, which `s[i]` only reads (spec 2.1.1).
        bool stringByte = false;
    };

    std::unordered_set<const syntax::ExprSyntax*> reportedStringByteConsumes;

    struct PatternCoverage {
        bool catchesAll = false;
        std::optional<semantic::SymbolId> enumCase;
        std::optional<bool> booleanLiteral;
        std::vector<PatternCoverage> payloads;
    };

    using PatternRow = std::vector<const PatternCoverage*>;

    // typechecking.cpp
    void report(
            TypeCheckingDiagnosticId id,
            SourceSpan span,
            std::string message,
            std::optional<std::string> help = std::nullopt,
            std::optional<DiagnosticFixIt> fixIt = std::nullopt,
            std::vector<DiagnosticSourceNote> notes = {});
    void recordNodeType(const syntax::NodePtr& node, TypeId type);
    void recordDeclaredType(const syntax::NodePtr& node, TypeId type);
    void recordResolvedReference(
            const syntax::NodePtr& node,
            semantic::SymbolId symbol);
    void recordResolvedCallTarget(
            const syntax::NodePtr& node,
            semantic::SymbolId symbol);
    void markDeferredReferenceHandled(const syntax::NodePtr& node);
    void validateDeferredReferences();

    // prelude.cpp
    [[nodiscard]] bool dictMethodRoleMatches(const semantic::Symbol& symbol, const TypedCallableSignature& signature,
                                             const semantic::Symbol& owner, std::string& failure);
    void validateEquatableRole();
    void validatePreludeContracts();
    void resolveBuiltinSignatures();

    // declarations.cpp
    std::optional<TypeId> resolveType(const syntax::TypePtr& syntaxType);
    void resolveTopLevelDeclaration(const syntax::NodePtr& node);
    const semantic::Symbol* symbolOf(semantic::SymbolId id) const;
    std::string symbolName(semantic::SymbolId id) const;
    bool checkProtocolReference(const syntax::TypePtr& reference);
    std::vector<semantic::SymbolId> recordTypeParameters(
            const std::vector<syntax::GenericParameterSyntax::Ptr>& syntaxParameters);
    void resolveGenericParameters(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            std::optional<TypeCheckingDiagnosticId> misplaced);
    static DiagnosticSourceNote sourceNote(SourceSpan span, std::string message);
    static SourceSpan coveringSpan(SourceSpan first, SourceSpan last);
    void rejectEquatableName(const Token::Ptr& name);
    void resolveBinding(const syntax::BindingDeclSyntax::Ptr& declaration);
    void resolveMethod(const syntax::FunctionDeclSyntax::Ptr& declaration);
    void resolveFunction(const syntax::FunctionDeclSyntax::Ptr& declaration);
    void enterExtension(const syntax::ExtensionDeclSyntax::Ptr& declaration);
    void checkExtensionHeader(
            const syntax::ExtensionDeclSyntax::Ptr& declaration,
            const semantic::Symbol& owner,
            const std::vector<semantic::SymbolId>& declared);
    std::vector<TypeId> enterGenericType(
            semantic::SymbolId type, const std::vector<syntax::GenericParameterSyntax::Ptr>& declared);
    void resolveStructure(const syntax::StructDeclSyntax::Ptr& declaration);
    void resolveEnumeration(const syntax::EnumDeclSyntax::Ptr& declaration);

    // protocols.cpp
    void resolveProtocol(const syntax::ProtocolDeclSyntax::Ptr& declaration);
    std::optional<MethodShape> methodShape(
            semantic::SymbolId method,
            const std::unordered_map<semantic::SymbolId, TypeId>& substitution);
    static const char* effectWord(syntax::AccessEffect effect);
    std::optional<std::string> shapeMismatch(const MethodShape& requirement, const MethodShape& witness) const;
    std::vector<semantic::SymbolId> requirementsOf(semantic::SymbolId protocol) const;
    std::string requirementTitle(semantic::SymbolId requirement) const;
    void checkConstraintConflicts();
    void checkConformances(const syntax::ExtensionDeclSyntax::Ptr& extension);
    bool satisfiesEquatable(TypeId type, std::vector<TypeId>& inProgress, bool assumeParameters) const;
    void clearEquatableCache() const;
    std::optional<std::pair<std::string, TypeId>> nonEquatablePath(TypeId type, size_t depth = 0) const;
    void validateDerivedConformances();
    bool satisfies(TypeId type, semantic::SymbolId protocol) const;

    // inference.cpp
    bool isOpenCaseReference(const syntax::ExprPtr& value) const;
    ArgumentInformation argumentInformation(const syntax::ExprPtr& value) const;
    bool mentionsAny(TypeId type, const std::vector<semantic::SymbolId>& parameters) const;
    void unify(TypeId parameter, TypeId argument, Inference& inference, SourceSpan span);
    void bindFromExpectedType(
            const semantic::Symbol& targetSymbol,
            const std::vector<semantic::SymbolId>& parameters,
            std::optional<TypeId> expectedResult,
            Inference& inference);
    TypedCallableSignature inferGenericCall(
            const syntax::CallExprSyntax::Ptr& expression,
            semantic::SymbolId target,
            const semantic::Symbol& targetSymbol,
            const TypedCallableSignature& signature,
            const std::vector<semantic::SymbolId>& parameters,
            std::vector<std::optional<std::optional<TypeId>>>& checked,
            std::optional<TypeId> expectedResult = std::nullopt);
    void checkTypeArguments(
            const syntax::CallExprSyntax::Ptr& expression,
            const semantic::Symbol& targetSymbol,
            const std::vector<semantic::SymbolId>& parameters,
            const std::vector<TypeId>& arguments);
    void checkPendingTypeArguments();
    void checkTypeArguments(
            SourceSpan span,
            const semantic::Symbol& targetSymbol,
            const std::vector<semantic::SymbolId>& parameters,
            const std::vector<TypeId>& arguments);

    // instances.cpp
    void checkExpandingRecursion(bool typesOnly = false);
    bool validateChannelReceiverOwnership(
            TypeId element, SourceSpan span, std::optional<size_t> requiredBy);
    void instantiate();
    void internInstanceTypes();
    void internInstanceMemberTypes();
    std::vector<DiagnosticSourceNote> instantiationChain(std::optional<size_t> instance) const;

    // native.cpp
    [[nodiscard]] bool allowedInCoreUnit(TypeId type) const;
    [[nodiscard]] static bool declaresLiteralAddress(const syntax::FunctionDeclSyntax& declaration);
    void requireCoreType(const syntax::NodePtr& node, TypeId type);
    void resolveNativeBinding(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            semantic::SymbolId function);
    void validateNativeSymbols();
    TypeId checkCallbackAddress(const syntax::CallExprSyntax::Ptr& expression);
    TypeId checkLayoutIntrinsic(const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol& target);

    // statements.cpp
    bool requireAssignable(TypeId source, TypeId destination, SourceSpan span);
    std::optional<TypeId> declaredAnnotationType(
            const syntax::BindingDeclSyntax::Ptr& declaration);
    void checkBinding(const syntax::BindingDeclSyntax::Ptr& declaration);
    static bool isForInSourceBinding(const syntax::BindingDeclSyntax::Ptr& declaration);
    void checkLinearStructure(const syntax::StructDeclSyntax::Ptr& structure);
    void requireNoLinearDictionary(TypeId id, SourceSpan span);
    void checkTopLevelBody(const syntax::NodePtr& node);
    void checkFunctionBody(const syntax::FunctionDeclSyntax::Ptr& function);
    TypeId checkBlock(const syntax::BlockExprSyntax::Ptr& block,
            std::optional<TypeId> expected = std::nullopt,
            bool discarded = false);
    TypeId checkNode(const syntax::NodePtr& node,
            std::optional<TypeId> expected = std::nullopt);
    bool isExpressionKind(syntax::Kind kind) const;

    // expressions.cpp
    std::optional<TypeId> checkExpression(
            const syntax::ExprPtr& expression,
            std::optional<TypeId> expected = std::nullopt,
            ReceiverMarker receiverMarker = ReceiverMarker::none);
    TypeId checkNumericLiteral(const Token::Ptr& literal, std::optional<TypeId> expected,
            bool negative = false);
    [[nodiscard]] std::optional<std::string> roleShadowHelp(
            std::optional<TypeId> type, std::optional<semantic::LibraryRole> only = std::nullopt) const;
    TypeId checkLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    TypeId checkPropagation(
            const syntax::PropagateExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    TypeId checkCast(const syntax::PrefixExprSyntax::Ptr& expression);
    TypeId checkPrefix(const syntax::PrefixExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    bool isNumericLiteral(const syntax::ExprPtr& expression) const;
    bool isEquatable(TypeId type) const;
    std::optional<TypeId> binaryResult(TokenKind op, TypeId left, TypeId right);
    TypeId checkBinary(const syntax::BinaryExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    bool isContextualNone(const syntax::ExprPtr& expression) const;
    bool isAbsentLiteral(const syntax::ExprPtr& expression) const;
    int operandTypeStrength(const syntax::ExprPtr& expression) const;
    bool isFloatLiteral(const syntax::ExprPtr& expression) const;
    void requireEqualityWitness(const syntax::BinaryExprSyntax::Ptr& expression, TypeId operand);
    void validateEqualityExclusivity(const syntax::BinaryExprSyntax::Ptr& expression);
    void reportInvalidOperator(
            const Token::Ptr& op,
            const std::vector<TypeId>& operands,
            SourceSpan span);
    std::optional<std::string> equatableHelp(TypeId type, SourceSpan span) const;
    TypeId commonType(TypeId left, TypeId right, SourceSpan mismatchSpan);

    // members.cpp
    bool rejectTypeAsValue(const syntax::ExprPtr& expression);
    TypeId checkName(const syntax::NameExprSyntax::Ptr& expression);
    std::optional<TypeId> functionValueType(const syntax::ExprPtr& expression);
    bool isFunctionType(TypeId type) const;
    bool isOptionalFunction(TypeId type) const;
    TypeId referencedValueType(const syntax::NodePtr& expression);
    static bool isEnumCase(const semantic::Symbol* symbol);
    const semantic::Symbol* referencedEnumCase(const syntax::NodePtr& expression) const;
    TypeId checkMember(
            const syntax::MemberExprSyntax::Ptr& expression,
            std::optional<TypeId> expectedResult = std::nullopt);
    std::unordered_map<semantic::SymbolId, TypeId> baseSubstitution(
            TypeId base, const std::vector<semantic::SymbolId>* inScope = nullptr) const;
    const std::vector<semantic::SymbolId>* parametersOfMember(semantic::SymbolId member) const;
    TypeId substituteForBase(TypeId type, TypeId base, semantic::SymbolId member) const;
    std::optional<semantic::SymbolId> lookupMember(
            TypeId base,
            const std::string& name) const;
    bool requireAccessible(semantic::SymbolId target, SourceSpan use);

    // calls.cpp
    TypeId checkFunctionValueCall(
            const syntax::CallExprSyntax::Ptr& expression,
            TypeId calleeType,
            ReceiverMarker receiverMarker);
    bool isMemberCall(const syntax::ExprPtr& expression) const;
    std::optional<syntax::AccessEffect> receiverEffectOf(const semantic::Symbol* target) const;
    TypeId checkCall(
            const syntax::CallExprSyntax::Ptr& expression,
            ReceiverMarker receiverMarker = ReceiverMarker::none,
            std::optional<TypeId> expectedResult = std::nullopt);
    void recordProcessLaunchUse(const semantic::Symbol* symbol, SourceSpan span);
    TypeId checkChannel(const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol& target);
    bool validateTaskTarget(const syntax::CallExprSyntax::Ptr& expression);
    std::optional<size_t> parameterIndexForArgument(
            const semantic::Symbol& target,
            const syntax::CallArgumentSyntax& argument,
            size_t positionalIndex) const;

    // exclusivity.cpp
    void collectCallReceiverAccess(
            const syntax::CallExprSyntax::Ptr& expression,
            const semantic::Symbol* target,
            std::vector<CallAccess>& accesses,
            bool sustained = true) const;
    void collectCallArgumentAccess(
            const syntax::CallArgumentSyntax& argument,
            syntax::AccessEffect effect,
            std::vector<CallAccess>& accesses,
            bool sustained = true) const;
    std::optional<AccessPath> storagePath(const syntax::ExprPtr& expression) const;
    void collectSubscriptIndexAccesses(
            const syntax::ExprPtr& expression,
            std::vector<CallAccess>& accesses) const;
    void collectEvaluationAccesses(
            const syntax::NodePtr& expression,
            std::vector<CallAccess>& accesses) const;
    void collectEnumEvaluationAccesses(
            const std::vector<syntax::CallArgumentSyntax::Ptr>& arguments,
            std::vector<CallAccess>& accesses) const;
    bool pathsOverlap(const AccessPath& left, const AccessPath& right) const;
    const char* accessEffectName(syntax::AccessEffect effect) const;
    void validateCallExclusivity(const std::vector<CallAccess>& accesses);

    // storage.cpp
    static std::optional<std::string> readOnlyHelp(const StorageAccess& storage);
    bool isStringByte(const syntax::ExprPtr& expression) const;
    bool rejectConsumedStringByte(const syntax::ExprPtr& operand);
    static bool isSubscript(const syntax::ExprPtr& expression);
    StorageAccess analyzeStorage(const syntax::ExprPtr& expression) const;
    TypeId checkCompoundAssignment(const syntax::AssignmentExprSyntax::Ptr& assignment,
            std::optional<TypeId> target);
    void checkAssignmentAccess(const syntax::ExprPtr& target);
    void checkCallArgumentAccess(
            const semantic::Symbol& target,
            size_t parameterIndex,
            const syntax::CallArgumentSyntax& argument);
    void checkArgumentAccess(syntax::AccessEffect required, const syntax::CallArgumentSyntax& argument);
    std::optional<DiagnosticFixIt> accessMarkerFix(
            syntax::AccessEffect required,
            const syntax::CallArgumentSyntax& argument) const;
    std::string accessMarkerHelp(
            syntax::AccessEffect required,
            syntax::AccessEffect supplied) const;
    static bool storesItsArguments(const semantic::Symbol& target);
    bool isPlaceExpression(const syntax::ExprPtr& expression) const;
    bool assignsWholeSelf(const syntax::ExprPtr& target) const;
    static bool consumesArgument(const syntax::CallArgumentSyntax& argument);
    void permitMove(const syntax::ExprPtr& expression);
    void requireStoredArgument(
            const syntax::CallArgumentSyntax& argument,
            std::optional<TypeId> type);
    void markValueDiscarded(const syntax::NodePtr& node);
    bool producesBranchValue(TypeId type) const;
    void collectCopiedPlaces(const syntax::ExprPtr& expression,
                             std::vector<syntax::ExprPtr>& places) const;
    void requireNoImplicitCopy(
            const syntax::ExprPtr& expression,
            std::optional<TypeId> type,
            bool movesOwningName = false);
    bool isConsumable(const syntax::ExprPtr& expression) const;
    bool isInitializable(const syntax::ExprPtr& expression) const;
    bool isNamedLocalPlace(const syntax::ExprPtr& expression) const;
    void requireExactPlaceType(std::optional<TypeId> actual, TypeId type, const std::string& name, SourceSpan span);

    // patterns.cpp
    TypeId checkContextualCase(
            const syntax::ContextualCaseExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    std::optional<TypedCallableSignature> enumCaseSignature(
            semantic::SymbolId caseSymbol,
            TypeId enumerationType) const;
    void validateContextualCaseArguments(
            const syntax::ContextualCaseExprSyntax& expression,
            semantic::SymbolId caseSymbol,
            const TypedCallableSignature& signature);
    void validateEnumArgumentLabel(
            const std::optional<std::string>& expected,
            const Token::Ptr& actual,
            SourceSpan span,
            const std::string& caseName);
    TypeId checkMatch(
            const syntax::MatchExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    bool patternBindsNoncopyable(const syntax::PatternPtr& pattern) const;
    void requireOwnedScrutineeForMovedBindings(const syntax::MatchExprSyntax::Ptr& expression);
    PatternCoverage checkPattern(
            const syntax::PatternPtr& pattern,
            TypeId expected);
    void checkLiteralPatternType(const Token::Ptr& literal, TypeId expected, SourceSpan span);
    TypeId patternLiteralType(const Token::Ptr& literal, TypeId expected);
    void reportNilPatternMismatch(SourceSpan span, TypeId expected);
    PatternCoverage checkEnumCasePattern(
            const syntax::EnumCasePatternSyntax::Ptr& pattern,
            TypeId expected);
    std::vector<PatternCoverage> validatePatternArguments(
            const syntax::EnumCasePatternSyntax& pattern,
            semantic::SymbolId caseSymbol,
            const TypedCallableSignature& signature);
    bool patternMatrixIsExhaustive(
            const std::vector<TypeId>& columns,
            const std::vector<PatternRow>& rows,
            size_t& remainingSpecializations,
            size_t depth = 0) const;
    void checkMatchExhaustiveness(
            const syntax::MatchExprSyntax::Ptr& expression,
            TypeId scrutinee,
            const std::vector<PatternCoverage>& coverage);
    std::vector<semantic::SymbolId> enumCases(TypeId type) const;
    std::optional<semantic::SymbolId> optionalAbsentCase(TypeId type) const;

    // collections.cpp
    bool containsFunctionType(TypeId type) const;
    bool genericLibraryKey(TypeId key, SourceSpan span) const;
    void rejectInvalidDictionaryKey(TypeId key, SourceSpan span);
    TypeId checkSubscript(const syntax::SubscriptExprSyntax::Ptr& expression);
    bool admittedKeyKind(TypeId type) const;
    TypeId rejectNeverElement(TypeId element, SourceSpan span, bool literal = false);
    TypeId rejectNeverDictionaryPart(TypeId type, SourceSpan span, const char* part, bool literal = false);
    TypeId checkArray(
            const syntax::ArrayExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
    TypeId checkDictionary(
            const syntax::DictionaryExprSyntax::Ptr& expression,
            std::optional<TypeId> expected);
};

} // namespace joyeer::typing

#endif
