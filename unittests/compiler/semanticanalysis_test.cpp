#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/semanticanalysis.h"
#include "joyeer/compiler/sourcefile.h"
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
        LexParser lexer(&lexerDiagnostics);
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

TEST_F(SemanticAnalysisTest, UnitStillRequiresInitializationAndConsumptionTracking) {
    for (const auto* text : {
            "func accept(value: Void) {}\nfunc run() {\n"
            "let value: Void\naccept(value: value)\n}\n",
            "func take(value: consuming Void) {}\nfunc accept(value: Void) {}\n"
            "func run() {\nlet value = ()\ntake(value: consume value)\n"
            "accept(value: value)\n}\n",
            "func initialize(value: initializing Void) {}\n"}) {
        SCOPED_TRACE(text);
        analyze(text);
        EXPECT_FALSE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, AcceptsUnitInitializationAndReinitialization) {
    analyze("func initialize(value: initializing Void) { &value = () }\n"
            "func take(value: consuming Void) {}\n"
            "func run() {\nvar value: Void\ninitialize(value: &value)\n"
            "take(value: consume value)\nvalue = ()\n"
            "match value { () => () }\n}\n");
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
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

TEST_F(SemanticAnalysisTest, TracksInitializingParametersAndCallerState) {
    analyze(R"JOYEER(func initialize(out: initializing String) {
&out = "value"
}
func forward(out: initializing String) {
initialize(out: &out)
}
func valid(): String {
var text: String
forward(out: &text)
return text
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, RejectsReadsBeforeInitializingWrites) {
    analyze(R"JOYEER(func invalid(out: initializing String) {
print(value: out)
&out = "value"
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::useBeforeInitialization));
}

TEST_F(SemanticAnalysisTest, RequiresInitializingParametersOnEveryReturnPath) {
    analyze(R"JOYEER(func invalid(out: initializing String, flag: Bool) {
if flag { return }
&out = "value"
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::initializingParameterNotInitialized));
}

TEST_F(SemanticAnalysisTest, RejectsInitializingAlreadyInitializedStorage) {
    analyze(R"JOYEER(func initialize(out: initializing String) { &out = "value" }
func invalid() {
var text = "already"
initialize(out: &text)
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::initializingInitializedStorage));
}

TEST_F(SemanticAnalysisTest, AllowsInitializingConsumedStorage) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func initialize(out: initializing String) { &out = "new" }
func valid(): String {
var text = "old"
take(value: consume text)
initialize(out: &text)
return text
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, TracksConsumedStructFieldsIndependently) {
    analyze(R"JOYEER(struct Pair { var left: String
var right: String
}
func take(value: consuming String) { print(value: value) }
func valid(): String {
var pair = Pair(left: "left", right: "right")
take(value: consume pair.left)
print(value: pair.right)
pair.left = "new"
return pair.left + pair.right
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, RejectsWholeValueReadsAfterFieldConsumption) {
    analyze(R"JOYEER(struct Pair { var left: String
var right: String
}
func take(value: consuming String) { print(value: value) }
func invalid() {
var pair = Pair(left: "left", right: "right")
take(value: consume pair.left)
let copy = pair
print(value: copy.right)
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));
}

TEST_F(SemanticAnalysisTest, ConservativelyTracksConsumedSubscripts) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func invalid() {
var values = ["left", "right"]
take(value: consume values[0])
print(value: values[1])
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));
}

TEST_F(SemanticAnalysisTest, AllowsReinitializingConsumedSubscripts) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func valid(): String {
var values = ["left", "right"]
take(value: consume values[0])
&values[0] = "new"
return values[1]
}
)JOYEER");

    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, MergesConsumedProjectionStateAcrossBranches) {
    analyze(R"JOYEER(struct Pair { var left: String
var right: String
}
func take(value: consuming String) { print(value: value) }
func invalid(flag: Bool) {
var pair = Pair(left: "left", right: "right")
if flag { take(value: consume pair.left) }
print(value: pair.left)
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));
}

