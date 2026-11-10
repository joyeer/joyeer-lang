#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/sourcefile.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/diagnostics/diagnostic.h"

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

    // Analyses `text` as a toolchain-authorized (runtime) source, where `linear` is allowed.
    void analyzeToolchain(const std::string& text) {
        Diagnostics lexerDiagnostics;
        const auto source = std::make_shared<SourceFile>(text);
        LexParser lexer(&lexerDiagnostics);
        lexer.parse(source);
        ASSERT_FALSE(lexerDiagnostics.hasFailure());

        joyeer::parser::Parser parser(source->tokens);
        const auto parsing = parser.parse();
        ASSERT_TRUE(parsing.succeeded()) << joyeer::parser::dump(parsing.diagnostics);

        const auto resolution = joyeer::semantic::NameResolver().resolve(
                { { "runtime.fixture", { parsing.root }, true } });
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

TEST_F(SemanticAnalysisTest, ConsumingReceiverEndsTheReceiversOwnership) {
    analyze("struct C {\n"
            "    var n: Int\n"
            "    func read(): Int { return self.n }\n"
            "    consuming func done(): Int { return self.n }\n"
            "}\n"
            "func run(): Int {\n"
            "    var c = C(n: 1)\n"
            "    let total = consume c.done()\n"
            "    return c.read() + total\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ReinitializingAConsumedReceiverAllowsLaterUse) {
    analyze("struct C {\n"
            "    var n: Int\n"
            "    func read(): Int { return self.n }\n"
            "    consuming func done(): Int { return self.n }\n"
            "}\n"
            "func run(): Int {\n"
            "    var c = C(n: 1)\n"
            "    let total = consume c.done()\n"
            "    c = C(n: total)\n"
            "    return c.read()\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnalyzesMethodBodiesLikeFunctions) {
    analyze("struct C {\n"
            "    var n: Int\n"
            "    func missing(): Int { }\n"
            "}\n"
            "enum E {\n"
            "    A\n"
            "    func code(): Int { return 1\n"
            "        let late = 2 }\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::missingReturn));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
}

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

TEST_F(SemanticAnalysisTest, KeepsPropagationSuccessPathsReachable) {
    analyze("func pass(input: Int?): Int? {\n"
            "let value = input?\n"
            "return value\n"
            "}\n");
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_TRUE(result.diagnostics.empty()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, NeverSuccessPropagationTerminatesTheFunction) {
    analyze("func fail(): Result<Never, String> { return .Err(\"stop\") }\n"
            "func forward(): Result<Int, String> {\n"
            "fail()?\n"
            "}\n");
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_TRUE(result.diagnostics.empty()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, NeverErrorPropagationDoesNotRequireEarlyOutputInitialization) {
    analyze("func pass(out: initializing String, value: Result<Int, Never>): "
            "Result<Int, Never> {\n"
            "let number = value?\n"
            "&out = \"ready\"\n"
            "return .Ok(number)\n"
            "}\n");
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_TRUE(result.diagnostics.empty()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, PropagationFailureRequiresInitializingOutputs) {
    analyze("func load(out: initializing String, input: Result<Int, String>): "
            "Result<Int, String> {\n"
            "let value = input?\n"
            "&out = \"ready\"\n"
            "return .Ok(value)\n"
            "}\n");
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::initializingParameterNotInitialized))
            << joyeer::analysis::dump(result.diagnostics);
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

TEST_F(SemanticAnalysisTest, DictionaryGetRequiresLiveInitializedReceiverAndKey) {
    for (const auto* text : {
            "func run() {\nlet values: [String: Int]\nvalues.get(key: \"x\")\n}\n",
            "func take(value: consuming [String: Int]) {}\nfunc run() {\n"
            "let values: [String: Int] = [:]\ntake(value: consume values)\nvalues.get(key: \"x\")\n}\n",
            "func run(values: [String: Int]) {\nlet key: String\nvalues.get(key: key)\n}\n",
        }) {
        SCOPED_TRACE(text);
        ASSERT_NO_FATAL_FAILURE(analyze(text));
        EXPECT_FALSE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, DictionaryGetDoesNotConsumeReceiverOrKey) {
    analyze("func run(values: [String: Int], key: String): Int? {\n"
            "let first = values.get(key: key)\n"
            "let second = values.get(key: key)\n"
            "print(value: values.count)\nprint(value: key)\n"
            "match first { .Some(value) => print(value: value), .None => () }\n"
            "return second\n}\n");
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
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

TEST_F(SemanticAnalysisTest, ReportsUnreachableCodeAfterLoopTransfers) {
    for (const auto* transfer : { "break", "continue" }) {
        SCOPED_TRACE(transfer);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func run(flag: Bool) {\nwhile flag {\n" +
                std::string(transfer) +
                "\nprint(value: 1)\n}\nprint(value: 2)\n}\n"));
        EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
        ASSERT_EQ(result.diagnostics.size(), 1u)
                << joyeer::analysis::dump(result.diagnostics);
        EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::unreachableCode);
    }
}

TEST_F(SemanticAnalysisTest, BreakIsNotAFunctionReturn) {
    ASSERT_NO_FATAL_FAILURE(analyze(
            "func incomplete(): Int {\nwhile true { break }\n}\n"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::missingReturn))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ConditionLoopTransfersDoNotProveFunctionReturn) {
    for (const auto* transfer : { "break", "continue" }) {
        SCOPED_TRACE(transfer);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func incomplete(flag: Bool): Int {\nwhile if flag { " +
                std::string(transfer) + " } else { " + transfer + " } {}\n}\n"));
        EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::missingReturn))
                << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, RecognizesFunctionReturnsInTheLoopCondition) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func valid(flag: Bool): Int {
while if flag { return 1 } else { return 2 } {}
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, MergesConsumptionFromConditionBreaks) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool) {
var text = "owned"
while if flag {
take(value: consume text)
break
} else { false } {}
print(value: text)
}
)JOYEER"));
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::useAfterConsume);
}

TEST_F(SemanticAnalysisTest, RechecksConsumptionFromConditionContinues) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool) {
var text = "owned"
while if flag {
take(value: consume text)
continue
} else { false } {}
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ConditionContinuesForgetLocalInitializationFacts) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String) { &out = "ready" }
func valid(flag: Bool) {
while if flag {
var text: String
initialize(out: &text)
print(value: text)
continue
} else { false } {}
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, DoesNotAnalyzeStorageAfterLoopTransfers) {
    for (const auto* transfer : { "break", "continue" }) {
        SCOPED_TRACE(transfer);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func take(value: consuming String) {}\n"
                "func valid(flag: Bool) {\nvar text = \"owned\"\nwhile flag {\n" +
                std::string(transfer) +
                "\ntake(value: consume text)\n}\nprint(value: text)\n}\n"));
        EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
        ASSERT_EQ(result.diagnostics.size(), 1u)
                << joyeer::analysis::dump(result.diagnostics);
        EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::unreachableCode);
    }
}

