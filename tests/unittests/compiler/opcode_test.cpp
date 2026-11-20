// The opcode descriptor table (decision 0028, stage 1): one row per live opcode, pinned here, and the verifier and
// helpers that read it.

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
    bool terminator;
    bool runtimeOnly;
};

// Every live opcode in declaration order, with the shape the verifier enforced before the table existed.
const std::vector<ExpectedRow>& expectedRows() {
    using R = OpcodeResult;
    using I = OpcodeImmediate;
    static const std::vector<ExpectedRow> rows = {
        { Opcode::integerConstant, "integer", 0, 0, R::always, I::integerLiteral, false, false },
        { Opcode::booleanConstant, "boolean", 0, 0, R::always, I::integerLiteral, false, false },
        { Opcode::stringConstant, "string", 0, 0, R::always, I::stringLiteral, false, false },
        { Opcode::byteConstant, "byte", 0, 0, R::always, I::integerLiteral, false, false },
        { Opcode::unitConstant, "unit", 0, 0, R::always, I::none, false, false },
        { Opcode::stackAllocate, "alloc_stack", 0, 0, R::always, I::none, false, false },
        { Opcode::zeroInitialize, "zero_init", 1, 0, R::none, I::none, false, false },
        { Opcode::load, "load", 1, 0, R::always, I::none, false, false },
        { Opcode::store, "store", 2, 0, R::none, I::none, false, false },
        { Opcode::copyValue, "copy", 1, 0, R::always, I::none, false, false },
        { Opcode::take, "take", 1, 0, R::always, I::none, false, false },
        { Opcode::destroy, "destroy", 1, 0, R::none, I::none, false, false },
        { Opcode::add, "add", 2, 0, R::always, I::none, false, false },
        { Opcode::subtract, "sub", 2, 0, R::always, I::none, false, false },
        { Opcode::multiply, "mul", 2, 0, R::always, I::none, false, false },
        { Opcode::divide, "div", 2, 0, R::always, I::none, false, false },
        { Opcode::remainder, "rem", 2, 0, R::always, I::none, false, false },
        { Opcode::less, "lt", 2, 0, R::always, I::none, false, false },
        { Opcode::lessEqual, "le", 2, 0, R::always, I::none, false, false },
        { Opcode::greater, "gt", 2, 0, R::always, I::none, false, false },
        { Opcode::greaterEqual, "ge", 2, 0, R::always, I::none, false, false },
        { Opcode::equal, "eq", 2, 0, R::always, I::none, false, false },
        { Opcode::notEqual, "ne", 2, 0, R::always, I::none, false, false },
        { Opcode::bitAnd, "bit_and", 2, 0, R::always, I::none, false, false },
        { Opcode::bitOr, "bit_or", 2, 0, R::always, I::none, false, false },
        { Opcode::bitXor, "bit_xor", 2, 0, R::always, I::none, false, false },
        { Opcode::shiftLeft, "shl", 2, 0, R::always, I::none, false, false },
        { Opcode::shiftRight, "shr", 2, 0, R::always, I::none, false, false },
        { Opcode::convertChecked, "convert", 1, 0, R::always, I::none, false, false },
        { Opcode::convertWrapping, "convert_wrapping", 1, 0, R::always, I::none, false, false },
        { Opcode::constantValue, "constant", 0, 0, R::always, I::constantIndex, false, false },
        { Opcode::integerFits, "fits", 1, 0, R::always, I::type, false, false },
        { Opcode::call, "call", variadic, 0, R::directCallee, I::none, false, false },
        { Opcode::constructStruct, "construct_struct", variadic, 0, R::always, I::none, false, false },
        { Opcode::fieldAddress, "field_addr", 1, 0, R::always, I::none, false, false },
        { Opcode::extractField, "extract_field", 1, 0, R::always, I::none, false, false },
        { Opcode::constructEnum, "construct_enum", variadic, 0, R::always, I::none, false, false },
        { Opcode::extractPayload, "extract_payload", 1, 0, R::always, I::payloadIndex, false, false },
        { Opcode::payloadAddress, "payload_address", 1, 0, R::always, I::payloadIndex, false, false },
        { Opcode::stringUtf8, "string_utf8", 1, 0, R::always, I::none, false, false },
        { Opcode::count, "count", 1, 0, R::always, I::none, false, false },
        { Opcode::subscript, "subscript", 2, 0, R::always, I::none, false, false },
        { Opcode::branch, "br", 0, 1, R::none, I::none, true, false },
        { Opcode::conditionalBranch, "cond_br", 1, 2, R::none, I::none, true, false },
        { Opcode::switchPattern, "switch_pattern", 1, 0, R::none, I::none, true, false },
        { Opcode::returnValue, "ret", 1, 0, R::none, I::none, true, false },
        { Opcode::returnVoid, "ret_void", 0, 0, R::none, I::none, true, false },
        { Opcode::unreachable, "unreachable", 0, 0, R::none, I::none, true, false },
        { Opcode::floatingConstant, "floating", 0, 0, R::always, I::floatingLiteral, false, false },
        { Opcode::functionAddress, "function_address", 0, 0, R::always, I::none, false, false },
        { Opcode::callIndirect, "call_indirect", variadic, 0, R::functionType, I::none, false, false },
        { Opcode::typeSize, "type_size", 0, 0, R::always, I::type, false, true },
        { Opcode::typeAlignment, "type_alignment", 0, 0, R::always, I::type, false, true },
        { Opcode::integerAddress, "integer_address", 1, 0, R::always, I::none, false, true },
        { Opcode::pointerToInteger, "pointer_to_integer", 1, 0, R::always, I::none, false, true },
        { Opcode::destroyRange, "destroy_range", 2, 0, R::none, I::type, false, true },
        { Opcode::cloneAt, "clone_at", 2, 0, R::none, I::type, false, true },
        { Opcode::cloneCallback, "clone_callback", 0, 0, R::always, I::type, false, true },
        { Opcode::destroyCallback, "destroy_callback", 0, 0, R::always, I::type, false, true },
        { Opcode::linearRangeGuard, "linear_range_guard", 1, 0, R::none, I::none, false, true },
        { Opcode::slotAddress, "slot_address", 2, 0, R::always, I::type, false, true },
        { Opcode::stackBlock, "stack_block", 1, 0, R::always, I::type, false, true },
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
        EXPECT_EQ(row.terminator, want.terminator);
        EXPECT_EQ(row.runtimeOnly, want.runtimeOnly);
        EXPECT_EQ(findOpcodeDescriptor(want.opcode), &row);
    }
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

TEST(OpcodeDescriptorTest, RemovedAndOutOfRangeSlotsHaveNoRow) {
    std::vector<int64_t> slots = { 23, 35, 36, 37, 38, 39, 40, 41, 50, -1, 1000 };
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

// A function `f(left: Int, right: Int) -> Int` whose entry block is `instruction` followed by `ret left`.
Module moduleWith(Instruction instruction, bool runtimeContext = false) {
    Module module;
    module.sourceName = "manual.joyeer";
    module.types = {
        TypeName { 0, "Void", joyeer::typing::TypeKind::voidType },
        TypeName { 1, "Int", joyeer::typing::TypeKind::integer },
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
    function.runtimeContext = runtimeContext;
    function.entry = 0;
    instruction.span = SourceSpan { 10, 3 };
    Instruction ret;
    ret.opcode = Opcode::returnValue;
    ret.operands = { 0 };
    ret.span = SourceSpan { 20, 1 };
    function.blocks = { BasicBlock { 0, "entry", { std::move(instruction), std::move(ret) } } };
    module.functions.push_back(std::move(function));
    return module;
}

size_t errorsContaining(const VerificationResult& result, const std::string& text) {
    return static_cast<size_t>(std::count_if(result.errors.begin(), result.errors.end(), [&](const auto& error) {
        return error.message.find(text) != std::string::npos;
    }));
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

TEST(OpcodeDescriptorTest, RuntimeOnlyOpcodesNeedRuntimeAuthorization) {
    Instruction size;
    size.opcode = Opcode::typeSize;
    size.result = Value { 2, 1, ValueCategory::value };
    size.integerValue = 1;
    const auto denied = Verifier().verify(moduleWith(size));
    EXPECT_EQ(errorsContaining(denied, "requires runtime authorization"), 1u) << dump(denied);
    const auto allowed = Verifier().verify(moduleWith(size, true));
    EXPECT_TRUE(allowed.succeeded()) << dump(allowed);

    Instruction guard;
    guard.opcode = Opcode::linearRangeGuard;
    guard.operands = { 0 };
    const auto guardDenied = Verifier().verify(moduleWith(guard));
    EXPECT_EQ(errorsContaining(guardDenied, "requires runtime authorization"), 1u) << dump(guardDenied);
    const auto guardAllowed = Verifier().verify(moduleWith(guard, true));
    EXPECT_TRUE(guardAllowed.succeeded()) << dump(guardAllowed);
}

TEST(OpcodeDescriptorTest, EverySlotUpToTheLastHasARowUnlessRemoved) {
    const std::set<int64_t> removed = { 23, 35, 36, 37, 38, 39, 40, 41, 50 };
    for (int64_t slot = 0; slot <= static_cast<int64_t>(Opcode::stackBlock); ++slot) {
        SCOPED_TRACE(slot);
        EXPECT_EQ(findOpcodeDescriptor(static_cast<Opcode>(slot)) != nullptr, !removed.contains(slot));
    }
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
        const auto result = Verifier().verify(moduleWith(instruction));
        EXPECT_EQ(errorsContaining(result, "requires runtime authorization"), 1u) << dump(result);
    }
}

// The dump text of every opcode the dumper prints through its generic path, and of `fits`. Each expectation also
// holds for the dumper before the descriptor table.
TEST(OpcodeDescriptorTest, DumpTextIsUnchanged) {
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
        std::string expected;
    };
    const auto value = [](ValueId id, TypeId type) { return std::optional(Value { id, type, ValueCategory::value }); };
    const auto address = [](ValueId id, TypeId type) {
        return std::optional(Value { id, type, ValueCategory::address });
    };
    const std::optional<Value> none;
    const std::optional<joyeer::semantic::SymbolId> noSymbol;
    const std::vector<Case> cases = {
        { Opcode::integerConstant, value(10, 1), {}, -5, "", noSymbol, "%10: Int = integer -5" },
        { Opcode::integerConstant, value(11, 6), {}, -1, "", noSymbol, "%11: UInt64 = integer 18446744073709551615" },
        { Opcode::booleanConstant, value(12, 3), {}, 1, "", noSymbol, "%12: Bool = boolean 1" },
        { Opcode::byteConstant, value(13, 2), {}, 200, "", noSymbol, "%13: UInt8 = byte 200" },
        { Opcode::stringConstant, value(14, 4), {}, 0, "hi", noSymbol, "%14: String = string \"hi\"" },
        { Opcode::floatingConstant, value(15, 5), {}, 0, "1.5", noSymbol, "%15: Float64 = floating 1.5" },
        { Opcode::unitConstant, value(16, 0), {}, 0, "", noSymbol, "%16: Void = unit" },
        { Opcode::constantValue, value(17, 1), {}, 0, "", noSymbol, "%17: Int = constant #0" },
        { Opcode::stackAllocate, address(18, 1), {}, 0, "", noSymbol, "%18: &Int = alloc_stack" },
        { Opcode::zeroInitialize, none, { 18 }, 0, "", noSymbol, "zero_init %18" },
        { Opcode::load, value(19, 1), { 18 }, 0, "", noSymbol, "%19: Int = load %18" },
        { Opcode::store, none, { 0, 18 }, 0, "", noSymbol, "store %0, %18" },
        { Opcode::copyValue, value(20, 1), { 0 }, 0, "", noSymbol, "%20: Int = copy %0" },
        { Opcode::take, value(21, 1), { 18 }, 0, "", noSymbol, "%21: Int = take %18" },
        { Opcode::destroy, none, { 18 }, 0, "", noSymbol, "destroy %18" },
        { Opcode::add, value(22, 1), { 0, 1 }, 0, "", noSymbol, "%22: Int = add %0, %1" },
        { Opcode::less, value(23, 3), { 0, 1 }, 0, "", noSymbol, "%23: Bool = lt %0, %1" },
        { Opcode::shiftLeft, value(24, 1), { 0, 1 }, 0, "", noSymbol, "%24: Int = shl %0, %1" },
        { Opcode::convertChecked, value(25, 2), { 0 }, 0, "", noSymbol, "%25: UInt8 = convert %0" },
        { Opcode::integerFits, value(26, 3), { 0 }, 2, "", noSymbol, "%26: Bool = fits %0 in type 2" },
        { Opcode::fieldAddress, address(27, 1), { 18 }, 0, "", 7, "%27: &Int = field_addr %18 symbol#7" },
        { Opcode::extractPayload, value(28, 1), { 0 }, 1, "", 9, "%28: Int = extract_payload %0, 1 symbol#9" },
        { Opcode::payloadAddress, address(29, 1), { 18 }, 0, "", 9, "%29: &Int = payload_address %18, 0 symbol#9" },
        { Opcode::stringUtf8, value(30, 1), { 14 }, 0, "", noSymbol, "%30: Int = string_utf8 %14" },
        { Opcode::count, value(31, 1), { 14 }, 0, "", noSymbol, "%31: Int = count %14" },
        { Opcode::subscript, value(32, 2), { 14, 0 }, 0, "", noSymbol, "%32: UInt8 = subscript %14, %0" },
        { Opcode::typeSize, value(33, 1), {}, 1, "", noSymbol, "%33: Int = type_size of type 1" },
        { Opcode::integerAddress, address(34, 1), { 0 }, 0, "", noSymbol, "%34: &Int = integer_address %0" },
        { Opcode::pointerToInteger, value(35, 1), { 18 }, 0, "", noSymbol, "%35: Int = pointer_to_integer %18" },
        { Opcode::destroyRange, none, { 0, 1 }, 1, "", noSymbol, "destroy_range of type 1 %0, %1" },
        { Opcode::cloneAt, none, { 0, 1 }, 1, "", noSymbol, "clone_at of type 1 %0, %1" },
        { Opcode::cloneCallback, value(36, 1), {}, 1, "", noSymbol, "%36: Int = clone_callback of type 1" },
        { Opcode::linearRangeGuard, none, { 0 }, 0, "", noSymbol, "linear_range_guard %0" },
        { Opcode::slotAddress, value(37, 1), { 0, 1 }, 1, "", noSymbol, "%37: Int = slot_address of type 1 %0, %1" },
        { Opcode::stackBlock, value(38, 1), { 0 }, 1, "", noSymbol, "%38: Int = stack_block of type 1 %0" },
        { Opcode::returnValue, none, { 0 }, 0, "", noSymbol, "ret %0" },
        { Opcode::returnVoid, none, {}, 0, "", noSymbol, "ret_void" },
        { Opcode::unreachable, none, {}, 0, "", noSymbol, "unreachable" },
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
