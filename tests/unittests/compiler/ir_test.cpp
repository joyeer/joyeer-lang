#include "compiler/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_set>

namespace {

using namespace joyeer::ir;

Module validAddModule() {
    Module module;
    module.sourceName = "manual.joyeer";
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "Int", joyeer::typing::TypeKind::integer },
        TypeName { 2, "Bool", joyeer::typing::TypeKind::boolean },
    };

    Function add;
    add.id = 0;
    add.name = "add";
    add.parameters = {
        Parameter { Value { 0, 1, ValueCategory::value }, std::nullopt, "left", false, {} },
        Parameter { Value { 1, 1, ValueCategory::value }, std::nullopt, "right", false, {} },
    };
    add.resultType = 1;
    add.returnsValue = true;
    add.entry = 0;
    add.blocks = {
        BasicBlock {
            0,
            "entry",
            {
                Instruction {
                    Opcode::add,
                    Value { 2, 1, ValueCategory::value },
                    { 0, 1 },
                    {},
                    std::nullopt,
                    std::nullopt,
                    0,
                    {},
                    SourceSpan { 10, 3 },
                },
                Instruction {
                    Opcode::ret,
                    std::nullopt,
                    { 2 },
                    {},
                    std::nullopt,
                    std::nullopt,
                    0,
                    {},
                    SourceSpan { 20, 1 },
                },
            },
        },
    };
    module.functions.push_back(std::move(add));
    return module;
}

bool hasError(const VerificationResult& result, VerificationErrorId id) {
    return std::any_of(result.errors.begin(), result.errors.end(), [id](const auto& error) {
        return error.id == id;
    });
}

void addValidVariableDebugInfo(Module& module) {
    module.sourceInfo = SourceInfo {
        "manual.joyeer",
        "C:/source",
        24,
        { 0, 10, 20 },
    };
    module.debugScopes.push_back(DebugScope {
        0,
        DebugScopeKind::function,
        0,
        std::nullopt,
        1,
        SourceSpan { 0, 24 },
    });
    module.debugVariables.push_back(DebugVariable {
        0,
        DebugVariableKind::local,
        0,
        0,
        10,
        "value",
        1,
        SourceSpan { 10, 3 },
        std::nullopt,
        true,
    });
    auto& function = module.functions[0];
    function.debugScope = 0;
    function.debugLocation = DebugLocation { SourceSpan { 0, 24 }, false, 0 };
    for (auto& instruction : function.blocks[0].instructions) {
        instruction.debugLocation = DebugLocation { instruction.span, false, 0 };
    }
    Instruction allocate {
        Opcode::stackAllocate,
        Value { 3, 1, ValueCategory::address },
    };
    allocate.span = SourceSpan { 10, 3 };
    allocate.debugLocation = DebugLocation { allocate.span, false, 0 };
    allocate.debugVariableBindings.push_back(DebugVariableBinding { 0, 3 });
    function.blocks[0].instructions.insert(
            function.blocks[0].instructions.begin(),
            std::move(allocate));
}

TEST(IRModelTest, VerifiesAndDumpsAWellFormedFunctionDeterministically) {
    const auto module = validAddModule();
    const auto verification = Verifier().verify(module);
    ASSERT_TRUE(verification.succeeded()) << dump(verification);

    EXPECT_EQ(dump(module), R"IR(module "manual.joyeer" {
  type !0 = "Void"
  type !1 = "Int"
  type !2 = "Bool"

  func @0 "add"(%0: Int "left", %1: Int "right") -> Int {
    ^0 "entry":
      %2: Int = add %0, %1 @10:3
      ret %2 @20:1
  }
}
)IR");
}

// The unit value is a `literal` of `Void` holding 0 (decision 0028).
TEST(IRModelTest, VerifiesUnitLiteralShapeAndType) {
    Module module;
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "Int", joyeer::typing::TypeKind::integer },
    };
    Function function;
    function.id = 0;
    function.name = "unit";
    function.resultType = 0;
    function.entry = 0;
    function.blocks = { BasicBlock { 0, "entry", {
        Instruction { Opcode::literal, Value { 0, 0, ValueCategory::value } },
        Instruction { Opcode::ret },
    } } };
    module.functions.push_back(std::move(function));
    EXPECT_TRUE(Verifier().verify(module).succeeded());
    EXPECT_NE(dump(module).find("%0: Void = literal 0"), std::string::npos) << dump(module);
    EXPECT_FALSE(requiresDestruction(module, 0));

    auto& unit = module.functions[0].blocks[0].instructions[0];
    unit.integerValue = 1;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::typeMismatch));
    unit.integerValue = 0;
    unit.result->category = ValueCategory::address;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::typeMismatch));
    unit.result->category = ValueCategory::value;
    unit.targets = { 0 };
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidInstruction));
}

TEST(IRModelTest, VerifiesCheckedDivisionAndRemainderTypesAndShapes) {
    for (const auto opcode : { Opcode::divide, Opcode::remainder }) {
        auto module = validAddModule();
        module.types[1].kind = joyeer::typing::TypeKind::integer;
        module.functions[0].blocks[0].instructions[0].opcode = opcode;
        ASSERT_TRUE(Verifier().verify(module).succeeded());
        EXPECT_NE(dump(module).find(std::string(opcodeName(opcode)) + " %0, %1"), std::string::npos);

        auto malformed = module;
        malformed.functions[0].blocks[0].instructions[0].operands.pop_back();
        EXPECT_FALSE(Verifier().verify(malformed).succeeded());
        malformed = module;
        malformed.types[1].kind = joyeer::typing::TypeKind::boolean;
        EXPECT_TRUE(hasError(Verifier().verify(malformed), VerificationErrorId::typeMismatch));
        malformed = module;
        malformed.functions[0].parameters[0].value.category = ValueCategory::address;
        EXPECT_TRUE(hasError(Verifier().verify(malformed), VerificationErrorId::typeMismatch));
        malformed = module;
        malformed.functions[0].blocks[0].instructions[0].result->type = 2;
        EXPECT_TRUE(hasError(Verifier().verify(malformed), VerificationErrorId::typeMismatch));
    }
}

TEST(IRModelTest, VerifiesSourceMapsAndDebugLocations) {
    auto module = validAddModule();
    module.sourceInfo = SourceInfo {
        "manual.joyeer",
        "C:/source",
        24,
        { 0, 10, 20 },
    };
    module.functions[0].debugLocation = DebugLocation { SourceSpan { 0, 24 }, false };
    for (auto& instruction : module.functions[0].blocks[0].instructions) {
        instruction.debugLocation = DebugLocation { instruction.span, false };
    }

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(verification.succeeded()) << dump(verification);
}

TEST(IRModelTest, RejectsMalformedSourceMapsAndOutOfBoundsDebugLocations) {
    auto module = validAddModule();
    module.sourceInfo = SourceInfo {
        "manual.joyeer",
        "C:/source",
        24,
        { 1, 20, 20 },
    };
    module.functions[0].debugLocation = DebugLocation { SourceSpan { 23, 2 }, false };

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::invalidSourceLocation));
}

