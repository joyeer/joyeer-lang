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
                    Opcode::returnValue,
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

TEST(IRModelTest, VerifiesUnitConstantShapeAndType) {
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
        Instruction { Opcode::unitConstant, Value { 0, 0, ValueCategory::value } },
        Instruction { Opcode::returnVoid },
    } } };
    module.functions.push_back(std::move(function));
    EXPECT_TRUE(Verifier().verify(module).succeeded());
    EXPECT_NE(dump(module).find("unit"), std::string::npos);
    EXPECT_FALSE(requiresDestruction(module, 0));

    auto& unit = module.functions[0].blocks[0].instructions[0];
    unit.result->type = 1;
    EXPECT_TRUE(hasError(Verifier().verify(module), VerificationErrorId::typeMismatch));
    unit.result->type = 0;
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
            Opcode::integerConstant,
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
        Instruction { Opcode::returnVoid },
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
        Instruction { Opcode::returnVoid },
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

TEST(IRModelTest, RemovedArrayWireSlotsAreRejectedAndNeverReused) {
    EXPECT_EQ(static_cast<unsigned>(Opcode::fieldAddress), 42u);
    EXPECT_EQ(static_cast<unsigned>(Opcode::branch), 51u);
    EXPECT_EQ(static_cast<unsigned>(joyeer::typing::TypeKind::int8), 12u);
    for (unsigned slot = 35; slot <= 38; ++slot) {
        auto module = validAddModule();
        module.functions[0].blocks[0].instructions[0].opcode = static_cast<Opcode>(slot);
        const auto verification = Verifier().verify(module);
        EXPECT_FALSE(verification.succeeded());
        EXPECT_NE(dump(verification).find("unknown or removed opcode"), std::string::npos);
    }
    auto module = validAddModule();
    module.types.push_back(TypeName { 3, "removed array kind", static_cast<joyeer::typing::TypeKind>(10) });
    const auto verification = Verifier().verify(module);
    EXPECT_FALSE(verification.succeeded());
    EXPECT_NE(dump(verification).find("unknown or removed type kind"), std::string::npos);
}