TEST_F(SemanticAnalysisTest, ChecksUnreachableCodeInEveryMatchArm) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
enum Choice { First, Second, }
func valid(flag: Bool, choice: Choice) {
while flag {
match choice {
.First => (),
.Second => { break
print(value: 1)
},
}
print(value: 2)
}
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::unreachableCode);
}

TEST_F(SemanticAnalysisTest, MergesBreakConsumptionWithoutAddingABackEdge) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool) {
var text = "owned"
while flag {
take(value: consume text)
break
}
print(value: text)
}
)JOYEER"));
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::useAfterConsume);
}

TEST_F(SemanticAnalysisTest, MergesPossibleInitializationAtBreakExits) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String) { &out = "ready" }
func invalid(flag: Bool) {
var text: String
while flag {
initialize(out: &text)
break
}
initialize(out: &text)
print(value: text)
}
)JOYEER"));
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::analysis::DiagnosticId::initializingInitializedStorage);
}

TEST_F(SemanticAnalysisTest, MergesConsumedProjectionsAtBreakExits) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
struct Pair { var first: String
var second: String
}
func take(value: consuming String) {}
func invalid(flag: Bool) {
var pair = Pair(first: "first", second: "second")
while flag {
take(value: consume pair.first)
break
}
print(value: pair.second)
print(value: pair.first)
}
)JOYEER"));
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::useAfterConsume);
}

TEST_F(SemanticAnalysisTest, ContinueRechecksTheLoopConditionBeforeRestoration) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func present(value: String): Bool { return true }
func take(value: consuming String) {}
func invalid() {
var text = "owned"
while present(value: text) {
take(value: consume text)
continue
text = "restored"
}
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
}

TEST_F(SemanticAnalysisTest, JoinsContinueAndNormalBackEdges) {
    for (const auto* body : {
                "if flag { take(value: consume text)\ncontinue }\ntext = \"restored\"",
                "if flag { continue }\ntake(value: consume text)" }) {
        SCOPED_TRACE(body);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func take(value: consuming String) {}\n"
                "func invalid(flag: Bool) {\nvar text = \"owned\"\nwhile flag {\n"
                "print(value: text)\n" + std::string(body) + "\n}\n}\n"));
        EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
                << joyeer::analysis::dump(result.diagnostics);
    }
}