TEST(IRModelTest, VerifiesSourceIdsAgainstTheirOwnFileMaps) {
    auto module = validAddModule();
    module.sourceFiles = {
        SourceInfo { "root.joyeer", "C:/source", 1, { 0 } },
        SourceInfo { "dependency.joyeer", "C:/source/dep", 24, { 0, 10, 20 } },
    };
    auto& function = module.functions[0];
    function.debugLocation = DebugLocation { SourceSpan { 0, 24, 1 } };
    for (auto& instruction : function.blocks[0].instructions) {
        instruction.span.sourceId = 1;
        instruction.debugLocation = DebugLocation { instruction.span };
    }
    ASSERT_TRUE(Verifier().verify(module).succeeded()) << dump(Verifier().verify(module));
    EXPECT_NE(dump(module).find("source #1 \"dependency.joyeer\""), std::string::npos);
    EXPECT_NE(dump(module).find("@10:3 source#1"), std::string::npos);
    function.debugLocation->span.sourceId = 0;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
    function.debugLocation->span.sourceId = 2;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
    function.debugLocation->span.sourceId = 1;
    module.sourceFiles[1].lineStarts = { 0, 25 };
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
}

TEST(IRModelTest, ValidatesGraphScopeVariableAndTypeSourceIds) {
    auto module = validAddModule();
    addValidVariableDebugInfo(module);
    module.sourceFiles = { *module.sourceInfo, *module.sourceInfo };
    module.sourceInfo.reset();
    ASSERT_TRUE(Verifier().verify(module).succeeded());
    module.debugScopes[0].span.sourceId = 2;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
    module.debugScopes[0].span.sourceId = 1;
    module.debugVariables[0].span.sourceId = 2;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
    module.debugVariables[0].span.sourceId = 1;
    module.types[1].declarationSpan = SourceSpan { 0, 1, 2 };
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
    module.types[1].declarationSpan->sourceId = 1;
    EXPECT_TRUE(Verifier().verify(module).succeeded());
}

TEST(IRModelTest, LegacySourceInfoOnlyAcceptsSourceIdZero) {
    auto module = validAddModule();
    addValidVariableDebugInfo(module);
    module.functions[0].debugLocation->span.sourceId = 1;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::invalidSourceLocation));
}

TEST(IRModelTest, RejectsDebugLocationsWithoutSourceInfo) {
    auto module = validAddModule();
    module.functions[0].debugLocation = DebugLocation { SourceSpan { 0, 1 }, false };
    module.functions[0].blocks[0].instructions[0].debugLocation =
            DebugLocation { SourceSpan { 10, 3 }, false };

    const auto verification = Verifier().verify(module);
    ASSERT_EQ(verification.errors.size(), 1u) << dump(verification);
    EXPECT_EQ(verification.errors[0].id, VerificationErrorId::invalidSourceLocation);
}

TEST(IRModelTest, RejectsSourcesBeyondTheSourceSpanCapacity) {
    auto module = validAddModule();
    module.sourceInfo = SourceInfo {
        "manual.joyeer",
        "C:/source",
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1,
        { 0 },
    };

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::invalidSourceLocation));
}

TEST(IRModelTest, RejectsInstructionLocationsWithoutAFunctionLocation) {
    auto module = validAddModule();
    module.sourceInfo = SourceInfo {
        "manual.joyeer",
        "C:/source",
        24,
        { 0, 10, 20 },
    };
    module.functions[0].blocks[0].instructions[0].debugLocation =
            DebugLocation { SourceSpan { 10, 3 }, false };

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::invalidSourceLocation));
}

TEST(IRModelTest, VerifiesLexicalScopesVariablesAndStorageBindings) {
    auto module = validAddModule();
    addValidVariableDebugInfo(module);

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(verification.succeeded()) << dump(verification);
    const auto text = dump(module);
    EXPECT_NE(text.find("debug_scope #0 function"), std::string::npos);
    EXPECT_NE(text.find("debug_var #0 local \"value\""), std::string::npos);
    EXPECT_NE(text.find("debug_bind #0 -> %3"), std::string::npos);
}

TEST(IRModelTest, RejectsCyclicDebugScopes) {
    auto module = validAddModule();
    addValidVariableDebugInfo(module);
    module.debugScopes.push_back(DebugScope {
        1,
        DebugScopeKind::lexicalBlock,
        0,
        1,
        2,
        SourceSpan { 10, 3 },
    });

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::invalidReference));
}

TEST(IRModelTest, RejectsDuplicateAndNonAddressDebugBindings) {
    auto module = validAddModule();
    addValidVariableDebugInfo(module);
    module.functions[0].blocks[0].instructions[1].debugVariableBindings.push_back(
            DebugVariableBinding { 0, 2 });

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch));
}

TEST(IRModelTest, AcceptsStackSlotsLoadsAndStores) {
    auto module = validAddModule();
    auto& function = module.functions[0];
    function.name = "stack";
    function.parameters.clear();
    function.resultType = 0;
    function.returnsValue = false;
    function.blocks[0].instructions = {
        Instruction {
            Opcode::stackAllocate,
            Value { 0, 1, ValueCategory::address },
            {},
            {},
            std::nullopt,
            std::nullopt,
            0,
            {},
            {},
        },
        Instruction {
            Opcode::literal,
            Value { 1, 1, ValueCategory::value },
            {},
            {},
            std::nullopt,
            std::nullopt,
            42,
            {},
            {},
        },
        Instruction {
            Opcode::store,
            std::nullopt,
            { 1, 0 },
            {},
            std::nullopt,
            std::nullopt,
            0,
            {},
            {},
        },
        Instruction {
            Opcode::load,
            Value { 2, 1, ValueCategory::value },
            { 0 },
            {},
            std::nullopt,
            std::nullopt,
            0,
            {},
            {},
        },
        Instruction { Opcode::ret },
    };

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(verification.succeeded()) << dump(verification);
}

TEST(IRModelTest, VerifiesAndDumpsZeroInitializedStorage) {
    auto module = validAddModule();
    auto& function = module.functions[0];
    function.name = "zero";
    function.parameters.clear();
    function.resultType = 0;
    function.returnsValue = false;
    function.blocks[0].instructions = {
        Instruction {
            Opcode::stackAllocate,
            Value { 0, 1, ValueCategory::address },
        },
        Instruction { Opcode::zeroInitialize, std::nullopt, { 0 } },
        Instruction { Opcode::ret },
    };

    const auto verification = Verifier().verify(module);
    ASSERT_TRUE(verification.succeeded()) << dump(verification);
    EXPECT_NE(dump(module).find("zero_init %0"), std::string::npos);
}

TEST(IRModelTest, RejectsZeroInitializationOfNonAddressValues) {
    auto module = validAddModule();
    auto& instructions = module.functions[0].blocks[0].instructions;
    instructions.insert(
            instructions.end() - 1,
            Instruction { Opcode::zeroInitialize, std::nullopt, { 0 } });

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch));
}

