// The opcode descriptor table (decision 0028): one row per live opcode, pinned here, and the verifier and helpers
// that read it. Stage 2 merged the constant, comparison, conversion, return and type-helper families and turned the
// linear range guard into `trap_if`.

#include "compiler/backend/ir_wire.h"
#include "compiler/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace joyeer::ir;

constexpr int variadic = variadicOperands;

struct ExpectedRow {
    Opcode opcode;
    const char* name;
    int operands;
    int targets;
    OpcodeResult result;
    OpcodeImmediate immediate;
    OpcodeModifier modifier;
    bool terminator;
    bool runtimeOnly;
};

// The wire slots no row may use: removed opcodes, kept unused until stage 4 renumbers.
const std::set<int64_t>& removedSlots() {
    static const std::set<int64_t> slots = { 1, 3, 4, 18, 19, 20, 21, 22, 23, 30, 35, 36, 37, 38, 39, 40, 41, 50, 55,
                                             67 };
    return slots;
}

// Every live opcode in declaration order.
const std::vector<ExpectedRow>& expectedRows() {
    using R = OpcodeResult;
    using I = OpcodeImmediate;
    using M = OpcodeModifier;
    static const std::vector<ExpectedRow> rows = {
        { Opcode::literal, "literal", 0, 0, R::always, I::integerLiteral, M::none, false, false },
        { Opcode::stringConstant, "string", 0, 0, R::always, I::stringLiteral, M::none, false, false },
        { Opcode::stackAllocate, "alloc_stack", 0, 0, R::always, I::none, M::none, false, false },
        { Opcode::zeroInitialize, "zero_init", 1, 0, R::none, I::none, M::none, false, false },
        { Opcode::load, "load", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::store, "store", 2, 0, R::none, I::none, M::none, false, false },
        { Opcode::copyValue, "copy", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::take, "take", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::destroy, "destroy", 1, 0, R::none, I::none, M::none, false, false },
        { Opcode::add, "add", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::subtract, "sub", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::multiply, "mul", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::divide, "div", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::remainder, "rem", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::compare, "compare", 2, 0, R::always, I::none, M::comparePredicate, false, false },
        { Opcode::bitAnd, "bit_and", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::bitOr, "bit_or", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::bitXor, "bit_xor", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::shiftLeft, "shl", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::shiftRight, "shr", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::convert, "convert", 1, 0, R::always, I::none, M::conversionMode, false, false },
        { Opcode::constantValue, "constant", 0, 0, R::always, I::constantIndex, M::none, false, false },
        { Opcode::integerFits, "fits", 1, 0, R::always, I::type, M::none, false, false },
        { Opcode::call, "call", variadic, 0, R::directCallee, I::none, M::none, false, false },
        { Opcode::constructStruct, "construct_struct", variadic, 0, R::always, I::none, M::none, false, false },
        { Opcode::fieldAddress, "field_addr", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::extractField, "extract_field", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::constructEnum, "construct_enum", variadic, 0, R::always, I::none, M::none, false, false },
        { Opcode::extractPayload, "extract_payload", 1, 0, R::always, I::payloadIndex, M::none, false, false },
        { Opcode::payloadAddress, "payload_address", 1, 0, R::always, I::payloadIndex, M::none, false, false },
        { Opcode::stringUtf8, "string_utf8", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::count, "count", 1, 0, R::always, I::none, M::none, false, false },
        { Opcode::subscript, "subscript", 2, 0, R::always, I::none, M::none, false, false },
        { Opcode::branch, "br", 0, 1, R::none, I::none, M::none, true, false },
        { Opcode::conditionalBranch, "cond_br", 1, 2, R::none, I::none, M::none, true, false },
        { Opcode::switchPattern, "switch_pattern", 1, 0, R::none, I::none, M::none, true, false },
        { Opcode::ret, "ret", variadic, 0, R::none, I::none, M::none, true, false },
        { Opcode::unreachable, "unreachable", 0, 0, R::none, I::none, M::none, true, false },
        { Opcode::floatingConstant, "floating", 0, 0, R::always, I::floatingLiteral, M::none, false, false },
        { Opcode::functionAddress, "function_address", 0, 0, R::always, I::none, M::none, false, false },
        { Opcode::callIndirect, "call_indirect", variadic, 0, R::functionType, I::none, M::none, false, false },
        { Opcode::typeSize, "type_size", 0, 0, R::always, I::type, M::none, false, true },
        { Opcode::typeAlignment, "type_alignment", 0, 0, R::always, I::type, M::none, false, true },
        { Opcode::integerAddress, "integer_address", 1, 0, R::always, I::none, M::none, false, true },
        { Opcode::pointerToInteger, "pointer_to_integer", 1, 0, R::always, I::none, M::none, false, true },
        { Opcode::destroyRange, "destroy_range", 2, 0, R::none, I::type, M::none, false, true },
        { Opcode::cloneAt, "clone_at", 2, 0, R::none, I::type, M::none, false, true },
        { Opcode::typeHelper, "type_helper", 0, 0, R::always, I::type, M::typeHelperKind, false, true },
        { Opcode::trapIf, "trap_if", 1, 0, R::none, I::message, M::none, false, true },
        { Opcode::slotAddress, "slot_address", 2, 0, R::always, I::type, M::none, false, true },
        { Opcode::stackBlock, "stack_block", 1, 0, R::always, I::type, M::none, false, true },
    };
    return rows;
}