TEST_F(SemanticAnalysisTest, NestedBreakTargetsOnlyTheInnerLoop) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool) {
var text = "owned"
while flag {
while flag {
take(value: consume text)
break
}
print(value: text)
break
}
}
)JOYEER"));
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].id, joyeer::analysis::DiagnosticId::useAfterConsume);
}

TEST_F(SemanticAnalysisTest, NestedContinueTargetsOnlyTheInnerLoop) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool) {
var text = "owned"
while flag {
text = "restored"
while flag {
take(value: consume text)
continue
}
}
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
}

TEST_F(SemanticAnalysisTest, IgnoresLoopTransfersAtContinuingBranchJoins) {
    for (const auto* transfer : { "break", "continue" }) {
        SCOPED_TRACE(transfer);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func valid(flag: Bool) {\nwhile flag {\nvar text: String\n"
                "if flag { " + std::string(transfer) +
                " } else { text = \"ready\" }\nprint(value: text)\nbreak\n}\n}\n"));
        EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
        EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
    }
}

TEST_F(SemanticAnalysisTest, LoopTransfersDoNotFulfillInitializationObligations) {
    for (const auto* transfer : { "break", "continue" }) {
        SCOPED_TRACE(transfer);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func invalid(out: initializing String, flag: Bool) {\n"
                "while flag {\n" + std::string(transfer) + "\n&out = \"ready\"\n}\n}\n"));
        EXPECT_TRUE(hasDiagnostic(
                joyeer::analysis::DiagnosticId::initializingParameterNotInitialized))
                << joyeer::analysis::dump(result.diagnostics);
        EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
    }
}

TEST_F(SemanticAnalysisTest, LoopTransfersDoNotCheckFunctionExitObligations) {
    for (const auto* transfer : { "break", "continue" }) {
        SCOPED_TRACE(transfer);
        ASSERT_NO_FATAL_FAILURE(analyze(
                "func valid(out: initializing String, flag: Bool) {\n"
                "while flag { " + std::string(transfer) +
                " }\n&out = \"ready\"\n}\n"));
        EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
        EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::unreachableCode));
    }
}

TEST_F(SemanticAnalysisTest, ReturnInsideALoopStillChecksFunctionExitObligations) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func invalid(out: initializing String, flag: Bool) {
while flag {
if flag { return } else { break }
}
&out = "ready"
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(
            joyeer::analysis::DiagnosticId::initializingParameterNotInitialized))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ContinueForgetsLocalInitializationFacts) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String) { &out = "ready" }