// The builtin array (wire 10) and dictionary (wire 11) type kinds were removed when the library `Array` and `Dict`
// replaced them (decisions 0026, 0027); their slots stay rejected and are never reused.
TEST(IRModelTest, RemovedTypeKindSlotsAreRejectedAndNeverReused) {
    EXPECT_EQ(static_cast<unsigned>(joyeer::typing::TypeKind::int8), 12u);
    for (const int kind : { 10, 11 }) {
        auto module = validAddModule();
        module.types.push_back(TypeName {
            3, "removed kind", static_cast<joyeer::typing::TypeKind>(kind), joyeer::semantic::invalidSymbolId, { 1, 2 },
        });
        const auto verification = Verifier().verify(module);
        EXPECT_FALSE(verification.succeeded()) << kind;
        EXPECT_NE(dump(verification).find("unknown or removed type kind"), std::string::npos) << kind;
    }
}

// Opcode values are dense (decision 0028, stage 4): a value past the table, or a negative one, is rejected.
TEST(IRModelTest, OpcodeValuesOutsideTheTableAreRejected) {
    for (const int value : { static_cast<int>(Opcode::stackBlock) + 1, 1000, -1 }) {
        auto module = validAddModule();
        module.functions[0].blocks[0].instructions[0].opcode = static_cast<Opcode>(value);
        const auto verification = Verifier().verify(module);
        EXPECT_FALSE(verification.succeeded()) << value;
        EXPECT_NE(dump(verification).find("unknown opcode"), std::string::npos) << value;
    }
}

// Decision 0024: every malformed role binding is rejected by the verifier, not trusted from lowering.
TEST(IRModelTest, VerifierRejectsMalformedRoleEnumerations) {
    auto module = validAddModule();
    module.types.push_back(TypeName {
        4, "Bool?", joyeer::typing::TypeKind::enumeration, 10, { 2 },
    });
    module.types.push_back(TypeName {
        6, "Result<Bool, Int>", joyeer::typing::TypeKind::enumeration, 20, { 2, 1 },
    });
    module.enumerations.push_back(EnumerationDefinition {
        10, 4, "Bool?",
        { EnumCaseDefinition { 11, "Some", { 2 } }, EnumCaseDefinition { 12, "None", {} } },
        EnumerationRole::optional, 0, 1,
    });
    module.enumerations.push_back(EnumerationDefinition {
        20, 6, "Result<Bool, Int>",
        { EnumCaseDefinition { 21, "Ok", { 2 } }, EnumCaseDefinition { 22, "Err", { 1 } } },
        EnumerationRole::result, 0, 1,
    });
    const auto valid = Verifier().verify(module);
    ASSERT_TRUE(valid.succeeded()) << dump(valid);

    // Each mutation must trip the role rule itself, not some unrelated type check.
    const auto rejects = [&](const char* what, const auto& mutate) {
        auto invalid = module;
        mutate(invalid);
        const auto verification = Verifier().verify(invalid);
        EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch)) << what;
        EXPECT_TRUE(std::any_of(verification.errors.begin(), verification.errors.end(), [](const auto& error) {
            return error.message.find("role") != std::string::npos;
        })) << what << ": " << dump(verification);
    };
    // Equal type arguments make the payloads alone unable to tell success from failure.
    {
        auto equal = module;
        equal.types.push_back(TypeName {
            7, "Result<Int, Int>", joyeer::typing::TypeKind::enumeration, 20, { 1, 1 },
        });
        equal.enumerations.push_back(EnumerationDefinition {
            20, 7, "Result<Int, Int>",
            { EnumCaseDefinition { 31, "Ok", { 1 } }, EnumCaseDefinition { 32, "Err", { 1 } } },
            EnumerationRole::result, 0, 1,
        });
        ASSERT_TRUE(Verifier().verify(equal).succeeded()) << dump(Verifier().verify(equal));
        std::swap(equal.enumerations.back().firstRoleCase, equal.enumerations.back().secondRoleCase);
        const auto swapped = Verifier().verify(equal);
        EXPECT_TRUE(hasError(swapped, VerificationErrorId::typeMismatch)) << dump(swapped);
    }
    rejects("role value outside the closed set", [](auto& m) {
        m.enumerations[1].role = static_cast<EnumerationRole>(7);
    });
    rejects("one declaration bound to both roles", [](auto& m) {
        m.enumerations[1].symbol = 10;
    });
    rejects("optional payload disagrees with the type argument", [](auto& m) {
        m.enumerations[0].cases[0].payloadTypes = { 1 };
    });
    rejects("result success and failure swapped", [](auto& m) {
        std::swap(m.enumerations[1].firstRoleCase, m.enumerations[1].secondRoleCase);
    });
    rejects("result with a third case", [](auto& m) {
        m.enumerations[1].cases.push_back(EnumCaseDefinition { 23, "Extra", {} });
    });
    rejects("result failure case without a payload", [](auto& m) {
        m.enumerations[1].cases[1].payloadTypes.clear();
    });
    rejects("optional absent case with a payload", [](auto& m) {
        m.enumerations[0].cases[1].payloadTypes = { 2 };
    });
    rejects("case index out of range", [](auto& m) {
        m.enumerations[1].secondRoleCase = 2;
    });
    rejects("role type is not an enumeration", [](auto& m) {
        m.types[m.types.size() - 1].kind = joyeer::typing::TypeKind::structure;
    });
    rejects("result failure payload disagrees with the second type argument", [](auto& m) {
        m.enumerations[1].cases[1].payloadTypes = { 2 };
    });
    rejects("wrong number of type arguments", [](auto& m) {
        m.types[m.types.size() - 1].arguments = { 2 };
    });
    rejects("type and definition name different declarations", [](auto& m) {
        m.types[m.types.size() - 1].symbol = 99;
    });
    rejects("two declarations bound to the optional role", [](auto& m) {
        m.types.push_back(TypeName { 7, "Int?", joyeer::typing::TypeKind::enumeration, 30, { 1 } });
        m.enumerations.push_back(EnumerationDefinition {
            30, 7, "Int?",
            { EnumCaseDefinition { 31, "Some", { 1 } }, EnumCaseDefinition { 32, "None", {} } },
            EnumerationRole::optional, 0, 1,
        });
    });
}

TEST(IRModelTest, ReportsDuplicateAndUndefinedValueIds) {
    auto module = validAddModule();
    auto& add = module.functions[0].blocks[0].instructions[0];
    add.result->id = 0;
    add.operands[1] = 99;

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::duplicateId));
    EXPECT_TRUE(hasError(verification, VerificationErrorId::invalidReference));
}

TEST(IRModelTest, ReportsMissingTerminatorsAndInstructionsAfterTerminators) {
    auto module = validAddModule();
    auto& instructions = module.functions[0].blocks[0].instructions;
    instructions.push_back(Instruction {
        Opcode::literal,
        Value { 3, 1, ValueCategory::value },
    });

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::instructionAfterTerminator));
    EXPECT_TRUE(hasError(verification, VerificationErrorId::missingTerminator));
}