TEST(OpcodeDescriptorTest, EveryLiveOpcodeHasItsPinnedRow) {
    const auto rows = opcodeDescriptors();
    const auto& expected = expectedRows();
    ASSERT_EQ(rows.size(), expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& row = rows[index];
        const auto& want = expected[index];
        SCOPED_TRACE(want.name);
        EXPECT_EQ(row.opcode, want.opcode);
        EXPECT_STREQ(row.name, want.name);
        EXPECT_EQ(row.operands, want.operands);
        EXPECT_EQ(row.targets, want.targets);
        EXPECT_EQ(row.result, want.result);
        EXPECT_EQ(row.immediate, want.immediate);
        EXPECT_EQ(row.modifier, want.modifier);
        EXPECT_EQ(row.terminator, want.terminator);
        EXPECT_EQ(row.runtimeOnly, want.runtimeOnly);
        EXPECT_EQ(findOpcodeDescriptor(want.opcode), &row);
    }
}

TEST(OpcodeDescriptorTest, MergedOpcodesKeepTheFirstSlotOfTheirFamily) {
    EXPECT_EQ(static_cast<int>(Opcode::literal), 0);
    EXPECT_EQ(static_cast<int>(Opcode::compare), 17);
    EXPECT_EQ(static_cast<int>(Opcode::convert), 29);
    EXPECT_EQ(static_cast<int>(Opcode::ret), 54);
    EXPECT_EQ(static_cast<int>(Opcode::typeHelper), 66);
    EXPECT_EQ(static_cast<int>(Opcode::trapIf), 68);
}

TEST(OpcodeDescriptorTest, OpcodesAndSpellingsAreUnique) {
    std::set<Opcode> opcodes;
    std::set<std::string> names;
    for (const auto& row : opcodeDescriptors()) {
        EXPECT_TRUE(opcodes.insert(row.opcode).second) << row.name;
        EXPECT_TRUE(names.insert(row.name).second) << row.name;
    }
}

TEST(OpcodeDescriptorTest, HelpersReadTheTable) {
    for (const auto& row : opcodeDescriptors()) {
        SCOPED_TRACE(row.name);
        EXPECT_STREQ(opcodeName(row.opcode), row.name);
        EXPECT_EQ(isTerminator(row.opcode), row.terminator);
        EXPECT_EQ(producesValue(row.opcode), row.result == OpcodeResult::always);
        if (row.terminator) {
            EXPECT_EQ(row.result, OpcodeResult::none);
            EXPECT_EQ(row.immediate, OpcodeImmediate::none);
        }
    }
}

TEST(OpcodeDescriptorTest, ModifiersHaveSpellings) {
    EXPECT_EQ(modifierCount(OpcodeModifier::none), 0u);
    EXPECT_EQ(modifierCount(OpcodeModifier::comparePredicate), 6u);
    EXPECT_EQ(modifierCount(OpcodeModifier::conversionMode), 2u);
    EXPECT_EQ(modifierCount(OpcodeModifier::typeHelperKind), 2u);
    const std::vector<std::string> predicates = { "lt", "le", "gt", "ge", "eq", "ne" };
    for (size_t index = 0; index < predicates.size(); ++index) {
        EXPECT_EQ(modifierName(OpcodeModifier::comparePredicate, static_cast<uint8_t>(index)), predicates[index]);
    }
    EXPECT_EQ(static_cast<uint8_t>(ComparePredicate::notEqual), 5u);
    EXPECT_STREQ(modifierName(OpcodeModifier::conversionMode, static_cast<uint8_t>(ConversionMode::checked)),
                 "checked");
    EXPECT_STREQ(modifierName(OpcodeModifier::conversionMode, static_cast<uint8_t>(ConversionMode::wrapping)),
                 "wrapping");
    EXPECT_STREQ(modifierName(OpcodeModifier::typeHelperKind, static_cast<uint8_t>(TypeHelperKind::clone)), "clone");
    EXPECT_STREQ(modifierName(OpcodeModifier::typeHelperKind, static_cast<uint8_t>(TypeHelperKind::destroy)),
                 "destroy");
    EXPECT_EQ(modifierName(OpcodeModifier::comparePredicate, 6), nullptr);
    EXPECT_EQ(modifierName(OpcodeModifier::none, 0), nullptr);
}

