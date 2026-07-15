#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/semanticanalysis.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

class SemanticAnalysisTest : public testing::Test {
protected:
    void analyze(const std::string& text) {
        Diagnostics lexerDiagnostics;
        const auto source = std::make_shared<SourceFile>(text);
        const auto context = std::make_shared<CompileContext>(
                &lexerDiagnostics,
                std::make_shared<SymbolTable>());
        LexParser lexer(context, LexerProfile::jsonParserMvp);
        lexer.parse(source);
        ASSERT_FALSE(lexerDiagnostics.hasFailure());

        joyeer::parser::Parser parser(source->tokens);
        const auto parsing = parser.parse();
        ASSERT_TRUE(parsing.succeeded()) << joyeer::parser::dump(parsing.diagnostics);

        const auto resolution = joyeer::semantic::NameResolver().resolve(parsing.root);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);

        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        result = joyeer::analysis::Analyzer().analyze(checking.model);
    }

    bool hasDiagnostic(joyeer::analysis::DiagnosticId id) const {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [id](const auto& diagnostic) {
            return diagnostic.id == id;
        });
    }

    joyeer::analysis::Result result;
};

TEST_F(SemanticAnalysisTest, AcceptsExplicitAndTrailingExpressionReturns) {
    analyze(R"JOYEER(func square(value: Int): Int {
value * value
}
func choose(flag: Bool): Int {
if flag { return 1 } else { return 2 }
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST_F(SemanticAnalysisTest, DiagnosesFunctionsThatCanFallThrough) {
    analyze(R"JOYEER(func incomplete(flag: Bool): Int {
if flag { return 1 }
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::analysis::DiagnosticId::missingReturn);
    EXPECT_EQ(
            result.diagnostics[0].severity,
            joyeer::analysis::Severity::error);
}

TEST_F(SemanticAnalysisTest, ConservativelyTreatsWhileAsFallthrough) {
    analyze(R"JOYEER(func incomplete(): Int {
while true { return 1 }
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::missingReturn));
}

TEST_F(SemanticAnalysisTest, AcceptsExhaustiveTerminatingMatchArms) {
    analyze(R"JOYEER(enum Choice { First, Second, }
func choose(value: Choice): Int {
match value {
.First => return 1,
.Second => return 2,
}
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AcceptsNeverTerminatingTrailingCalls) {
    analyze(R"JOYEER(func stop(): Never { return stop() }
func value(): Int { stop() }
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ReportsUnreachableStatementsAsWarnings) {
    analyze(R"JOYEER(func value(): Int {
return 1
let unreachable = 2
return unreachable
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_TRUE(std::all_of(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::analysis::DiagnosticId::unreachableCode &&
                        diagnostic.severity == joyeer::analysis::Severity::warning;
            }));
}

TEST_F(SemanticAnalysisTest, RejectsDirectReadsBeforeInitialization) {
    analyze(R"JOYEER(func value(): Int {
var output: Int
return output
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::useBeforeInitialization));
}

TEST_F(SemanticAnalysisTest, AcceptsInitializationBeforeUse) {
    analyze(R"JOYEER(func value(): Int {
var output: Int
output = 42
return output
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, RequiresEveryContinuingBranchToInitialize) {
    analyze(R"JOYEER(func complete(flag: Bool): Int {
var output: Int
if flag { output = 1 } else { output = 2 }
return output
}
func incomplete(flag: Bool): Int {
var output: Int
if flag { output = 1 }
return output
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(
            std::count_if(
                    result.diagnostics.begin(),
                    result.diagnostics.end(),
                    [](const auto& diagnostic) {
                        return diagnostic.id ==
                                joyeer::analysis::DiagnosticId::useBeforeInitialization;
                    }),
            1);
}

TEST_F(SemanticAnalysisTest, IgnoresTerminatedBranchesAtInitializationMerge) {
    analyze(R"JOYEER(func value(flag: Bool): Int {
var output: Int
if flag { return 1 } else { output = 2 }
return output
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, DoesNotAssumeWhileBodiesExecute) {
    analyze(R"JOYEER(func value(flag: Bool): Int {
var output: Int
while flag { output = 1 }
return output
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::useBeforeInitialization));
}

TEST_F(SemanticAnalysisTest, ChecksReadsOnAssignmentRightHandSides) {
    analyze(R"JOYEER(func value(): Int {
var output: Int
output = output + 1
return output
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::useBeforeInitialization));
}

TEST_F(SemanticAnalysisTest, RequiresInitializedAggregateStorageAndInoutArguments) {
    analyze(R"JOYEER(func update(value: inout Int) { &value = 1 }
func run() {
var numbers: [Int]
&numbers[0] = 1
var number: Int
update(value: &number)
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(
            std::count_if(
                    result.diagnostics.begin(),
                    result.diagnostics.end(),
                    [](const auto& diagnostic) {
                        return diagnostic.id ==
                                joyeer::analysis::DiagnosticId::useBeforeInitialization;
                    }),
            2);
}

TEST_F(SemanticAnalysisTest, RejectsUseAndDoubleConsumeAfterOwnershipTransfer) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func invalid() {
var text = "owned"
take(value: consume text)
print(value: text)
take(value: consume text)
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(
            std::count_if(
                    result.diagnostics.begin(),
                    result.diagnostics.end(),
                    [](const auto& diagnostic) {
                        return diagnostic.id ==
                                joyeer::analysis::DiagnosticId::useAfterConsume;
                    }),
            2);
}

TEST_F(SemanticAnalysisTest, AllowsAssignmentToReinitializeConsumedStorage) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func valid(): String {
var text = "first"
take(value: consume text)
text = "second"
return text
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, MergesConsumedStateAcrossBranches) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func invalid(flag: Bool) {
var text = "owned"
if flag { take(value: consume text) }
print(value: text)
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));
}

TEST_F(SemanticAnalysisTest, RejectsConsumptionAcrossLoopBackEdges) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func invalid(flag: Bool) {
var text = "owned"
while flag { take(value: consume text) }
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));
}

TEST_F(SemanticAnalysisTest, ReportsUnusedLocalBindingsAsWarnings) {
    analyze(R"JOYEER(func run() {
let first = 1
var second: Int
second = 2
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    ASSERT_EQ(result.diagnostics.size(), 2u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_TRUE(std::all_of(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::analysis::DiagnosticId::unusedBinding &&
                        diagnostic.severity == joyeer::analysis::Severity::warning;
            }));
}

TEST_F(SemanticAnalysisTest, SuppressesUsedAndExplicitlyIgnoredBindings) {
    analyze(R"JOYEER(func run() {
let used = 1
let _ignored = 2
print(value: used)
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::unusedBinding));
}

TEST_F(SemanticAnalysisTest, CountsAggregateAndInoutStorageAsUsed) {
    analyze(R"JOYEER(func update(value: inout Int) { &value = 1 }
func run() {
var values = [1]
&values[0] = 2
var number = 0
update(value: &number)
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::unusedBinding));
}

TEST_F(SemanticAnalysisTest, ReportsUnusedPatternBindings) {
    analyze(R"JOYEER(enum Choice { None, Some(Int), }
func run(value: Choice) {
match value {
.None => return,
.Some(item) => return,
}
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::unusedBinding);
    EXPECT_NE(result.diagnostics[0].message.find("item"), std::string::npos);
}

} // namespace