TEST(IRModelTest, ReportsCallSignatureMismatches) {
    auto module = validAddModule();

    Function caller;
    caller.id = 1;
    caller.name = "caller";
    caller.resultType = 1;
    caller.returnsValue = true;
    caller.entry = 0;
    caller.blocks = {
        BasicBlock {
            0,
            "entry",
            {
                Instruction {
                    Opcode::call,
                    Value { 0, 1, ValueCategory::value },
                    {},
                    {},
                    FunctionId { 0 },
                },
                Instruction {
                    Opcode::ret,
                    std::nullopt,
                    { 0 },
                },
            },
        },
    };
    module.functions.push_back(std::move(caller));

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::callMismatch));
}

TEST(IRModelTest, ClassifiesNestedTypesThatRequireDestruction) {
    Module module;
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "Int", joyeer::typing::TypeKind::integer },
        TypeName { 2, "String", joyeer::typing::TypeKind::string },
        TypeName { 3, "Box", joyeer::typing::TypeKind::structure, 30 },
        TypeName { 4, "MaybeBox", joyeer::typing::TypeKind::enumeration, 40 },
    };
    module.structures = {
        StructureDefinition {
            30,
            3,
            "Box",
            { FieldDefinition { 31, "text", 2, false } },
        },
    };
    module.enumerations = {
        EnumerationDefinition {
            40,
            4,
            "MaybeBox",
            {
                EnumCaseDefinition { 41, "None", {} },
                EnumCaseDefinition { 42, "Some", { 3 } },
            },
        },
    };

    EXPECT_FALSE(requiresDestruction(module, 0));
    EXPECT_FALSE(requiresDestruction(module, 1));
    EXPECT_TRUE(requiresDestruction(module, 2));
    EXPECT_TRUE(requiresDestruction(module, 3));
    EXPECT_TRUE(requiresDestruction(module, 4));

    const DestructionAnalysis analysis(module);
    for (const auto id : {4u, 2u, 0u, 3u, 1u, 4u}) {
        EXPECT_EQ(analysis.requiresDestruction(id), requiresDestruction(module, id));
    }
    EXPECT_FALSE(analysis.requiresDestruction(999));
}

TEST(IRModelTest, KeepsDestructionAnalysisLocalToItsTypeSnapshot) {
    Module module;
    module.types = {
        TypeName { 42, "Box", joyeer::typing::TypeKind::structure },
        TypeName { 7, "Element", joyeer::typing::TypeKind::integer },
    };
    module.structures = {
        StructureDefinition { 0, 42, "Box", {FieldDefinition { 1, "value", 7, false }} },
    };
    const DestructionAnalysis trivial(module);
    EXPECT_FALSE(trivial.requiresDestruction(42));

    module.types[1].kind = joyeer::typing::TypeKind::string;
    const DestructionAnalysis managed(module);
    EXPECT_TRUE(managed.requiresDestruction(42));
    EXPECT_TRUE(managed.requiresDestruction(7));
    EXPECT_FALSE(trivial.requiresDestruction(42));
    EXPECT_FALSE(trivial.requiresDestruction(7));
}

TEST(IRModelTest, ConservativelyClassifiesRecursiveDestructionDependencies) {
    Module module;
    module.types = {
        TypeName { 42, "Left", joyeer::typing::TypeKind::structure },
        TypeName { 7, "Right", joyeer::typing::TypeKind::enumeration },
        TypeName { 100, "Int", joyeer::typing::TypeKind::integer },
    };
    module.structures = {
        StructureDefinition { 0, 42, "Left", {FieldDefinition { 1, "next", 7, false }} },
    };
    module.enumerations = {
        EnumerationDefinition { 2, 7, "Right", {EnumCaseDefinition { 3, "Next", {42} }} },
    };
    const DestructionAnalysis analysis(module);
    EXPECT_TRUE(analysis.requiresDestruction(7));
    EXPECT_TRUE(analysis.requiresDestruction(42));
    EXPECT_FALSE(analysis.requiresDestruction(100));
}

BasicBlock terminatorBlock(BlockId id, Opcode opcode, std::vector<BlockId> targets = {}) {
    BasicBlock block;
    block.id = id;
    block.instructions.push_back(Instruction { opcode, std::nullopt, {}, std::move(targets) });
    return block;
}

BasicBlock switchBlock(BlockId id, const std::vector<BlockId>& targets) {
    BasicBlock block = terminatorBlock(id, Opcode::switchPattern);
    for (const auto target : targets) {
        SwitchCase switchCase;
        switchCase.target = target;
        block.instructions.back().switchCases.push_back(switchCase);
    }
    return block;
}

// A chain of `count` diamonds: block 3i branches to 3i+1 and 3i+2, which both join at 3i+3.
Function diamondChain(size_t count, bool reversed) {
    Function function;
    function.entry = 0;
    for (BlockId diamond = 0; diamond < count; ++diamond) {
        const BlockId head = diamond * 3;
        function.blocks.push_back(terminatorBlock(head, Opcode::conditionalBranch, {head + 1, head + 2}));
        function.blocks.push_back(terminatorBlock(head + 1, Opcode::branch, {head + 3}));
        function.blocks.push_back(terminatorBlock(head + 2, Opcode::branch, {head + 3}));
    }
    function.blocks.push_back(terminatorBlock(static_cast<BlockId>(count * 3), Opcode::ret));
    if (reversed) std::reverse(function.blocks.begin(), function.blocks.end());
    return function;
}

TEST(IRModelTest, DominanceFollowsBranchesSwitchesAndBackEdges) {
    Function function;
    function.entry = 4;
    function.blocks = {
        terminatorBlock(1, Opcode::branch, {3}),
        terminatorBlock(2, Opcode::branch, {3}),
        terminatorBlock(3, Opcode::conditionalBranch, {5, 4}),
        switchBlock(5, {6, 7}),
        terminatorBlock(6, Opcode::branch, {8}),
        terminatorBlock(7, Opcode::branch, {8, 77}),
        terminatorBlock(8, Opcode::ret),
        // The entry is last in the vector and is a back-edge target; a target that is not a block is ignored.
        terminatorBlock(4, Opcode::conditionalBranch, {1, 2}),
    };
    const DominanceAnalysis dominance(function);
    EXPECT_TRUE(dominance.dominates(4, 8));
    EXPECT_TRUE(dominance.dominates(3, 8));
    EXPECT_TRUE(dominance.dominates(5, 8));
    EXPECT_TRUE(dominance.dominates(8, 8));
    EXPECT_FALSE(dominance.dominates(1, 3));
    EXPECT_FALSE(dominance.dominates(2, 3));
    EXPECT_FALSE(dominance.dominates(6, 8));
    EXPECT_FALSE(dominance.dominates(7, 8));
    EXPECT_FALSE(dominance.dominates(3, 4));
    EXPECT_FALSE(dominance.dominates(8, 5));
    EXPECT_FALSE(dominance.dominates(77, 8));
    EXPECT_FALSE(dominance.dominates(4, 77));
}