TEST(OpcodeDescriptorTest, RemovedAndOutOfRangeSlotsHaveNoRow) {
    std::vector<int64_t> slots(removedSlots().begin(), removedSlots().end());
    slots.push_back(-1);
    slots.push_back(1000);
    slots.push_back(static_cast<int64_t>(Opcode::stackBlock) + 1);
    for (const auto slot : slots) {
        SCOPED_TRACE(slot);
        const auto opcode = static_cast<Opcode>(slot);
        EXPECT_EQ(findOpcodeDescriptor(opcode), nullptr);
        EXPECT_STREQ(opcodeName(opcode), "unknown");
        EXPECT_FALSE(isTerminator(opcode));
        EXPECT_FALSE(producesValue(opcode));
    }
}

TEST(OpcodeDescriptorTest, EverySlotUpToTheLastHasARowUnlessRemoved) {
    for (int64_t slot = 0; slot <= static_cast<int64_t>(Opcode::stackBlock); ++slot) {
        SCOPED_TRACE(slot);
        EXPECT_EQ(findOpcodeDescriptor(static_cast<Opcode>(slot)) != nullptr, !removedSlots().contains(slot));
    }
}

// Types: 0 Void, 1 Int, 2 Bool, 3 UInt8, 4 Float64, 5 String, 6 Int8.
std::vector<TypeName> testTypes() {
    return {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "Int", joyeer::typing::TypeKind::integer },
        TypeName { 2, "Bool", joyeer::typing::TypeKind::boolean },
        TypeName { 3, "UInt8", joyeer::typing::TypeKind::uint8 },
        TypeName { 4, "Float64", joyeer::typing::TypeKind::float64 },
        TypeName { 5, "String", joyeer::typing::TypeKind::string },
        TypeName { 6, "Int8", joyeer::typing::TypeKind::int8 },
    };
}

// A function `f(left: Int, right: Int) -> Int` whose entry block is `instructions` followed by `ret left`.
Module moduleWith(std::vector<Instruction> instructions, bool runtimeContext = false) {
    Module module;
    module.sourceName = "manual.joyeer";
    module.types = testTypes();
    Function function;
    function.id = 0;
    function.name = "f";
    function.parameters = {
        Parameter { Value { 0, 1, ValueCategory::value }, std::nullopt, "left", false, {} },
        Parameter { Value { 1, 1, ValueCategory::value }, std::nullopt, "right", false, {} },
    };
    function.resultType = 1;
    function.returnsValue = true;
    function.runtimeContext = runtimeContext;
    function.entry = 0;
    for (size_t index = 0; index < instructions.size(); ++index) {
        instructions[index].span = SourceSpan { static_cast<uint32_t>(10 + index), 1 };
    }
    Instruction ret;
    ret.opcode = Opcode::ret;
    ret.operands = { 0 };
    ret.span = SourceSpan { 2, 1 };
    instructions.push_back(std::move(ret));
    function.blocks = { BasicBlock { 0, "entry", std::move(instructions) } };
    module.functions.push_back(std::move(function));
    return module;
}

Module moduleWith(Instruction instruction, bool runtimeContext = false) {
    return moduleWith(std::vector<Instruction> { std::move(instruction) }, runtimeContext);
}

size_t errorsContaining(const VerificationResult& result, const std::string& text) {
    return static_cast<size_t>(std::count_if(result.errors.begin(), result.errors.end(), [&](const auto& error) {
        return error.message.find(text) != std::string::npos;
    }));
}

Instruction literal(ValueId id, TypeId type, int64_t value) {
    Instruction instruction;
    instruction.opcode = Opcode::literal;
    instruction.result = Value { id, type, ValueCategory::value };
    instruction.integerValue = value;
    return instruction;
}

TEST(OpcodeDescriptorTest, VerifierReportsAFixedShapeMismatchOnce) {
    Instruction load;
    load.opcode = Opcode::load;
    load.result = Value { 2, 1, ValueCategory::value };
    load.operands = { 0, 1 };
    const auto result = Verifier().verify(moduleWith(load));
    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(errorsContaining(result, "expects"), 1u) << dump(result);
    EXPECT_EQ(errorsContaining(result, "load expects 1 operand(s) and 0 target(s)"), 1u) << dump(result);
}

