#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"
#include "joyeer/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

class IRLoweringTest : public testing::Test {
protected:
    void lower(const std::string& text) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        const auto context = std::make_shared<CompileContext>(
                &lexerDiagnostics,
                std::make_shared<SymbolTable>());
        LexParser lexer(context, LexerProfile::jsonParserMvp);
        lexer.parse(source);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());

        joyeer::parser::Parser parser(source->tokens);
        const auto parsing = parser.parse();
        ASSERT_TRUE(parsing.succeeded()) << joyeer::parser::dump(parsing.diagnostics);

        const auto resolution = joyeer::semantic::NameResolver().resolve(parsing.root);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);

        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        result = joyeer::lowering::Lowerer().lower(checking.model, "test.joyeer");
    }

    const joyeer::ir::Function& function(const std::string& name) const {
        const auto found = std::find_if(
                result.module->functions.begin(),
                result.module->functions.end(),
                [&name](const auto& function) { return function.name == name; });
        EXPECT_NE(found, result.module->functions.end());
        return *found;
    }

    size_t opcodeCount(
            const joyeer::ir::Function& function,
            joyeer::ir::Opcode opcode) const {
        size_t count = 0;
        for (const auto& block : function.blocks) {
            count += static_cast<size_t>(std::count_if(
                    block.instructions.begin(),
                    block.instructions.end(),
                    [opcode](const auto& instruction) {
                        return instruction.opcode == opcode;
                    }));
        }
        return count;
    }

    Diagnostics lexerDiagnostics;
    SourceFile::Ptr source;
    joyeer::lowering::Result result;
};

TEST_F(IRLoweringTest, LowersParametersOperatorsAndReturns) {
    lower(R"JOYEER(func add(left: Int, right: Int): Int {
return left + right
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    ASSERT_NE(result.module, nullptr);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& add = function("add");
    ASSERT_EQ(add.blocks.size(), 1u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::stackAllocate), 2u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::store), 2u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::load), 2u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::add), 1u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersBindingsAndMutableAssignmentsThroughStackSlots) {
    lower(R"JOYEER(func increment(value: Int): Int {
var current = value
current = current + 1
return current
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& increment = function("increment");
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::stackAllocate), 2u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::store), 3u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::integerConstant), 1u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::add), 1u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersUserCallsAndTypeErasedPrintCalls) {
    lower(R"JOYEER(func identity(value: Int): Int { return value }
func run() {
let number = identity(value: 42)
print(value: number)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& print = function("print");
    ASSERT_TRUE(print.isExternal);
    ASSERT_EQ(print.parameters.size(), 1u);
    EXPECT_TRUE(print.parameters[0].acceptsAnyType);

    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::call), 2u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::returnVoid), 1u);
}

TEST_F(IRLoweringTest, LowersIfExpressionValuesThroughMergeSlots) {
    lower(R"JOYEER(func choose(flag: Bool): Int {
return if flag { 1 } else { 2 }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& choose = function("choose");
    EXPECT_EQ(choose.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::conditionalBranch), 1u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::branch), 2u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::stackAllocate), 2u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersIfBranchesThatReturnEarly) {
    lower(R"JOYEER(func choose(flag: Bool): Int {
if flag { return 1 }
return 2
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& choose = function("choose");
    EXPECT_EQ(choose.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::conditionalBranch), 1u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::returnValue), 2u);
}

TEST_F(IRLoweringTest, LowersWhileHeadersBodiesExitsAndBackEdges) {
    lower(R"JOYEER(func count(limit: Int): Int {
var value = 0
while value < limit {
value = value + 1
}
return value
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& count = function("count");
    EXPECT_EQ(count.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::conditionalBranch), 1u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::branch), 2u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::less), 1u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersInoutParametersAndArgumentsAsAddresses) {
    lower(R"JOYEER(func increment(value: inout Int) {
&value = value + 1
}
func run(): Int {
var number = 0
increment(value: &number)
return number
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& increment = function("increment");
    ASSERT_EQ(increment.parameters.size(), 1u);
    EXPECT_TRUE(increment.parameters[0].isMutable);
    EXPECT_EQ(
            increment.parameters[0].value.category,
            joyeer::ir::ValueCategory::address);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::stackAllocate), 0u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::store), 1u);

    const auto& run = function("run");
    const auto call = std::find_if(
            run.blocks[0].instructions.begin(),
            run.blocks[0].instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::call;
            });
    ASSERT_NE(call, run.blocks[0].instructions.end());
    ASSERT_EQ(call->operands.size(), 1u);
    const auto slot = std::find_if(
            run.blocks[0].instructions.begin(),
            run.blocks[0].instructions.end(),
            [&call](const auto& instruction) {
                return instruction.result.has_value() &&
                        instruction.result->id == call->operands[0];
            });
    ASSERT_NE(slot, run.blocks[0].instructions.end());
    EXPECT_EQ(slot->result->category, joyeer::ir::ValueCategory::address);
}