// The builtin dictionary's operations (39-41), `subscript_addr` (50) and its type kind (11) were removed when the
// library `Dict` replaced it (decision 0027, wire 25); their slots stay rejected and are never reused.
TEST(IRModelTest, RemovedDictionaryWireSlotsAreRejectedAndNeverReused) {
    EXPECT_EQ(static_cast<unsigned>(Opcode::subscript), 49u);
    for (const unsigned slot : { 39u, 40u, 41u, 50u }) {
        auto module = validAddModule();
        module.functions[0].blocks[0].instructions[0].opcode = static_cast<Opcode>(slot);
        const auto verification = Verifier().verify(module);
        EXPECT_FALSE(verification.succeeded()) << slot;
        EXPECT_NE(dump(verification).find("unknown or removed opcode"), std::string::npos) << slot;
    }
    auto module = validAddModule();
    module.types.push_back(TypeName {
        3, "[Int: Bool]", static_cast<joyeer::typing::TypeKind>(11), joyeer::semantic::invalidSymbolId, { 1, 2 },
    });
    const auto verification = Verifier().verify(module);
    EXPECT_FALSE(verification.succeeded());
    EXPECT_NE(dump(verification).find("unknown or removed type kind"), std::string::npos);
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

// `subscript` and `count` are String-only since the builtin dictionary went (decision 0027): the backend reads the
// handle's fields without checking the base kind, so the verifier is their only guard.
TEST(IRModelTest, StringByteAndCountAcceptOnlyAStringValue) {
    auto module = validAddModule();
    module.types.push_back(TypeName { 3, "String", joyeer::typing::TypeKind::string });
    module.types.push_back(TypeName { 4, "UInt8", joyeer::typing::TypeKind::uint8 });
    auto& function = module.functions[0];
    function.name = "byte";
    function.parameters = {
        Parameter { Value { 0, 3, ValueCategory::value }, std::nullopt, "text", false, {} },
        Parameter { Value { 1, 1, ValueCategory::value }, std::nullopt, "index", false, {} },
    };
    function.resultType = 4;
    function.blocks[0].instructions = {
        Instruction { Opcode::count, Value { 2, 1, ValueCategory::value }, { 0 } },
        Instruction { Opcode::subscript, Value { 3, 4, ValueCategory::value }, { 0, 1 } },
        Instruction { Opcode::returnValue, std::nullopt, { 3 } },
    };
    const auto valid = Verifier().verify(module);
    ASSERT_TRUE(valid.succeeded()) << dump(valid);

    const auto rejects = [&](const char* what, const char* message, const auto& mutate) {
        auto invalid = module;
        mutate(invalid);
        const auto verification = Verifier().verify(invalid);
        EXPECT_TRUE(hasError(verification, VerificationErrorId::typeMismatch)) << what;
        EXPECT_NE(dump(verification).find(message), std::string::npos) << what << ": " << dump(verification);
    };
    constexpr auto subscriptMessage = "subscript index/result types do not match its base type";
    constexpr auto countMessage = "count requires String and produces Int";
    rejects("a subscript of a non-String base", subscriptMessage, [](Module& m) {
        m.functions[0].parameters[0].value.type = 2;
        m.functions[0].blocks[0].instructions.erase(m.functions[0].blocks[0].instructions.begin());
    });
    rejects("a subscript of an address base", subscriptMessage, [](Module& m) {
        m.functions[0].parameters[0].value.category = ValueCategory::address;
        m.functions[0].blocks[0].instructions.erase(m.functions[0].blocks[0].instructions.begin());
    });
    rejects("a subscript with a non-Int index", subscriptMessage, [](Module& m) {
        m.functions[0].parameters[1].value.type = 2;
    });
    rejects("a subscript producing a non-UInt8 result", subscriptMessage, [](Module& m) {
        m.functions[0].blocks[0].instructions[1].result->type = 1;
        m.functions[0].resultType = 1;
    });
    rejects("a subscript producing an address", subscriptMessage, [](Module& m) {
        m.functions[0].blocks[0].instructions[1].result->category = ValueCategory::address;
    });
    rejects("a subscript in a module without UInt8", subscriptMessage, [](Module& m) {
        m.types.pop_back();
        m.functions[0].blocks[0].instructions[1].result->type = 1;
        m.functions[0].resultType = 1;
    });
    rejects("a count of a non-String base", countMessage, [](Module& m) {
        m.functions[0].blocks[0].instructions[0].operands = { 1 };
    });
    rejects("a count producing a non-Int result", countMessage, [](Module& m) {
        m.functions[0].blocks[0].instructions[0].result->type = 4;
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
        Opcode::integerConstant,
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
                    Opcode::returnValue,
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
    function.blocks.push_back(terminatorBlock(static_cast<BlockId>(count * 3), Opcode::returnVoid));
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
        terminatorBlock(8, Opcode::returnVoid),
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
        terminatorBlock(6, Opcode::returnVoid),
        terminatorBlock(7, Opcode::returnVoid),
        // 8 and 9 reach each other but no root reaches them; 10 joins them with the entry.
        terminatorBlock(8, Opcode::conditionalBranch, {9, 10}),
        terminatorBlock(9, Opcode::branch, {8}),
        terminatorBlock(10, Opcode::returnVoid),
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
    function.blocks.push_back(terminatorBlock(exit, Opcode::returnVoid));
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
        terminatorBlock(4, Opcode::returnVoid),
        terminatorBlock(5, Opcode::returnVoid),
        // Switch cases count only on a switch.
        terminatorBlock(6, Opcode::branch, {}),
    };
    BasicBlock empty;
    empty.id = 7;
    function.blocks.push_back(empty);
    SwitchCase stray;
    stray.target = 8;
    function.blocks[6].instructions.back().switchCases.push_back(stray);
    function.blocks.push_back(terminatorBlock(8, Opcode::returnVoid));
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
    const Opcode terminators[] = {Opcode::branch, Opcode::conditionalBranch, Opcode::switchPattern, Opcode::returnVoid};
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
                Instruction { Opcode::returnVoid },
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

TEST(IRModelTest, VerifiesStringUtf8ProducesAByteArray) {
    Module module;
    module.sourceName = "utf8.joyeer";
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "String", joyeer::typing::TypeKind::string },
        TypeName { 2, "UInt8", joyeer::typing::TypeKind::uint8 },
        TypeName {
            3,
            "[UInt8]",
            joyeer::typing::TypeKind::structure,
            50,
            { 2 },
        },
        TypeName { 4, "Int", joyeer::typing::TypeKind::integer },
    };
    StructureDefinition array;
    array.symbol = 50;
    array.type = 3;
    array.name = "[UInt8]";
    array.role = joyeer::ir::StructureRole::array;
    array.fields = { { 51, "address", 4, true }, { 52, "count", 4, true }, { 53, "capacity", 4, true } };
    array.deinitializer = 1;
    array.copier = 2;
    module.structures.push_back(array);
    Function function;
    function.id = 0;
    function.name = "bytes";
    function.parameters = {
        Parameter { Value { 0, 1, ValueCategory::value }, std::nullopt, "text" },
    };
    function.resultType = 3;
    function.returnsValue = true;
    function.entry = 0;
    function.blocks = {
        BasicBlock {
            0,
            "entry",
            {
                Instruction {
                    Opcode::stringUtf8,
                    Value { 1, 3, ValueCategory::value },
                    { 0 },
                },
                Instruction { Opcode::returnValue, std::nullopt, { 1 } },
            },
        },
    };
    module.functions.push_back(std::move(function));
    Function destroy;
    destroy.id = 1;
    destroy.name = "destroy";
    destroy.parameters = { Parameter { Value { 0, 3, ValueCategory::address }, std::nullopt, "self", true } };
    destroy.resultType = 0;
    destroy.entry = 0;
    destroy.blocks = { BasicBlock { 0, "entry", { Instruction { Opcode::returnVoid } } } };
    module.functions.push_back(destroy);
    Function clone;
    clone.id = 2;
    clone.name = "clone";
    clone.parameters = { Parameter { Value { 0, 3, ValueCategory::value }, std::nullopt, "self" } };
    clone.resultType = 3;
    clone.returnsValue = true;
    clone.entry = 0;
    clone.blocks = { BasicBlock { 0, "entry", { Instruction { Opcode::returnValue, std::nullopt, { 0 } } } } };
    module.functions.push_back(clone);

    const auto valid = Verifier().verify(module);
    ASSERT_TRUE(valid.succeeded()) << dump(valid);
    EXPECT_NE(dump(module).find("string_utf8 %0"), std::string::npos);

    module.functions[0].blocks[0].instructions[0].result->type = 1;
    const auto invalid = Verifier().verify(module);
    EXPECT_TRUE(hasError(invalid, VerificationErrorId::typeMismatch));
}

} // namespace