TEST(OpcodeDescriptorTest, VerifierReportsATargetMismatchFromTheTable) {
    Instruction add;
    add.opcode = Opcode::add;
    add.result = Value { 2, 1, ValueCategory::value };
    add.operands = { 0, 1 };
    add.targets = { 0 };
    const auto result = Verifier().verify(moduleWith(add));
    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(errorsContaining(result, "add expects 2 operand(s) and 0 target(s)"), 1u) << dump(result);
}

TEST(OpcodeDescriptorTest, VerifierSkipsShapeChecksForUnknownOpcodes) {
    Instruction unknown;
    unknown.opcode = static_cast<Opcode>(23);
    unknown.operands = { 0, 1 };
    const auto result = Verifier().verify(moduleWith(unknown));
    EXPECT_EQ(errorsContaining(result, "unknown or removed opcode"), 1u) << dump(result);
    EXPECT_EQ(errorsContaining(result, "expects"), 0u) << dump(result);
}

TEST(OpcodeDescriptorTest, VerifierReportsTheResultRuleBeforeTheShape) {
    Instruction load;
    load.opcode = Opcode::load;
    load.operands = { 0, 1 };
    const auto result = Verifier().verify(moduleWith(load));
    const auto position = [&](const std::string& text) {
        return std::find_if(result.errors.begin(), result.errors.end(), [&](const auto& error) {
            return error.message.find(text) != std::string::npos;
        }) - result.errors.begin();
    };
    ASSERT_EQ(errorsContaining(result, "load must define a result"), 1u) << dump(result);
    ASSERT_EQ(errorsContaining(result, "load expects 1 operand(s)"), 1u) << dump(result);
    EXPECT_LT(position("load must define a result"), position("load expects 1 operand(s)"));
}

TEST(OpcodeDescriptorTest, VariadicOpcodesKeepTheirOwnTargetCheck) {
    Instruction call;
    call.opcode = Opcode::call;
    call.callee = 0;
    call.result = Value { 2, 1, ValueCategory::value };
    call.operands = { 0, 1 };
    call.targets = { 0 };
    const auto result = Verifier().verify(moduleWith(call));
    EXPECT_EQ(errorsContaining(result, "call must not contain block targets"), 1u) << dump(result);
    EXPECT_EQ(errorsContaining(result, "expects"), 0u) << dump(result);
}

TEST(OpcodeDescriptorTest, ModifiersAreCheckedAgainstTheRow) {
    Instruction compare;
    compare.opcode = Opcode::compare;
    compare.result = Value { 2, 2, ValueCategory::value };
    compare.operands = { 0, 1 };
    compare.modifier = static_cast<uint8_t>(ComparePredicate::notEqual);
    const auto valid = Verifier().verify(moduleWith(compare));
    EXPECT_TRUE(valid.succeeded()) << dump(valid);

    compare.modifier = 6;
    const auto outOfRange = Verifier().verify(moduleWith(compare));
    EXPECT_EQ(errorsContaining(outOfRange, "compare has an invalid modifier 6"), 1u) << dump(outOfRange);

    Instruction add;
    add.opcode = Opcode::add;
    add.result = Value { 2, 1, ValueCategory::value };
    add.operands = { 0, 1 };
    add.modifier = 1;
    const auto stray = Verifier().verify(moduleWith(add));
    EXPECT_EQ(errorsContaining(stray, "add has an invalid modifier 1"), 1u) << dump(stray);
    EXPECT_NE(dump(moduleWith(add)).find("add modifier#1 %0, %1"), std::string::npos) << dump(moduleWith(add));

    Instruction convert;
    convert.opcode = Opcode::convert;
    convert.result = Value { 2, 3, ValueCategory::value };
    convert.operands = { 0 };
    convert.modifier = 2;
    const auto badMode = Verifier().verify(moduleWith(convert));
    EXPECT_EQ(errorsContaining(badMode, "convert has an invalid modifier 2"), 1u) << dump(badMode);

    Instruction helper;
    helper.opcode = Opcode::typeHelper;
    helper.result = Value { 2, 1, ValueCategory::value };
    helper.integerValue = 1;
    helper.modifier = 2;
    const auto badKind = Verifier().verify(moduleWith(helper, true));
    EXPECT_EQ(errorsContaining(badKind, "type_helper has an invalid modifier 2"), 1u) << dump(badKind);

    auto module = moduleWith(std::vector<Instruction> {});
    module.functions[0].blocks[0].instructions[0].modifier = 1;
    const auto badReturn = Verifier().verify(module);
    EXPECT_EQ(errorsContaining(badReturn, "ret has an invalid modifier 1"), 1u) << dump(badReturn);
}