TEST(IRModelTest, DominanceTreatsBlocksWithoutPredecessorsAsRootsAndUnrootedCyclesAsTop) {
    Function function;
    function.entry = 0;
    function.blocks = {
        terminatorBlock(0, Opcode::conditionalBranch, {1, 6}),
        terminatorBlock(1, Opcode::branch, {10}),
        // 5 has no predecessors, so it roots its own region; 6 joins it with the entry's region.
        terminatorBlock(5, Opcode::conditionalBranch, {6, 7}),
        terminatorBlock(6, Opcode::ret),
        terminatorBlock(7, Opcode::ret),
        // 8 and 9 reach each other but no root reaches them; 10 joins them with the entry.
        terminatorBlock(8, Opcode::conditionalBranch, {9, 10}),
        terminatorBlock(9, Opcode::branch, {8}),
        terminatorBlock(10, Opcode::ret),
        terminatorBlock(11, Opcode::branch, {10}),
        terminatorBlock(12, Opcode::branch, {12}),
    };
    const DominanceAnalysis dominance(function);
    EXPECT_TRUE(dominance.dominates(5, 5));
    EXPECT_TRUE(dominance.dominates(5, 7));
    EXPECT_FALSE(dominance.dominates(0, 7));
    EXPECT_FALSE(dominance.dominates(0, 6));
    EXPECT_FALSE(dominance.dominates(5, 6));
    EXPECT_TRUE(dominance.dominates(6, 6));
    EXPECT_FALSE(dominance.dominates(0, 5));
    // An unrooted cycle, and a block that only loops to itself, are dominated by every block.
    EXPECT_TRUE(dominance.dominates(0, 8));
    EXPECT_TRUE(dominance.dominates(7, 9));
    EXPECT_TRUE(dominance.dominates(10, 8));
    EXPECT_TRUE(dominance.dominates(1, 12));
    EXPECT_FALSE(dominance.dominates(99, 8));
    // 10 is reached from the entry through 1, from the unrooted cycle, and from the root 11.
    EXPECT_FALSE(dominance.dominates(0, 10));
    EXPECT_FALSE(dominance.dominates(11, 10));
    EXPECT_FALSE(dominance.dominates(8, 10));
    EXPECT_TRUE(dominance.dominates(10, 10));
}

TEST(IRModelTest, DominanceWorkGrowsLinearlyWithStraightLineBranching) {
    for (const bool reversed : {false, true}) {
        const auto function = diamondChain(300, reversed);
        const DominanceAnalysis dominance(function);
        EXPECT_TRUE(dominance.dominates(0, 900));
        EXPECT_TRUE(dominance.dominates(450, 900));
        EXPECT_FALSE(dominance.dominates(451, 900));
        EXPECT_FALSE(dominance.dominates(452, 453));
        EXPECT_TRUE(dominance.dominates(450, 452));
        EXPECT_FALSE(dominance.dominates(900, 450));
        // Straight-line branching costs a bounded number of steps per block, whatever the block order,
        // so a long function is not quadratic to verify.
        EXPECT_LE(dominance.work(), 16 * function.blocks.size()) << (reversed ? "reversed" : "in order");
    }
}

TEST(IRModelTest, DominanceWorkForManyExitsToOneBlockGrowsWithTheirDepth) {
    // A loop body of `count` sequential `if c { break }` lowers to blocks that each branch to one shared
    // exit at increasing dominator depth. Each join climbs the dominator tree, so this shape costs about
    // 2 * count * count steps: a known limit, recorded here so a change to it is deliberate.
    constexpr BlockId count = 300;
    const BlockId exit = 2 * count;
    Function function;
    function.entry = 0;
    for (BlockId index = 0; index < count; ++index) {
        function.blocks.push_back(terminatorBlock(2 * index, Opcode::conditionalBranch, {exit, 2 * index + 1}));
        function.blocks.push_back(terminatorBlock(2 * index + 1, Opcode::branch, {2 * index + 2 == exit ? exit : 2 * index + 2}));
    }
    function.blocks.push_back(terminatorBlock(exit, Opcode::ret));
    const DominanceAnalysis dominance(function);
    EXPECT_TRUE(dominance.dominates(0, exit));
    EXPECT_FALSE(dominance.dominates(2, exit));
    EXPECT_TRUE(dominance.dominates(2, 2 * count - 1));
    EXPECT_LE(dominance.work(), 3 * static_cast<size_t>(count) * count);
}

TEST(IRModelTest, DominancePinsMalformedBlockShapes) {
    Function function;
    function.entry = 1;
    function.blocks = {
        terminatorBlock(1, Opcode::conditionalBranch, {2, 3}),
        // A duplicate id keeps the first block, but both contribute their edges.
        terminatorBlock(2, Opcode::branch, {4}),
        terminatorBlock(2, Opcode::branch, {5}),
        terminatorBlock(3, Opcode::branch, {4}),
        terminatorBlock(4, Opcode::ret),
        terminatorBlock(5, Opcode::ret),
        // Switch cases count only on a switch.
        terminatorBlock(6, Opcode::branch, {}),
    };
    BasicBlock empty;
    empty.id = 7;
    function.blocks.push_back(empty);
    SwitchCase stray;
    stray.target = 8;
    function.blocks[6].instructions.back().switchCases.push_back(stray);
    function.blocks.push_back(terminatorBlock(8, Opcode::ret));
    const DominanceAnalysis dominance(function);
    EXPECT_TRUE(dominance.dominates(1, 4));
    EXPECT_FALSE(dominance.dominates(2, 4));
    EXPECT_TRUE(dominance.dominates(2, 5));
    EXPECT_TRUE(dominance.dominates(7, 7));
    EXPECT_FALSE(dominance.dominates(1, 7));
    EXPECT_FALSE(dominance.dominates(6, 8));
    EXPECT_TRUE(dominance.dominates(8, 8));

    // Without the entry among the blocks, only blocks without predecessors root the graph.
    function.entry = 99;
    const DominanceAnalysis detached(function);
    EXPECT_TRUE(detached.dominates(1, 4));
    EXPECT_FALSE(detached.dominates(99, 1));
    EXPECT_FALSE(detached.dominates(1, 99));
}

// The verifier's dominance as a set-based maximal fixed point, which DominanceAnalysis must reproduce.
std::map<BlockId, std::set<BlockId>> naiveDominators(const Function& function) {
    std::set<BlockId> all;
    for (const auto& block : function.blocks) all.insert(block.id);
    std::map<BlockId, std::set<BlockId>> predecessors;
    for (const auto& block : function.blocks) {
        if (block.instructions.empty()) continue;
        const auto& terminator = block.instructions.back();
        for (const auto target : terminator.targets) {
            if (all.contains(target)) predecessors[target].insert(block.id);
        }
        if (terminator.opcode != Opcode::switchPattern) continue;
        for (const auto& switchCase : terminator.switchCases) {
            if (all.contains(switchCase.target)) predecessors[switchCase.target].insert(block.id);
        }
    }
    std::map<BlockId, std::set<BlockId>> dominators;
    for (const auto id : all) dominators[id] = id == function.entry ? std::set<BlockId> {id} : all;
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto id : all) {
            if (id == function.entry) continue;
            std::set<BlockId> next;
            bool first = true;
            for (const auto predecessor : predecessors[id]) {
                if (first) {
                    next = dominators[predecessor];
                    first = false;
                    continue;
                }
                std::erase_if(next, [&](BlockId candidate) { return !dominators[predecessor].contains(candidate); });
            }
            next.insert(id);
            if (next != dominators[id]) {
                dominators[id] = std::move(next);
                changed = true;
            }
        }
    }
    return dominators;
}