TEST_F(IRLoweringTest, LowersStructConstructionFieldReadsAndWrites) {
    lower(R"JOYEER(struct Box {
var value: Int
let fixed: Int = 7
}
func make(): Int {
var box = Box(value: 1)
box.value = box.value + box.fixed
return box.value
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    ASSERT_EQ(result.module->structures.size(), 1u);
    EXPECT_EQ(result.module->structures[0].fields.size(), 2u);

    const auto& make = function("make");
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::constructStruct), 1u);
    EXPECT_GE(opcodeCount(make, joyeer::ir::Opcode::fieldAddress), 3u);
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::add), 1u);
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersUserOptionalAndNilEnumConstruction) {
    lower(R"JOYEER(enum Value { Empty, Number(Int), }
func qualified(): Value { return Value.Number(1) }
func contextual(): Value { return .Empty }
func some(value: Int): Int? { return .Some(value) }
func none(): Int? { return nil }
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_GE(result.module->enumerations.size(), 2u);
    EXPECT_EQ(opcodeCount(function("qualified"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("contextual"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("some"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("none"), joyeer::ir::Opcode::constructEnum), 1u);
}

TEST_F(IRLoweringTest, LowersStringCountAndSubscript) {
    lower(R"JOYEER(func first(text: String): UInt8 {
let length = text.count
print(value: length)
return text[0]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& first = function("first");
    EXPECT_EQ(opcodeCount(first, joyeer::ir::Opcode::count), 1u);
    EXPECT_EQ(opcodeCount(first, joyeer::ir::Opcode::subscript), 1u);
}

TEST_F(IRLoweringTest, LowersEnumMatchesAndPayloadBindings) {
    lower(R"JOYEER(enum Value { Empty, Number(Int), }
func read(value: Value): Int {
return match value {
.Empty => 0,
.Number(number) => number,
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& read = function("read");
    EXPECT_EQ(read.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(read, joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(read, joyeer::ir::Opcode::extractPayload), 1u);
    EXPECT_EQ(opcodeCount(read, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersOptionalMatchesToResultConstruction) {
    lower(R"JOYEER(enum Failure { Missing, }
func unwrap(value: Int?): Result<Int, Failure> {
return match value {
.Some(number) => .Ok(number),
.None => .Err(.Missing),
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& unwrap = function("unwrap");
    EXPECT_EQ(opcodeCount(unwrap, joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(unwrap, joyeer::ir::Opcode::extractPayload), 1u);
    EXPECT_EQ(opcodeCount(unwrap, joyeer::ir::Opcode::constructEnum), 3u);
}

TEST_F(IRLoweringTest, LowersLiteralWildcardAndNestedPayloadPatterns) {
    lower(R"JOYEER(enum Flag { Bool(Bool), }
func classify(value: UInt8): Int {
return match value { b'a' => 1, _ => 0, }
}
func fromFlag(flag: Flag): Int {
return match flag { .Bool(true) => 1, .Bool(false) => 0, }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_EQ(opcodeCount(function("classify"), joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(function("fromFlag"), joyeer::ir::Opcode::switchPattern), 1u);

    const auto text = joyeer::ir::dump(*result.module);
    EXPECT_NE(text.find("byte(97):UInt8"), std::string::npos);
    EXPECT_NE(text.find("case#"), std::string::npos);
    EXPECT_NE(text.find("true:Bool"), std::string::npos);
}

TEST_F(IRLoweringTest, ReportsStraightLineFunctionsThatFallThrough) {
    lower(R"JOYEER(func incomplete(value: Int): Int {
let copy = value
}
)JOYEER");

    ASSERT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::lowering::DiagnosticId::missingReturn);
}

} // namespace