TEST(OpcodeDescriptorTest, CompareTakesScalarOrStringOperandsAndYieldsABool) {
    Instruction compare;
    compare.opcode = Opcode::compare;
    compare.result = Value { 2, 1, ValueCategory::value };
    compare.operands = { 0, 1 };
    const auto intResult = Verifier().verify(moduleWith(compare));
    EXPECT_EQ(errorsContaining(intResult, "compare takes scalar or String operands and yields a Bool"), 1u)
            << dump(intResult);

    Instruction unit = literal(2, 0, 0);
    Instruction compareUnits;
    compareUnits.opcode = Opcode::compare;
    compareUnits.result = Value { 3, 2, ValueCategory::value };
    compareUnits.operands = { 2, 2 };
    compareUnits.modifier = static_cast<uint8_t>(ComparePredicate::equal);
    const auto voidOperands = Verifier().verify(moduleWith({ unit, compareUnits }));
    EXPECT_EQ(errorsContaining(voidOperands, "compare takes scalar or String operands and yields a Bool"), 1u)
            << dump(voidOperands);
}

TEST(OpcodeDescriptorTest, OnlyIntegerLiteralsCountForStackBlocks) {
    Instruction count = literal(2, 2, 1);
    Instruction block;
    block.opcode = Opcode::stackBlock;
    block.result = Value { 3, 1, ValueCategory::value };
    block.operands = { 2 };
    block.integerValue = 1;
    const auto boolCount = Verifier().verify(moduleWith({ count, block }, true));
    EXPECT_EQ(errorsContaining(boolCount, "stack_block takes a non-negative constant count"), 1u) << dump(boolCount);

    const auto intCount = Verifier().verify(moduleWith({ literal(2, 1, 1), block }, true));
    EXPECT_TRUE(intCount.succeeded()) << dump(intCount);
}

TEST(OpcodeDescriptorTest, LiteralsFitTheirResultType) {
    const auto valid = Verifier().verify(moduleWith({
        literal(2, 1, -5), literal(3, 0, 0), literal(4, 2, 1), literal(5, 3, 255),
    }));
    EXPECT_TRUE(valid.succeeded()) << dump(valid);
    for (const auto& invalid : { literal(2, 0, 1), literal(2, 2, 2), literal(2, 3, 256), literal(2, 3, -1),
                                 literal(2, 6, -129), literal(2, 6, 128), literal(2, 4, 0), literal(2, 5, 0) }) {
        const auto result = Verifier().verify(moduleWith(invalid));
        EXPECT_EQ(errorsContaining(result, "literal kind or value does not match its result type"), 1u)
                << dump(result);
    }
}

TEST(OpcodeDescriptorTest, RetMatchesTheFunctionResult) {
    auto module = moduleWith(std::vector<Instruction> {});
    module.functions[0].blocks[0].instructions[0].operands.clear();
    const auto missing = Verifier().verify(module);
    EXPECT_EQ(errorsContaining(missing, "value-returning function cannot return void"), 1u) << dump(missing);

    module.functions[0].returnsValue = false;
    module.functions[0].resultType = 0;
    const auto voidReturn = Verifier().verify(module);
    EXPECT_TRUE(voidReturn.succeeded()) << dump(voidReturn);

    module.functions[0].blocks[0].instructions[0].operands = { 0 };
    const auto extra = Verifier().verify(module);
    EXPECT_EQ(errorsContaining(extra, "return value does not match function result type"), 1u) << dump(extra);

    module.functions[0].returnsValue = true;
    module.functions[0].resultType = 1;
    module.functions[0].blocks[0].instructions[0].operands = { 0, 1 };
    const auto twice = Verifier().verify(module);
    EXPECT_EQ(errorsContaining(twice, "ret takes one operand in a value-returning function and none otherwise"), 1u)
            << dump(twice);
}