TEST(IRModelTest, DominanceMatchesTheSetBasedFixedPointOnRandomFunctions) {
    // std::mt19937's sequence is fixed by the standard; distributions are not, so draw with modulo.
    std::mt19937 random(20261118);
    const auto below = [&random](uint32_t bound) { return static_cast<BlockId>(random() % bound); };
    const Opcode terminators[] = {Opcode::branch, Opcode::conditionalBranch, Opcode::switchPattern, Opcode::ret};
    for (int trial = 0; trial < 3000; ++trial) {
        Function function;
        const auto blockCount = 1 + below(10);
        function.entry = below(blockCount + 1);
        for (BlockId index = 0; index < blockCount; ++index) {
            BasicBlock block;
            block.id = below(5) == 0 ? below(blockCount) : index;
            if (below(10) != 0) {
                block = terminatorBlock(block.id, terminators[below(4)]);
                for (auto targets = below(3); targets > 0; --targets) {
                    block.instructions.back().targets.push_back(below(blockCount + 2));
                }
                for (auto cases = below(3); cases > 0; --cases) {
                    SwitchCase switchCase;
                    switchCase.target = below(blockCount + 2);
                    block.instructions.back().switchCases.push_back(switchCase);
                }
            }
            function.blocks.push_back(std::move(block));
        }
        auto expected = naiveDominators(function);
        const DominanceAnalysis dominance(function);
        for (BlockId block = 0; block < blockCount + 2; ++block) {
            for (BlockId dominator = 0; dominator < blockCount + 2; ++dominator) {
                const auto found = expected.find(block);
                const bool want = found != expected.end() && found->second.contains(dominator);
                ASSERT_EQ(dominance.dominates(dominator, block), want)
                        << "trial " << trial << ": does " << dominator << " dominate " << block << "?";
            }
        }
    }
}

// Dominance rooted at one block along a given edge map (the dictionary literal protocol roots it at the packing
// block, in both directions): a block other than the root is reached from the root along a path that avoids another
// block exactly when it is reached at all and that block does not dominate it. Checked against a walk that stops at
// the avoided block, on random graphs with self loops, cycles and edges to blocks that have no edges of their own.
TEST(IRModelTest, RootedDominanceMatchesAWalkThatAvoidsEachBlock) {
    std::mt19937 random(20261118);
    const auto below = [&random](uint32_t bound) { return static_cast<BlockId>(random() % bound); };
    for (int trial = 0; trial < 2000; ++trial) {
        const auto blockCount = 1 + below(10);
        std::unordered_map<BlockId, std::unordered_set<BlockId>> edges;
        for (BlockId from = 0; from < blockCount; ++from) {
            for (auto count = below(4); count > 0; --count) edges[from].insert(below(blockCount + 2));
        }
        const auto root = below(blockCount);
        const auto walk = [&](std::optional<BlockId> avoided) {
            std::unordered_set<BlockId> reached;
            std::vector<BlockId> work { root };
            while (!work.empty()) {
                const auto current = work.back();
                work.pop_back();
                const auto next = edges.find(current);
                if (next == edges.end()) continue;
                for (const auto target : next->second) {
                    if (target != avoided && reached.insert(target).second) work.push_back(target);
                }
            }
            return reached;
        };
        const auto reached = walk(std::nullopt);
        const DominanceAnalysis dominance(root, edges);
        for (BlockId avoided = 0; avoided < blockCount + 2; ++avoided) {
            if (avoided == root) continue;
            const auto expected = walk(avoided);
            for (BlockId block = 0; block < blockCount + 2; ++block) {
                if (block == root) continue;
                const bool viaDominance = reached.contains(block) && !dominance.dominates(avoided, block);
                ASSERT_EQ(viaDominance, expected.contains(block))
                        << "trial " << trial << ": is " << block << " reached from " << root << " avoiding " << avoided
                        << "?";
            }
        }
        EXPECT_TRUE(dominance.dominates(root, root));
    }
}

TEST(IRModelTest, VerifiesExplicitCopyTakeAndDestroyOperations) {
    Module module;
    module.sourceName = "ownership.joyeer";
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "String", joyeer::typing::TypeKind::string },
    };
    Function function;
    function.id = 0;
    function.name = "ownership";
    function.resultType = 0;
    function.entry = 0;
    function.blocks = {
        BasicBlock {
            0,
            "entry",
            {
                Instruction {
                    Opcode::stringConstant,
                    Value { 0, 1, ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 0, "text",
                },
                Instruction {
                    Opcode::copyValue,
                    Value { 1, 1, ValueCategory::value },
                    { 0 },
                },
                Instruction {
                    Opcode::stackAllocate,
                    Value { 2, 1, ValueCategory::address },
                },
                Instruction { Opcode::store, std::nullopt, { 1, 2 } },
                Instruction {
                    Opcode::take,
                    Value { 3, 1, ValueCategory::value },
                    { 2 },
                },
                Instruction { Opcode::store, std::nullopt, { 3, 2 } },
                Instruction { Opcode::destroy, std::nullopt, { 2 } },
                Instruction { Opcode::ret },
            },
        },
    };
    module.functions.push_back(std::move(function));

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(verification.succeeded()) << dump(verification);
}

TEST(IRModelTest, RejectsDestroyingTrivialOrNonAddressValues) {
    auto module = validAddModule();
    auto& block = module.functions[0].blocks[0];
    block.instructions.insert(
            block.instructions.end() - 1,
            Instruction { Opcode::destroy, std::nullopt, { 0 } });

    const auto verification = Verifier().verify(module);
    EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch));
}

// The rule of `switch_pattern` cases (docs/impl/ir.md section 4): every pattern's kind fits its type, a top-level
// wildcard is the last case, and the cases cover every value of the scrutinee. Types 3 to 5 are `Never`, `Bool?`
// and `Never?`, whose cases are `Some` (11) and `None` (12) of the optional declaration 10; types 6 to 9 are
// `String`, `UInt8`, `Int8` and `Float64`.
Pattern anything(TypeId type) {
    Pattern pattern;
    pattern.type = type;
    return pattern;
}

Pattern enumCase(TypeId type, joyeer::semantic::SymbolId symbol, std::vector<Pattern> payloads = {}) {
    Pattern pattern;
    pattern.kind = PatternKind::enumCase;
    pattern.type = type;
    pattern.symbol = symbol;
    pattern.payloads = std::move(payloads);
    return pattern;
}

Pattern literal(PatternKind kind, TypeId type, int64_t value) {
    Pattern pattern;
    pattern.kind = kind;
    pattern.type = type;
    pattern.integerValue = value;
    return pattern;
}