func valid(flag: Bool) {
while flag {
var text: String
initialize(out: &text)
print(value: text)
continue
}
}
)JOYEER"));
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ContinueInvalidatesExitedIndexBindings) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool) {
var values = ["first", "second"]
while flag {
let index = if flag { 0 } else { 1 }
&values[index] = "restored"
take(value: consume values[index])
continue
}
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ContinueInvalidatesExitedPatternIndexBindings) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func invalid(flag: Bool, selected: Int?) {
var values = ["first", "second"]
while flag {
match selected {
.Some(index) => {
&values[index] = "restored"
take(value: consume values[index])
continue
},
.None => { break },
}
}
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, InnerBreakPreservesEnclosingIndexIdentity) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func take(value: consuming String) {}
func valid(flag: Bool) {
var values = ["first", "second"]
while flag {
let index = if flag { 0 } else { 1 }
while flag {
take(value: consume values[index])
break
}
&values[index] = "restored"
print(value: values[index])
continue
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

TEST_F(SemanticAnalysisTest, PreservesConditionalLogicalOrEffects) {
    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func initialize(out: initializing String): Bool { &out = "new"
return true
}
func invalid(flag: Bool) {
var text: String
flag || initialize(out: &text)
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
flag || take(value: consume text)
print(value: text)
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume));

    ASSERT_NO_FATAL_FAILURE(analyze(R"JOYEER(
func stop(): Never { return stop() }
func incomplete(flag: Bool): Int {
flag || stop()
print(value: 1)
}
)JOYEER"));
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::missingReturn));
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
// Decision 0015, stage 5: a struct with `deinit` is destroyed whole, so no part of it may be
// moved out, and its `deinit` body gets the same initialization analysis as any method.
TEST_F(SemanticAnalysisTest, RejectsMovingAFieldOutOfAStructWithDeinit) {
    analyze("struct Handle {\n"
            "    var name: String\n"
            "    deinit() { }\n"
            "}\n"
            "func take(value: consuming String) { print(value: value) }\n"
            "func run() {\n"
            "    var handle = Handle(name: \"a\")\n"
            "    take(value: consume handle.name)\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::partialConsumeOfDeinitType))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, RejectsMovingAnElementOutOfAnArrayFieldOfAStructWithDeinit) {
    analyze("struct Pool {\n"
            "    var names: [String]\n"
            "    deinit() { }\n"
            "}\n"
            "func take(value: consuming String) { print(value: value) }\n"
            "func run() {\n"
            "    var pool = Pool(names: [\"a\"])\n"
            "    take(value: consume pool.names[0])\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::partialConsumeOfDeinitType))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AllowsMovingAFieldOutOfAStructWithoutDeinit) {
    analyze("struct Pair {\n"
            "    var left: String\n"
            "    var right: String\n"
            "}\n"
            "func take(value: consuming String) { print(value: value) }\n"
            "func run() {\n"
            "    var pair = Pair(left: \"a\", right: \"b\")\n"
            "    take(value: consume pair.left)\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::partialConsumeOfDeinitType))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AllowsConsumingAWholeStructWithDeinit) {
    analyze("struct Handle {\n"
            "    var name: String\n"
            "    deinit() { }\n"
            "}\n"
            "func take(value: consuming Handle) { print(value: value.name) }\n"
            "func run() {\n"
            "    var handle = Handle(name: \"a\")\n"
            "    take(value: consume handle)\n"
            "}\n");
    EXPECT_TRUE(result.succeeded()) << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::partialConsumeOfDeinitType));
}

TEST_F(SemanticAnalysisTest, AllowsMovingAHandleOutOfAWrapperWithoutDeinit) {
    analyze("struct Handle {\n"
            "    var name: String\n"
            "    deinit() { }\n"
            "}\n"
            "struct Wrapper {\n"
            "    var handle: Handle\n"
            "    var count: Int\n"
            "}\n"
            "func take(value: consuming Handle) { print(value: value.name) }\n"
            "func run() {\n"
            "    var wrapper = Wrapper(handle: Handle(name: \"a\"), count: 1)\n"
            "    take(value: consume wrapper.handle)\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::partialConsumeOfDeinitType))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnalysesTheDeinitBodyForUseBeforeInitialization) {
    analyze("struct Handle {\n"
            "    var fd: Int\n"
            "    deinit() {\n"
            "        let early: Int\n"
            "        print(value: early)\n"
            "    }\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useBeforeInitialization))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ReportsAnUnusedBindingInsideDeinit) {
    analyze("struct Handle {\n"
            "    var fd: Int\n"
            "    deinit() { let spare = 1 }\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::unusedBinding))
            << joyeer::analysis::dump(result.diagnostics);
}
// A `consume` that moves a value into an aggregate, a binding or an assignment ends the
// source's ownership like any other consume.
TEST_F(SemanticAnalysisTest, MovingIntoAnAggregateConsumesTheSource) {
    analyze("struct Handle {\n    var id: Int\n    deinit() { }\n}\n"
            "struct Holder { var handle: Handle }\n"
            "func run() {\n"
            "    let a = Handle(id: 1)\n"
            "    let holder = Holder(handle: consume a)\n"
            "    let again = Holder(handle: consume a)\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, MovingIntoABindingConsumesTheSource) {
    analyze("struct Handle {\n    var id: Int\n    deinit() { }\n}\n"
            "func run() {\n"
            "    let a = Handle(id: 1)\n"
            "    let b = consume a\n"
            "    let c = consume a\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AMoveInOneBranchLeavesTheSourceConsumedAfterTheJoin) {
    analyze("struct Handle {\n    var id: Int\n    deinit() { }\n}\n"
            "func run(flag: Bool) {\n"
            "    let a = Handle(id: 1)\n"
            "    if flag { let b = consume a }\n"
            "    let c = consume a\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}
// A binding that a pattern moves out owns its value, so moving it onward ends its ownership.
TEST_F(SemanticAnalysisTest, MovingAMatchBindingOnwardConsumesIt) {
    analyze("struct Handle {\n    var id: Int\n    deinit() { }\n}\n"
            "enum Slot {\n    Full(Handle),\n    Empty,\n}\n"
            "func take(handle: consuming Handle) { }\n"
            "func run(slot: consuming Slot) {\n"
            "    match consume slot {\n"
            "        .Full(handle) => { take(handle: consume handle)\n take(handle: consume handle) },\n"
            "        .Empty => (),\n"
            "    }\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, MatchConsumeEndsTheScrutineesOwnership) {
    analyze("struct Handle {\n    var id: Int\n    deinit() { }\n}\n"
            "enum Slot {\n    Full(Handle),\n    Empty,\n}\n"
            "func run(slot: consuming Slot) {\n"
            "    match consume slot { .Full(_) => (), .Empty => () }\n"
            "    match consume slot { .Full(_) => (), .Empty => () }\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}
// Decision 0015, "Linear types": a linear value is never destroyed implicitly, so a binding that
// may still hold one at an exit of its scope is an error. The tests declare their own linear
// struct in a toolchain-authorized source (the `linear` modifier is not public syntax).
constexpr const char* linearPrelude =
        "linear struct Token {\n"
        "    var id: Int\n"
        "    consuming func finish(): Int {\n"
        "        let id = consume self.id\n"
        "        return id\n"
        "    }\n"
        "}\n"
        "func mint(): Token { return Token(id: 1) }\n"
        "func maybe(): Int? { return .None }\n";

TEST_F(SemanticAnalysisTest, AnEndedLinearValueIsAccepted) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(): Int {\n    let t = mint()\n    let n = consume t.finish()\n    return n\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ALinearValueLeftAtTheEndOfItsBlockIsReported) {
    analyzeToolchain(std::string(linearPrelude) + "func run() {\n    let t = mint()\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnEarlyReturnWithALiveLinearValueIsReported) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(flag: Bool): Int {\n"
                     "    let t = mint()\n"
                     "    if flag { return 0 }\n"
                     "    let n = consume t.finish()\n"
                     "    return n\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

// A consume on one branch only ends the value on that path: the merge must treat the value as live.
TEST_F(SemanticAnalysisTest, AConsumeOnOneBranchLeavesTheValueLiveAfterTheJoin) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(flag: Bool) {\n"
                     "    let t = mint()\n"
                     "    if flag { let n = consume t.finish() }\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AConsumeOnEveryBranchEndsTheValue) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(flag: Bool): Int {\n"
                     "    let t = mint()\n"
                     "    if flag { let a = consume t.finish()\n return a } else { let b = consume t.finish()\n return b }\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, PropagationWithALiveLinearValueIsAnEarlyExit) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(): Int? {\n"
                     "    let t = mint()\n"
                     "    let x = maybe()?\n"
                     "    let n = consume t.finish()\n"
                     "    return .Some(n + x)\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, PropagationBeforeTheLinearValueExistsIsFine) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(): Int? {\n"
                     "    let x = maybe()?\n"
                     "    let t = mint()\n"
                     "    let n = consume t.finish()\n"
                     "    return .Some(n + x)\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, BreakAndContinueWithALiveLinearValueAreExits) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(n: Int) {\n"
                     "    var i = 0\n"
                     "    while i < n {\n"
                     "        let t = mint()\n"
                     "        if i == 1 { break }\n"
                     "        let m = consume t.finish()\n"
                     "        i = i + 1\n"
                     "    }\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ALinearValueEndedInEveryIterationIsAccepted) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(n: Int) {\n"
                     "    var i = 0\n"
                     "    while i < n {\n"
                     "        let t = mint()\n"
                     "        let m = consume t.finish()\n"
                     "        i = i + 1\n"
                     "    }\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AssigningOverALiveLinearValueDropsIt) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run() {\n"
                     "    var t = mint()\n"
                     "    t = mint()\n"
                     "    let n = consume t.finish()\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearOverwrite))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AssigningIntoAnEndedLinearBindingReinitialisesIt) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run() {\n"
                     "    var t = mint()\n"
                     "    let a = consume t.finish()\n"
                     "    t = mint()\n"
                     "    let b = consume t.finish()\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearOverwrite))
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AStatementThatDropsALinearResultIsReported) {
    analyzeToolchain(std::string(linearPrelude) + "func run() {\n    mint()\n    let n = 1\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AConsumingParameterOfLinearTypeMustBeEnded) {
    analyzeToolchain(std::string(linearPrelude) + "func run(t: consuming Token) {\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(t: consuming Token): Int { let n = consume t.finish()\n return n }\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ABorrowedLinearParameterNeedsNoEnding) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func peek(t: Token): Int { return t.id }\n"
                     "func run(): Int {\n    let t = mint()\n    let n = peek(t: t)\n"
                     "    let m = consume t.finish()\n    return n + m\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ALinearAggregateEndsFieldByField) {
    analyzeToolchain(std::string(linearPrelude) +
                     "struct Pair {\n    var a: Token\n    var b: Token\n}\n"
                     "func missing(): Int {\n"
                     "    let p = Pair(a: mint(), b: mint())\n"
                     "    let x = consume p.a.finish()\n"
                     "    return x\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) +
                     "struct Pair {\n    var a: Token\n    var b: Token\n}\n"
                     "func both(): Int {\n"
                     "    let p = Pair(a: mint(), b: mint())\n"
                     "    let x = consume p.a.finish()\n"
                     "    let y = consume p.b.finish()\n"
                     "    return x + y\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AMatchMustBindEveryLinearPayload) {
    const std::string box = "enum Box {\n    Full(Token),\n    Empty,\n}\n";
    analyzeToolchain(std::string(linearPrelude) + box +
                     "func run(b: consuming Box) { match consume b { .Full(_) => (), .Empty => () } }\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) + box +
                     "func run(b: consuming Box) {\n"
                     "    match consume b {\n"
                     "        .Full(t) => { let n = consume t.finish() },\n"
                     "        .Empty => (),\n"
                     "    }\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ABoundLinearPayloadLeftLiveAtTheEndOfItsArmIsReported) {
    analyzeToolchain(std::string(linearPrelude) +
                     "enum Box {\n    Full(Token),\n    Empty,\n}\n"
                     "func run(b: consuming Box) {\n"
                     "    match consume b {\n"
                     "        .Full(t) => (),\n"
                     "        .Empty => (),\n"
                     "    }\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ReturningALinearLocalMovesItOut) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func make(): Token {\n    let t = mint()\n    return t\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ANonLinearProgramIsUnaffected) {
    analyze("struct Plain {\n    var id: Int\n}\n"
            "func run(): Int {\n    let p = Plain(id: 1)\n    return p.id\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed));
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearOverwrite));
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped));
}
// Review findings for the first linear implementation: statement tails, patterns, borrowed
// temporaries, and the join of a whole consume with per-field consumes.
TEST_F(SemanticAnalysisTest, TheLastExpressionOfAVoidFunctionDropsALinearValue) {
    analyzeToolchain(std::string(linearPrelude) + "func run() {\n    mint()\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, TheTailOfAnIfOrWhileStatementDropsALinearValue) {
    analyzeToolchain(std::string(linearPrelude) + "func run(c: Bool) {\n    if c { mint() }\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(n: Int) {\n    var i = 0\n    while i < n {\n        i = i + 1\n        mint()\n    }\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, TheTailOfAFunctionThatReturnsAValueIsMovedOn) {
    analyzeToolchain(std::string(linearPrelude) + "func make(): Token {\n    mint()\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) +
                     "func make(c: Bool): Token {\n    if c { mint() } else { mint() }\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AWildcardArmDropsALinearPayloadOfAMovedScrutinee) {
    const std::string box = "enum Box {\n    Full(Token),\n    Empty,\n}\n";
    analyzeToolchain(std::string(linearPrelude) + box +
                     "func run(b: consuming Box) { match consume b { .Empty => (), _ => () } }\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) + "func run() { match mint() { _ => () } }\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, SomeAndOkWildcardsDropALinearPayload) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(o: consuming Token?) { match consume o { .Some(_) => (), .None => () } }\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(r: consuming Result<Token, String>) {\n"
                     "    match consume r { .Ok(_) => (), .Err(e) => () }\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, PeekingAtABorrowedLinearEnumWithAWildcardIsAccepted) {
    analyzeToolchain(std::string(linearPrelude) +
                     "enum Box {\n    Full(Token),\n    Empty,\n}\n"
                     "func isFull(b: Box): Bool {\n"
                     "    return match b { .Full(_) => true, .Empty => false }\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ABorrowedLinearTemporaryIsDropped) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func peek(t: Token): Int { return t.id }\n"
                     "func run(): Int {\n    let n = peek(t: mint())\n    return n\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(): Int {\n    let n = mint().id\n    return n\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ALinearTemporaryMovedIntoAnAggregateIsNotBorrowed) {
    analyzeToolchain(std::string(linearPrelude) +
                     "struct Pair {\n    var a: Token\n    var b: Token\n}\n"
                     "func endPair(p: consuming Pair): Int {\n"
                     "    let x = consume p.a.finish()\n    let y = consume p.b.finish()\n    return x + y\n}\n"
                     "func run(): Int {\n    return endPair(p: consume Pair(a: mint(), b: mint()))\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

// `consume p` on one branch and `consume p.a`, `consume p.b` on the other end the same fields.
TEST_F(SemanticAnalysisTest, AWholeConsumeAndPerFieldConsumesJoinAsEnded) {
    analyzeToolchain(std::string(linearPrelude) +
                     "struct Pair {\n    var a: Token\n    var b: Token\n}\n"
                     "func endPair(p: consuming Pair): Int {\n"
                     "    let x = consume p.a.finish()\n    let y = consume p.b.finish()\n    return x + y\n}\n"
                     "func run(c: Bool): Int {\n"
                     "    let p = Pair(a: mint(), b: mint())\n"
                     "    if c {\n        let x = consume p.a.finish()\n        let y = consume p.b.finish()\n"
                     "    } else {\n        let z = endPair(p: consume p)\n    }\n"
                     "    return 0\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnAggregateCanBeRefilledAfterEveryLeafWasConsumed) {
    analyzeToolchain(std::string(linearPrelude) +
                     "struct Pair {\n    var a: Token\n    var b: Token\n}\n"
                     "func run() {\n"
                     "    var p = Pair(a: mint(), b: mint())\n"
                     "    let x = consume p.a.finish()\n    let y = consume p.b.finish()\n"
                     "    p = Pair(a: mint(), b: mint())\n"
                     "    let u = consume p.a.finish()\n    let v = consume p.b.finish()\n"
                     "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearOverwrite))
            << joyeer::analysis::dump(result.diagnostics);
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnAggregateWithOneLeafLeftCannotBeRefilled) {
    analyzeToolchain(std::string(linearPrelude) +
                     "struct Pair {\n    var a: Token\n    var b: Token\n}\n"
                     "func run() {\n"
                     "    var p = Pair(a: mint(), b: mint())\n"
                     "    let x = consume p.a.finish()\n"
                     "    p = Pair(a: mint(), b: mint())\n"
                     "    let u = consume p.a.finish()\n    let v = consume p.b.finish()\n"
                     "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearOverwrite))
            << joyeer::analysis::dump(result.diagnostics);
}


// `consume a.consumeEmpty()` consumes the receiver like a consuming method does.
TEST_F(SemanticAnalysisTest, ConsumeEmptyEndsTheArray) {
    analyze("func run(): Int {\n"
            "    var items: [Int] = []\n"
            "    consume items.consumeEmpty()\n"
            "    return items.count\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnArrayEndedByConsumeEmptyCanBeFilledAgain) {
    analyze("func run(): Int {\n"
            "    var items: [Int] = []\n"
            "    consume items.consumeEmpty()\n"
            "    items = [1]\n"
            "    return items.count\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}


// Control flow around `consume a.consumeEmpty()`: it consumes the binding like any consuming call.
TEST_F(SemanticAnalysisTest, ConsumeEmptyInALoopWithoutReinitialisationIsUseAfterConsume) {
    analyze("func run() {\n"
            "    var items: [Int] = []\n"
            "    var i = 0\n"
            "    while i < 2 {\n"
            "        consume items.consumeEmpty()\n"
            "        i = i + 1\n"
            "    }\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ConsumeEmptyInALoopThatRefillsIsAccepted) {
    analyze("func run() {\n"
            "    var items: [Int] = []\n"
            "    var i = 0\n"
            "    while i < 2 {\n"
            "        consume items.consumeEmpty()\n"
            "        items = []\n"
            "        i = i + 1\n"
            "    }\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ConsumeEmptyOnOneBranchLeavesTheBindingConsumedAfterTheJoin) {
    analyze("func run(flag: Bool): Int {\n"
            "    var items: [Int] = []\n"
            "    if flag {\n"
            "        consume items.consumeEmpty()\n"
            "    }\n"
            "    return items.count\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ConsumingOneArrayFieldLeavesItsSiblingReadable) {
    analyze("struct Box {\n    var xs: [Int]\n    var ys: [Int]\n}\n"
            "func run(): Int {\n"
            "    var box = Box(xs: [], ys: [1])\n"
            "    consume box.xs.consumeEmpty()\n"
            "    return box.ys.count\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
    analyze("struct Box {\n    var xs: [Int]\n    var ys: [Int]\n}\n"
            "func run(): Int {\n"
            "    var box = Box(xs: [], ys: [1])\n"
            "    consume box.xs.consumeEmpty()\n"
            "    return box.xs.count\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ConsumeEmptyEndsAnArrayOfLinearElements) {
    analyzeToolchain(std::string(linearPrelude) +
                     "func run() {\n    var tokens: [Token] = []\n    consume tokens.consumeEmpty()\n}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
    // An array of linear elements that is not ended is still reported.
    analyzeToolchain(std::string(linearPrelude) + "func run() {\n    var tokens: [Token] = []\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
    // Consumed on one path only: live on the other at the exit.
    analyzeToolchain(std::string(linearPrelude) +
                     "func run(flag: Bool) {\n    var tokens: [Token] = []\n"
                     "    if flag {\n        consume tokens.consumeEmpty()\n    }\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}


// Tasks (spec 21.3): a JoinHandle is linear, `spawn` consumes its input and `join` ends the handle.
constexpr const char* taskAnalysisPrelude =
        "func square(value: consuming Int): Int { return value * value }\n"
        "func label(name: consuming String): String { return name }\n";

TEST_F(SemanticAnalysisTest, AJoinedTaskIsAccepted) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run(): Int {\n"
            "    let h = spawn(function: square, input: consume 3)\n"
            "    return consume h.join()\n"
            "}\n");
    EXPECT_TRUE(result.diagnostics.empty()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ATaskThatIsNotJoinedIsLinearNotConsumed) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run() {\n    let h = spawn(function: square, input: consume 3)\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnEarlyReturnBetweenSpawnAndJoinLeavesTheTaskLive) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run(flag: Bool): Int {\n"
            "    let h = spawn(function: square, input: consume 3)\n"
            "    if flag { return 0 }\n"
            "    return consume h.join()\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, JoinOnBothBranchesEndsTheTask) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run(flag: Bool): Int {\n"
            "    let h = spawn(function: square, input: consume 3)\n"
            "    if flag { return consume h.join() }\n"
            "    return consume h.join()\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, ASpawnStatementDropsItsHandle) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run() {\n    spawn(function: square, input: consume 3)\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearResultDropped))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, OverwritingALiveTaskIsReported) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run(): Int {\n"
            "    var h = spawn(function: square, input: consume 3)\n"
            "    h = spawn(function: square, input: consume 4)\n"
            "    return consume h.join()\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearOverwrite))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, TheSpawnInputIsMovedIntoTheTask) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run(): Int {\n"
            "    let text = \"x\"\n"
            "    let h = spawn(function: label, input: consume text)\n"
            "    let again = text.count\n"
            "    let r = consume h.join()\n"
            "    return again + r.count\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::useAfterConsume))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AFanOutOverAnArrayOfTasksIsEndedByConsumeEmpty) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run(count: Int): Int {\n"
            "    var handles: [JoinHandle<Int>] = []\n"
            "    var index = 0\n"
            "    while index < count {\n"
            "        &handles.append(element: consume spawn(function: square, input: consume (index + 1)))\n"
            "        index = index + 1\n"
            "    }\n"
            "    var sum = 0\n"
            "    while true {\n"
            "        let popped = &handles.removeLast()\n"
            "        match consume popped {\n"
            "            .Some(handle) => { sum = sum + consume handle.join() }\n"
            "            .None => { break }\n"
            "        }\n"
            "    }\n"
            "    consume handles.consumeEmpty()\n"
            "    return sum\n"
            "}\n");
    EXPECT_TRUE(result.diagnostics.empty()) << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AnArrayOfTasksThatIsNotEndedIsReported) {
    analyze(std::string(taskAnalysisPrelude) +
            "func run() {\n    var handles: [JoinHandle<Int>] = []\n}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}


// Decision 0015 rule 3: a linear value moved into a call is pending until the call commits, so a `?`
// in a later operand would drop it on the error path.
constexpr const char* pendingMovePrelude =
        "func leaf(value: consuming Int): Int { return value }\n"
        "func fallible(ok: Bool): Result<Int, String> { return .Ok(1) }\n"
        "func take(handle: consuming JoinHandle<Int>, extra: Int): Int {\n"
        "    return consume handle.join() + extra\n"
        "}\n"
        "struct Holder {\n    var handle: JoinHandle<Int>\n    var extra: Int\n}\n";

TEST_F(SemanticAnalysisTest, AQuestionMarkAfterAMovedSpawnInTheSameCallIsReported) {
    analyze(std::string(pendingMovePrelude) +
            "func run(ok: Bool): Result<Int, String> {\n"
            "    let v = take(handle: consume spawn(function: leaf, input: consume 5), extra: fallible(ok: ok)?)\n"
            "    return .Ok(v)\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AQuestionMarkAfterAMovedHandleInTheSameCallIsReported) {
    analyze(std::string(pendingMovePrelude) +
            "func run(ok: Bool): Result<Int, String> {\n"
            "    let h = spawn(function: leaf, input: consume 5)\n"
            "    let v = take(handle: consume h, extra: fallible(ok: ok)?)\n"
            "    return .Ok(v)\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, AQuestionMarkAfterAHandleStoredInAnInitializerIsReported) {
    analyze(std::string(pendingMovePrelude) +
            "func run(ok: Bool): Result<Int, String> {\n"
            "    let holder = Holder(handle: spawn(function: leaf, input: consume 5), extra: fallible(ok: ok)?)\n"
            "    let value = consume holder.handle.join()\n"
            "    return .Ok(value)\n"
            "}\n");
    EXPECT_TRUE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

TEST_F(SemanticAnalysisTest, EvaluatingTheQuestionMarkFirstAndBindingItIsAccepted) {
    analyze(std::string(pendingMovePrelude) +
            "func run(ok: Bool): Result<Int, String> {\n"
            "    let extra = fallible(ok: ok)?\n"
            "    let v = take(handle: consume spawn(function: leaf, input: consume 5), extra: extra)\n"
            "    return .Ok(v)\n"
            "}\n");
    EXPECT_FALSE(hasDiagnostic(joyeer::analysis::DiagnosticId::linearNotConsumed))
            << joyeer::analysis::dump(result.diagnostics);
}

} // namespace