TEST(OpcodeDescriptorTest, TrapIfTakesABoolConditionAndAMessage) {
    Instruction condition;
    condition.opcode = Opcode::compare;
    condition.result = Value { 2, 2, ValueCategory::value };
    condition.operands = { 0, 1 };
    condition.modifier = static_cast<uint8_t>(ComparePredicate::notEqual);
    Instruction trap;
    trap.opcode = Opcode::trapIf;
    trap.operands = { 2 };
    trap.text = "linear container dropped while not empty";
    const auto valid = Verifier().verify(moduleWith({ condition, trap }, true));
    EXPECT_TRUE(valid.succeeded()) << dump(valid);

    auto silent = trap;
    silent.text.clear();
    const auto noMessage = Verifier().verify(moduleWith({ condition, silent }, true));
    EXPECT_EQ(errorsContaining(noMessage, "trap_if takes a Bool value and a message"), 1u) << dump(noMessage);

    auto onInt = trap;
    onInt.operands = { 0 };
    const auto intCondition = Verifier().verify(moduleWith(onInt, true));
    EXPECT_EQ(errorsContaining(intCondition, "trap_if takes a Bool value and a message"), 1u) << dump(intCondition);

    auto unknown = trap;
    unknown.operands = { 99 };
    const auto unknownCondition = Verifier().verify(moduleWith(unknown, true));
    EXPECT_EQ(errorsContaining(unknownCondition, "trap_if takes"), 0u) << dump(unknownCondition);

    auto nul = trap;
    nul.text = std::string("bad\0message", 11);
    const auto nulMessage = Verifier().verify(moduleWith({ condition, nul }, true));
    EXPECT_EQ(errorsContaining(nulMessage, "trap_if message must not contain a NUL byte"), 1u) << dump(nulMessage);

    auto stray = trap;
    stray.modifier = 1;
    const auto badModifier = Verifier().verify(moduleWith({ condition, stray }, true));
    EXPECT_EQ(errorsContaining(badModifier, "trap_if has an invalid modifier 1"), 1u) << dump(badModifier);
}

TEST(OpcodeDescriptorTest, RuntimeOnlyOpcodesNeedRuntimeAuthorization) {
    Instruction size;
    size.opcode = Opcode::typeSize;
    size.result = Value { 2, 1, ValueCategory::value };
    size.integerValue = 1;
    const auto denied = Verifier().verify(moduleWith(size));
    EXPECT_EQ(errorsContaining(denied, "requires runtime authorization"), 1u) << dump(denied);
    const auto allowed = Verifier().verify(moduleWith(size, true));
    EXPECT_TRUE(allowed.succeeded()) << dump(allowed);

    Instruction helper;
    helper.opcode = Opcode::typeHelper;
    helper.result = Value { 2, 1, ValueCategory::value };
    helper.integerValue = 1;
    helper.modifier = static_cast<uint8_t>(TypeHelperKind::destroy);
    const auto helperDenied = Verifier().verify(moduleWith(helper));
    EXPECT_EQ(errorsContaining(helperDenied, "requires runtime authorization"), 1u) << dump(helperDenied);
    const auto helperAllowed = Verifier().verify(moduleWith(helper, true));
    EXPECT_TRUE(helperAllowed.succeeded()) << dump(helperAllowed);
}

TEST(OpcodeDescriptorTest, TheWireCarriesModifiersAndTrapMessages) {
    Instruction compare;
    compare.opcode = Opcode::compare;
    compare.result = Value { 2, 2, ValueCategory::value };
    compare.operands = { 0, 1 };
    compare.modifier = static_cast<uint8_t>(ComparePredicate::greaterEqual);
    Instruction convert;
    convert.opcode = Opcode::convert;
    convert.result = Value { 3, 3, ValueCategory::value };
    convert.operands = { 0 };
    convert.modifier = static_cast<uint8_t>(ConversionMode::wrapping);
    Instruction helper;
    helper.opcode = Opcode::typeHelper;
    helper.result = Value { 4, 1, ValueCategory::value };
    helper.integerValue = 1;
    helper.modifier = static_cast<uint8_t>(TypeHelperKind::destroy);
    Instruction trap;
    trap.opcode = Opcode::trapIf;
    trap.operands = { 2 };
    trap.text = "boom";
    const auto module = moduleWith({ compare, convert, helper, trap }, true);
    ASSERT_TRUE(Verifier().verify(module).succeeded()) << dump(Verifier().verify(module));

    joyeer::llvmbackend::wire::Writer writer;
    writer(module);
    Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    const auto& before = module.functions[0].blocks[0].instructions;
    const auto& after = decoded.functions[0].blocks[0].instructions;
    ASSERT_EQ(after.size(), before.size());
    for (size_t index = 0; index < before.size(); ++index) {
        EXPECT_EQ(after[index].opcode, before[index].opcode) << index;
        EXPECT_EQ(after[index].modifier, before[index].modifier) << index;
        EXPECT_EQ(after[index].text, before[index].text) << index;
    }
    EXPECT_EQ(dump(decoded), dump(module));
}