// `func pick(value: <scrutinee>)`, whose entry switches over `value` to returning blocks 1, 2, 3, ... in case order.
Module switchModule(TypeId scrutinee, const std::vector<Pattern>& patterns) {
    auto module = validAddModule();
    module.types.push_back(TypeName { 3, "Never", joyeer::typing::TypeKind::never });
    module.types.push_back(TypeName { 4, "Bool?", joyeer::typing::TypeKind::enumeration, 10, { 2 } });
    module.types.push_back(TypeName { 5, "Never?", joyeer::typing::TypeKind::enumeration, 10, { 3 } });
    module.types.push_back(TypeName { 6, "String", joyeer::typing::TypeKind::string });
    module.types.push_back(TypeName { 7, "UInt8", joyeer::typing::TypeKind::uint8 });
    module.types.push_back(TypeName { 8, "Int8", joyeer::typing::TypeKind::int8 });
    module.types.push_back(TypeName { 9, "Float64", joyeer::typing::TypeKind::float64 });
    module.enumerations.push_back(EnumerationDefinition {
        10, 4, "Bool?",
        { EnumCaseDefinition { 11, "Some", { 2 } }, EnumCaseDefinition { 12, "None", {} } },
        EnumerationRole::optional, 0, 1,
    });
    module.enumerations.push_back(EnumerationDefinition {
        10, 5, "Never?",
        { EnumCaseDefinition { 11, "Some", { 3 } }, EnumCaseDefinition { 12, "None", {} } },
        EnumerationRole::optional, 0, 1,
    });
    Function pick;
    pick.id = 1;
    pick.name = "pick";
    pick.parameters = { Parameter { Value { 10, scrutinee, ValueCategory::value }, std::nullopt, "value", false, {} } };
    pick.resultType = 0;
    pick.entry = 0;
    Instruction dispatch { Opcode::switchPattern, std::nullopt, { 10 } };
    for (size_t index = 0; index < patterns.size(); ++index) {
        dispatch.switchCases.push_back(SwitchCase { patterns[index], static_cast<BlockId>(index + 1) });
    }
    pick.blocks.push_back(BasicBlock { 0, "entry", { dispatch } });
    for (size_t index = 0; index < patterns.size(); ++index) {
        pick.blocks.push_back(BasicBlock {
            static_cast<BlockId>(index + 1), "arm", { Instruction { Opcode::ret } } });
    }
    module.functions.push_back(std::move(pick));
    return module;
}

Pattern someBool(Pattern payload) {
    return enumCase(4, 11, { std::move(payload) });
}

Pattern boolean(bool value) {
    return literal(PatternKind::booleanLiteral, 2, value ? 1 : 0);
}

Pattern integer(int64_t value) {
    return literal(PatternKind::integerLiteral, 1, value);
}

Pattern textual(PatternKind kind, TypeId type, std::string text) {
    Pattern pattern;
    pattern.kind = kind;
    pattern.type = type;
    pattern.text = std::move(text);
    return pattern;
}

TEST(IRModelTest, SwitchPatternAcceptsCoveringCasesEndedByAnyWildcard) {
    const std::vector<std::pair<const char*, Module>> valid {
        { "Some(_), None", switchModule(4, { someBool(anything(2)), enumCase(4, 12) }) },
        { "None, Some(_)", switchModule(4, { enumCase(4, 12), someBool(anything(2)) }) },
        { "Some(_), _", switchModule(4, { someBool(anything(2)), anything(4) }) },
        { "_ alone", switchModule(4, { anything(4) }) },
        { "nested Booleans", switchModule(4, { someBool(boolean(true)), someBool(boolean(false)), enumCase(4, 12) }) },
        { "a nested case, then a wildcard", switchModule(4, { someBool(boolean(true)), anything(4) }) },
        // A case that an earlier one already covers is dead but harmless: the first matching case wins.
        { "a covered case", switchModule(4, { someBool(anything(2)), someBool(boolean(true)), enumCase(4, 12) }) },
        { "Never? with both cases", switchModule(5, { enumCase(5, 11, { anything(3) }), enumCase(5, 12) }) },
        { "Bool literals", switchModule(2, { boolean(false), boolean(true) }) },
        { "Int literals, then a wildcard", switchModule(1, { integer(1), integer(2), anything(1) }) },
    };
    for (const auto& [what, module] : valid) {
        const auto verification = Verifier().verify(module);
        EXPECT_TRUE(verification.succeeded()) << what << ": " << dump(verification);
    }
}

TEST(IRModelTest, SwitchPatternRejectsCasesAfterAWildcard) {
    // The backend branches to the first wildcard and never tests the cases after it.
    const std::vector<std::pair<const char*, Module>> invalid {
        { "_, Some(_)", switchModule(4, { anything(4), someBool(anything(2)) }) },
        { "_, _", switchModule(4, { anything(4), anything(4) }) },
        { "Some(_), _, None", switchModule(4, { someBool(anything(2)), anything(4), enumCase(4, 12) }) },
        { "Int: _, 1", switchModule(1, { anything(1), integer(1) }) },
    };
    for (const auto& [what, module] : invalid) {
        const auto verification = Verifier().verify(module);
        EXPECT_TRUE(hasError(verification, VerificationErrorId::unreachableSwitchCase))
                << what << ": " << dump(verification);
        EXPECT_FALSE(hasError(verification, VerificationErrorId::typeMismatch)) << what;
    }
}

TEST(IRModelTest, SwitchPatternRejectsCasesThatMissAValue) {
    // The backend ends the case tests with `unreachable`, so a value that no case matches is undefined behaviour.
    const std::vector<std::pair<const char*, Module>> invalid {
        { "Some(_) alone", switchModule(4, { someBool(anything(2)) }) },
        { "None alone", switchModule(4, { enumCase(4, 12) }) },
        { "Some(true), None", switchModule(4, { someBool(boolean(true)), enumCase(4, 12) }) },
        { "Bool: true", switchModule(2, { boolean(true) }) },
        { "Int literals", switchModule(1, { integer(1), integer(2) }) },
        // An uninhabited payload does not remove its case: every case still needs a pattern.
        { "Never?: None alone", switchModule(5, { enumCase(5, 12) }) },
    };
    for (const auto& [what, module] : invalid) {
        const auto verification = Verifier().verify(module);
        EXPECT_TRUE(hasError(verification, VerificationErrorId::nonExhaustiveSwitch))
                << what << ": " << dump(verification);
        EXPECT_FALSE(hasError(verification, VerificationErrorId::typeMismatch)) << what;
    }
}

TEST(IRModelTest, SwitchPatternRejectsAWildcardPayloadOfTheWrongType) {
    // `Never?`'s `Some` carries `Never`; a wildcard typed as `Bool` or as the scrutinee is not its payload.
    for (const TypeId payload : { TypeId { 2 }, TypeId { 5 } }) {
        const auto verification = Verifier().verify(
                switchModule(5, { enumCase(5, 11, { anything(payload) }), enumCase(5, 12) }));
        EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch))
                << payload << ": " << dump(verification);
    }
    // Nor is a top-level wildcard of another type a case of the scrutinee.
    const auto verification = Verifier().verify(switchModule(5, { enumCase(5, 12), anything(4) }));
    EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch)) << dump(verification);
}

