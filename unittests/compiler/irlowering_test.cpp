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

TEST_F(IRLoweringTest, ReportsUnsupportedControlFlowWithoutInvalidIRClaims) {
    lower(R"JOYEER(func choose(flag: Bool): Int {
return if flag { 1 } else { 2 }
}
)JOYEER");

    ASSERT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::lowering::DiagnosticId::unsupportedSyntax);
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