TEST_F(SemanticAnalysisTest, RejectsProjectionConsumptionAcrossLoopBackEdges) {
    analyze(R"JOYEER(func take(value: consuming String) { print(value: value) }
func invalid(flag: Bool) {
var values = ["left"]
while flag { take(value: consume values[0]) }
}
)JOYEER");

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));
}

TEST_F(SemanticAnalysisTest, ChecksIndexReadsInEveryStorageProjection) {
    for (const auto* use : {
                "take(value: consume values[index].text)",
                "print(value: values[index].text)",
                "&values[index].text = \"new\"" }) {
        SCOPED_TRACE(use);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "struct Item { var text: String }\n"
                "func take(value: consuming String) {}\n"
                "func invalid() {\n"
                "var values = [Item(text: \"old\")]\n"
                "var index: Int\n" + std::string(use) + "\n}\n"));
        EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useBeforeInitialization))
                << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, AppliesConsumeEffectsInsideProjectionIndices) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func index(value: consuming String): Int { return 0 }
func take(value: consuming String) {}
func invalid() {
var values = ["element"]
var text = "index"
take(value: consume values[index(value: consume text)])
print(value: text)
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, DoesNotRestoreADifferentConsumedElement) {
    for (const auto* replacement : {
                "&values[1] = \"new\"",
                "index = 1\n&values[index] = \"new\"",
                "setIndex(value: &index)\n&values[index] = \"new\"" }) {
        SCOPED_TRACE(replacement);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func take(value: consuming String) {}\n"
                "func setIndex(value: inout Int) { &value = 1 }\n"
                "func invalid() {\nvar values = [\"a\", \"b\"]\n"
                "var index = 0\ntake(value: consume values[index])\n" +
                std::string(replacement) + "\nprint(value: values[0])\n}\n"));
        EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
                << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, RestoresAnUnchangedIndexAndDoesNotReuseMutatedIdentity) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func valid(index: Int): String {
var values = ["old"]
take(value: consume values[index])
&values[index] = "new"
return values[index]
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);

    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid() {
var values = ["a", "b"]
var index = 0
take(value: consume values[1])
&values[index] = if true { index = 1
"new"
} else { "new" }
print(value: values[1])
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, RejectsConsumptionOnTheNextLoopCondition) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func next(value: consuming String): Bool { return true }
func invalid() {
var text = "owned"
while next(value: consume text) {}
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AcceptsRestorationBeforeEachLoopConsumption) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func next(value: consuming String): Bool { return true }
func valid(flag: Bool) {
var text = "owned"
while flag {
text = "new"
take(value: consume text)
}
text = "reset"
while next(value: consume text) {
text = "next"
}
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, CommitsInitializingEffectsOnlyAfterTheCallReturns) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String, flag: Bool) { &out = "new" }
func invalid(out: initializing String, flag: Bool) {
initialize(out: &out, flag: if flag { return } else { false })
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::initializingParameterNotInitialized))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, RejectsPossiblyInitializedDestinations) {
    for (const auto* setup : {
                "if flag { text = \"old\" }\ninitialize(out: &text)",
                "while flag { initialize(out: &text) }" }) {
        SCOPED_TRACE(setup);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func initialize(out: initializing String) { &out = \"new\" }\n"
                "func invalid(flag: Bool) {\nvar text: String\n" +
                std::string(setup) + "\n}\n"));
        EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::initializingInitializedStorage))
                << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, AllowsRepeatedInitializationAfterConsumption) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String) { &out = "new" }
func take(value: consuming String) {}
func valid(flag: Bool) {
var text: String
while flag {
initialize(out: &text)
take(value: consume text)
}
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, PreservesConditionalLogicalAndEffects) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String): Bool { &out = "new"
return true
}
func invalid(flag: Bool) {
var text: String
flag && initialize(out: &text)
print(value: text)
initialize(out: &text)
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useBeforeInitialization));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::initializingInitializedStorage));

    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String): Bool { return true }
func invalid(flag: Bool) {
var text = "owned"
flag && take(value: consume text)
print(value: text)
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));

    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func valid(flag: Bool) {
var text = "owned"
flag && if true { return } else { false }
print(value: text)
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
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