// Each runtime-only row is enforced by its opcode's check, outside verified literal storage.
TEST(OpcodeDescriptorTest, EveryRuntimeOnlyOpcodeNeedsRuntimeAuthorization) {
    for (const auto& row : opcodeDescriptors()) {
        if (!row.runtimeOnly) continue;
        SCOPED_TRACE(row.name);
        Instruction instruction;
        instruction.opcode = row.opcode;
        ASSERT_GE(row.operands, 0);
        ASSERT_LE(row.operands, 2);
        for (int operand = 0; operand < row.operands; ++operand) {
            instruction.operands.push_back(static_cast<ValueId>(operand));
        }
        if (row.result == OpcodeResult::always) instruction.result = Value { 2, 1, ValueCategory::value };
        instruction.integerValue = 1;
        instruction.text = "message";
        const auto result = Verifier().verify(moduleWith(instruction));
        EXPECT_EQ(errorsContaining(result, "requires runtime authorization"), 1u) << dump(result);
    }
}

// The dump text of every opcode the dumper prints through its generic path, and of `fits`.
TEST(OpcodeDescriptorTest, DumpTextSpellsImmediatesAndModifiers) {
    Module module;
    module.sourceName = "manual.joyeer";
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "Int", joyeer::typing::TypeKind::integer },
        TypeName { 2, "UInt8", joyeer::typing::TypeKind::uint8 },
        TypeName { 3, "Bool", joyeer::typing::TypeKind::boolean },
        TypeName { 4, "String", joyeer::typing::TypeKind::string },
        TypeName { 5, "Float64", joyeer::typing::TypeKind::float64 },
        TypeName { 6, "UInt64", joyeer::typing::TypeKind::uint64 },
    };
    Function function;
    function.id = 0;
    function.name = "f";
    function.parameters = {
        Parameter { Value { 0, 1, ValueCategory::value }, std::nullopt, "left", false, {} },
        Parameter { Value { 1, 1, ValueCategory::value }, std::nullopt, "right", false, {} },
    };
    function.resultType = 1;
    function.returnsValue = true;
    function.entry = 0;

    struct Case {
        Opcode opcode;
        std::optional<Value> result;
        std::vector<ValueId> operands;
        int64_t integerValue;
        std::string text;
        std::optional<joyeer::semantic::SymbolId> symbol;
        uint8_t modifier;
        std::string expected;
    };
    const auto value = [](ValueId id, TypeId type) { return std::optional(Value { id, type, ValueCategory::value }); };
    const auto address = [](ValueId id, TypeId type) {
        return std::optional(Value { id, type, ValueCategory::address });
    };
    const std::optional<Value> none;
    const std::optional<joyeer::semantic::SymbolId> noSymbol;
    const auto lt = static_cast<uint8_t>(ComparePredicate::less);
    const auto ne = static_cast<uint8_t>(ComparePredicate::notEqual);
    const auto checked = static_cast<uint8_t>(ConversionMode::checked);
    const auto wrapping = static_cast<uint8_t>(ConversionMode::wrapping);
    const auto clone = static_cast<uint8_t>(TypeHelperKind::clone);
    const auto destroy = static_cast<uint8_t>(TypeHelperKind::destroy);
    const std::vector<Case> cases = {
        { Opcode::literal, value(10, 1), {}, -5, "", noSymbol, 0, "%10: Int = literal -5" },
        { Opcode::literal, value(11, 6), {}, -1, "", noSymbol, 0, "%11: UInt64 = literal 18446744073709551615" },
        { Opcode::literal, value(12, 3), {}, 1, "", noSymbol, 0, "%12: Bool = literal 1" },
        { Opcode::literal, value(13, 2), {}, 200, "", noSymbol, 0, "%13: UInt8 = literal 200" },
        { Opcode::stringConstant, value(14, 4), {}, 0, "hi", noSymbol, 0, "%14: String = string \"hi\"" },
        { Opcode::floatingConstant, value(15, 5), {}, 0, "1.5", noSymbol, 0, "%15: Float64 = floating 1.5" },
        { Opcode::literal, value(16, 0), {}, 0, "", noSymbol, 0, "%16: Void = literal 0" },
        { Opcode::constantValue, value(17, 1), {}, 0, "", noSymbol, 0, "%17: Int = constant #0" },
        { Opcode::stackAllocate, address(18, 1), {}, 0, "", noSymbol, 0, "%18: &Int = alloc_stack" },
        { Opcode::zeroInitialize, none, { 18 }, 0, "", noSymbol, 0, "zero_init %18" },
        { Opcode::load, value(19, 1), { 18 }, 0, "", noSymbol, 0, "%19: Int = load %18" },
        { Opcode::store, none, { 0, 18 }, 0, "", noSymbol, 0, "store %0, %18" },
        { Opcode::copyValue, value(20, 1), { 0 }, 0, "", noSymbol, 0, "%20: Int = copy %0" },
        { Opcode::take, value(21, 1), { 18 }, 0, "", noSymbol, 0, "%21: Int = take %18" },
        { Opcode::destroy, none, { 18 }, 0, "", noSymbol, 0, "destroy %18" },
        { Opcode::add, value(22, 1), { 0, 1 }, 0, "", noSymbol, 0, "%22: Int = add %0, %1" },
        { Opcode::compare, value(23, 3), { 0, 1 }, 0, "", noSymbol, lt, "%23: Bool = compare lt %0, %1" },
        { Opcode::shiftLeft, value(24, 1), { 0, 1 }, 0, "", noSymbol, 0, "%24: Int = shl %0, %1" },
        { Opcode::convert, value(25, 2), { 0 }, 0, "", noSymbol, checked, "%25: UInt8 = convert checked %0" },
        { Opcode::integerFits, value(26, 3), { 0 }, 2, "", noSymbol, 0, "%26: Bool = fits %0 in type 2" },
        { Opcode::fieldAddress, address(27, 1), { 18 }, 0, "", 7, 0, "%27: &Int = field_addr %18 symbol#7" },
        { Opcode::extractPayload, value(28, 1), { 0 }, 1, "", 9, 0, "%28: Int = extract_payload %0, 1 symbol#9" },
        { Opcode::payloadAddress, address(29, 1), { 18 }, 0, "", 9, 0,
          "%29: &Int = payload_address %18, 0 symbol#9" },
        { Opcode::stringUtf8, value(30, 1), { 14 }, 0, "", noSymbol, 0, "%30: Int = string_utf8 %14" },
        { Opcode::count, value(31, 1), { 14 }, 0, "", noSymbol, 0, "%31: Int = count %14" },
        { Opcode::subscript, value(32, 2), { 14, 0 }, 0, "", noSymbol, 0, "%32: UInt8 = subscript %14, %0" },
        { Opcode::typeSize, value(33, 1), {}, 1, "", noSymbol, 0, "%33: Int = type_size of type 1" },
        { Opcode::integerAddress, address(34, 1), { 0 }, 0, "", noSymbol, 0, "%34: &Int = integer_address %0" },
        { Opcode::pointerToInteger, value(35, 1), { 18 }, 0, "", noSymbol, 0, "%35: Int = pointer_to_integer %18" },
        { Opcode::destroyRange, none, { 0, 1 }, 1, "", noSymbol, 0, "destroy_range of type 1 %0, %1" },
        { Opcode::cloneAt, none, { 0, 1 }, 1, "", noSymbol, 0, "clone_at of type 1 %0, %1" },
        { Opcode::typeHelper, value(36, 1), {}, 1, "", noSymbol, clone, "%36: Int = type_helper clone of type 1" },
        { Opcode::trapIf, none, { 23 }, 0, "boom", noSymbol, 0, "trap_if %23, \"boom\"" },
        { Opcode::slotAddress, value(37, 1), { 0, 1 }, 1, "", noSymbol, 0,
          "%37: Int = slot_address of type 1 %0, %1" },
        { Opcode::stackBlock, value(38, 1), { 0 }, 1, "", noSymbol, 0, "%38: Int = stack_block of type 1 %0" },
        { Opcode::compare, value(39, 3), { 0, 1 }, 0, "", noSymbol, ne, "%39: Bool = compare ne %0, %1" },
        { Opcode::convert, value(40, 2), { 0 }, 0, "", noSymbol, wrapping, "%40: UInt8 = convert wrapping %0" },
        { Opcode::typeHelper, value(41, 1), {}, 1, "", noSymbol, destroy,
          "%41: Int = type_helper destroy of type 1" },
        { Opcode::ret, none, { 0 }, 0, "", noSymbol, 0, "ret %0" },
        { Opcode::ret, none, {}, 0, "", noSymbol, 0, "ret" },
        { Opcode::unreachable, none, {}, 0, "", noSymbol, 0, "unreachable" },
    };
    BasicBlock block { 0, "entry", {} };
    for (size_t index = 0; index < cases.size(); ++index) {
        Instruction instruction;
        instruction.opcode = cases[index].opcode;
        instruction.result = cases[index].result;
        instruction.operands = cases[index].operands;
        instruction.integerValue = cases[index].integerValue;
        instruction.text = cases[index].text;
        instruction.symbol = cases[index].symbol;
        instruction.modifier = cases[index].modifier;
        instruction.span = SourceSpan { static_cast<uint32_t>(100 + index), 1 };
        block.instructions.push_back(std::move(instruction));
    }
    function.blocks = { std::move(block) };
    module.functions.push_back(std::move(function));

    const auto text = dump(module);
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto line = "      " + cases[index].expected + " @" + std::to_string(100 + index) + ":1\n";
        EXPECT_NE(text.find(line), std::string::npos) << "missing: " << line << "in:\n" << text;
    }
}

} // namespace
