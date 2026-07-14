#include "joyeer/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

using namespace joyeer::ir;

Module validAddModule() {
    Module module;
    module.sourceName = "manual.joyeer";
    module.types = {
        TypeName { 0, "Void" },
        TypeName { 1, "Int" },
        TypeName { 2, "Bool" },
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

} // namespace