TEST(IRModelTest, SwitchPatternAcceptsEveryPatternKindOnItsType) {
    const std::vector<std::pair<const char*, Module>> valid {
        { "Int: -5", switchModule(1, { integer(-5), anything(1) }) },
        { "UInt8: 0, 255, byte(0), byte(255)", switchModule(7, {
                literal(PatternKind::integerLiteral, 7, 0), literal(PatternKind::integerLiteral, 7, 255),
                literal(PatternKind::byteLiteral, 7, 0), literal(PatternKind::byteLiteral, 7, 255), anything(7) }) },
        { "Int8: -128, 127", switchModule(8, {
                literal(PatternKind::integerLiteral, 8, -128), literal(PatternKind::integerLiteral, 8, 127),
                anything(8) }) },
        { "String: \"a\"", switchModule(6, { textual(PatternKind::stringLiteral, 6, "a"), anything(6) }) },
        { "Float64: 1.5", switchModule(9, { textual(PatternKind::floatingLiteral, 9, "1.5"), anything(9) }) },
    };
    for (const auto& [what, module] : valid) {
        const auto verification = Verifier().verify(module);
        EXPECT_TRUE(verification.succeeded()) << what << ": " << dump(verification);
    }
}

// Each module ends its cases with a wildcard, so a pattern whose kind does not fit its type is its only error.
void expectPatternKindMismatch(const std::vector<std::pair<const char*, Module>>& invalid) {
    for (const auto& [what, module] : invalid) {
        const auto verification = Verifier().verify(module);
        EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch)) << what << ": " << dump(verification);
    }
}

TEST(IRModelTest, SwitchPatternRejectsAnIntegerLiteralOffAnIntegerTypeOrOutOfRange) {
    // The backend compares an integer literal with `icmp eq` on the scrutinee: an enum is a struct, and a
    // value outside the type is truncated to one that another literal also names.
    expectPatternKindMismatch({
        { "Bool?: 0", switchModule(4, { literal(PatternKind::integerLiteral, 4, 0), anything(4) }) },
        { "Bool: 1", switchModule(2, { literal(PatternKind::integerLiteral, 2, 1), anything(2) }) },
        { "String: 1", switchModule(6, { literal(PatternKind::integerLiteral, 6, 1), anything(6) }) },
        { "Float64: 1", switchModule(9, { literal(PatternKind::integerLiteral, 9, 1), anything(9) }) },
        { "UInt8: 256", switchModule(7, { literal(PatternKind::integerLiteral, 7, 256), anything(7) }) },
        { "UInt8: -1", switchModule(7, { literal(PatternKind::integerLiteral, 7, -1), anything(7) }) },
        { "Int8: 128", switchModule(8, { literal(PatternKind::integerLiteral, 8, 128), anything(8) }) },
        { "Int8: -129", switchModule(8, { literal(PatternKind::integerLiteral, 8, -129), anything(8) }) },
        { "Bool?: Some(1)", switchModule(4, { someBool(literal(PatternKind::integerLiteral, 2, 1)), anything(4) }) },
    });
}

TEST(IRModelTest, SwitchPatternRejectsABooleanLiteralOffBoolOrOtherThanZeroAndOne) {
    // The backend truncates a Boolean literal to its low bit, so only 0 and 1 name `false` and `true`.
    expectPatternKindMismatch({
        { "Int: true", switchModule(1, { literal(PatternKind::booleanLiteral, 1, 1), anything(1) }) },
        { "Bool?: true", switchModule(4, { literal(PatternKind::booleanLiteral, 4, 1), anything(4) }) },
        { "Bool: 2", switchModule(2, { literal(PatternKind::booleanLiteral, 2, 2), anything(2) }) },
        { "Bool: -1", switchModule(2, { literal(PatternKind::booleanLiteral, 2, -1), anything(2) }) },
    });
}

TEST(IRModelTest, SwitchPatternRejectsAStringLiteralOffString) {
    // The backend compares a string literal with the runtime's string equality, which reads a `String`.
    expectPatternKindMismatch({
        { "Int: \"a\"", switchModule(1, { textual(PatternKind::stringLiteral, 1, "a"), anything(1) }) },
        { "UInt8: \"a\"", switchModule(7, { textual(PatternKind::stringLiteral, 7, "a"), anything(7) }) },
        { "Bool?: \"a\"", switchModule(4, { textual(PatternKind::stringLiteral, 4, "a"), anything(4) }) },
    });
}

TEST(IRModelTest, SwitchPatternRejectsAByteLiteralOffUInt8OrOutOfRange) {
    expectPatternKindMismatch({
        { "Int: byte(1)", switchModule(1, { literal(PatternKind::byteLiteral, 1, 1), anything(1) }) },
        { "Int8: byte(1)", switchModule(8, { literal(PatternKind::byteLiteral, 8, 1), anything(8) }) },
        { "UInt8: byte(256)", switchModule(7, { literal(PatternKind::byteLiteral, 7, 256), anything(7) }) },
        { "UInt8: byte(-1)", switchModule(7, { literal(PatternKind::byteLiteral, 7, -1), anything(7) }) },
    });
}

TEST(IRModelTest, SwitchPatternRejectsAFloatingLiteralOffAFloatingType) {
    expectPatternKindMismatch({
        { "Int: 1.5", switchModule(1, { textual(PatternKind::floatingLiteral, 1, "1.5"), anything(1) }) },
        { "Float64: x", switchModule(9, { textual(PatternKind::floatingLiteral, 9, "x"), anything(9) }) },
    });
}

TEST(IRModelTest, SwitchPatternRejectsAnEnumCaseOffAnEnumeration) {
    expectPatternKindMismatch({
        { "Int: Some(_)", switchModule(1, { enumCase(1, 11, { anything(2) }), anything(1) }) },
        { "Bool: None", switchModule(2, { enumCase(2, 12), anything(2) }) },
    });
}

TEST(IRModelTest, VerificationErrorSpanIsTheFailingInstructionsSpan) {
    auto module = switchModule(1, { textual(PatternKind::stringLiteral, 1, "a"), anything(1) });
    module.functions[1].blocks[0].instructions[0].span = SourceSpan { 30, 5, 0 };
    const auto verification = Verifier().verify(module);
    ASSERT_EQ(verification.errors.size(), 1u) << dump(verification);
    const auto span = verificationErrorSpan(module, verification.errors[0]);
    EXPECT_EQ(span.offset, 30u);
    EXPECT_EQ(span.length, 5u);

    // An error that names no instruction, or one the module does not have, has no span.
    const std::vector<VerificationError> elsewhere {
        { VerificationErrorId::duplicateId, std::nullopt, std::nullopt, std::nullopt, "" },
        { VerificationErrorId::missingTerminator, FunctionId { 1 }, BlockId { 0 }, std::nullopt, "" },
        { VerificationErrorId::typeMismatch, FunctionId { 1 }, BlockId { 9 }, size_t { 0 }, "" },
        { VerificationErrorId::typeMismatch, FunctionId { 1 }, BlockId { 0 }, size_t { 9 }, "" },
        { VerificationErrorId::typeMismatch, FunctionId { 9 }, BlockId { 0 }, size_t { 0 }, "" },
    };
    for (const auto& error : elsewhere) {
        const auto none = verificationErrorSpan(module, error);
        EXPECT_EQ(none.offset, 0u);
        EXPECT_EQ(none.length, 0u);
    }
}

} // namespace
