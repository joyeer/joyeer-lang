#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/sourcefile.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/diagnostics/diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using joyeer::typing::TypeContext;
using joyeer::typing::TypeKind;

std::string readFixture(const std::string& relativePath) {
    const std::string path = std::string(JOYEER_TESTS_DIR) + "/" + relativePath;
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open type-checking fixture: " + path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

class TypeContextTest : public testing::Test {
protected:
    void SetUp() override {
        const auto root = std::make_shared<joyeer::syntax::SourceFileSyntax>(
                SourceSpan {},
                std::vector<joyeer::syntax::NodePtr> {});
        resolution = joyeer::semantic::NameResolver().resolve(root);
        ASSERT_TRUE(resolution.succeeded());
        ASSERT_NE(resolution.model, nullptr);
        types = std::make_unique<TypeContext>(*resolution.model);
    }

    const joyeer::typing::TypeRecord& type(joyeer::typing::TypeId id) const {
        const auto* result = types->type(id);
        EXPECT_NE(result, nullptr);
        return *result;
    }

    joyeer::semantic::NameResolutionResult resolution;
    std::unique_ptr<TypeContext> types;
};

TEST_F(TypeContextTest, RegistersConcreteBuiltinTypes) {
    EXPECT_EQ(type(types->voidType()).kind, TypeKind::voidType);
    EXPECT_EQ(type(types->neverType()).kind, TypeKind::never);
    EXPECT_EQ(type(types->anyType()).kind, TypeKind::any);
    EXPECT_EQ(type(types->intType()).kind, TypeKind::integer);
    EXPECT_EQ(type(types->boolType()).kind, TypeKind::boolean);
    EXPECT_EQ(type(types->stringType()).kind, TypeKind::string);
    EXPECT_EQ(type(types->uint8Type()).kind, TypeKind::uint8);

    const auto intSymbol = types->builtinSymbol("Int");
    ASSERT_TRUE(intSymbol.has_value());
    EXPECT_EQ(types->typeForSymbol(*intSymbol), types->intType());
    EXPECT_EQ(types->displayName(types->intType()), "Int");
}

TEST_F(TypeContextTest, InternsConcreteBuiltinGenericApplications) {
    const auto array = types->arrayType(types->intType());
    const auto optional = types->optionalType(types->stringType());
    const auto dictionary = types->dictionaryType(types->stringType(), array);
    const auto result = types->resultType(dictionary, optional);

    EXPECT_EQ(types->arrayType(types->intType()), array);
    EXPECT_EQ(types->optionalType(types->stringType()), optional);
    EXPECT_EQ(type(array).kind, TypeKind::array);
    EXPECT_EQ(type(array).arguments, std::vector { types->intType() });
    EXPECT_EQ(type(dictionary).kind, TypeKind::dictionary);
    EXPECT_EQ(type(result).kind, TypeKind::result);
    EXPECT_EQ(
            types->displayName(result),
            "Result<[String: [Int]], String?>");
}

TEST_F(TypeContextTest, RejectsInvalidTypeConstructorArityAndSymbols) {
    const auto arraySymbol = types->builtinSymbol("Array");
    const auto intSymbol = types->builtinSymbol("Int");
    ASSERT_TRUE(arraySymbol.has_value());
    ASSERT_TRUE(intSymbol.has_value());

    EXPECT_FALSE(types->typeForSymbol(*arraySymbol).has_value());
    EXPECT_FALSE(types->typeForSymbol(
            *arraySymbol,
            { types->intType(), types->stringType() }).has_value());
    EXPECT_FALSE(types->typeForSymbol(
            *intSymbol,
            { types->intType() }).has_value());
    EXPECT_FALSE(types->typeForSymbol(joyeer::semantic::invalidSymbolId).has_value());
}

TEST_F(TypeContextTest, PropagatesErrorTypesWithoutCreatingCompositeNoise) {
    const auto before = types->size();
    EXPECT_EQ(types->arrayType(types->errorType()), types->errorType());
    EXPECT_EQ(types->resultType(types->intType(), types->errorType()), types->errorType());
    EXPECT_EQ(types->size(), before);
    EXPECT_EQ(types->displayName(types->errorType()), "<error>");
}

    class TypeCheckingTest : public testing::Test {
    protected:
        void check(const std::string& text) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser lexer(&lexerDiagnostics);
        lexer.parse(source);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());

        joyeer::parser::Parser parser(source->tokens);
        parseResult = parser.parse();
        ASSERT_TRUE(parseResult.succeeded()) << joyeer::parser::dump(parseResult.diagnostics);

        resolution = joyeer::semantic::NameResolver().resolve(parseResult.root);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_NE(checking.model, nullptr);
        }

        // Checks `text` as a toolchain-authorized (runtime) source, where `linear` is allowed.
        void checkToolchain(const std::string& text) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser lexer(&lexerDiagnostics);
        lexer.parse(source);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());

        joyeer::parser::Parser parser(source->tokens);
        parseResult = parser.parse();
        ASSERT_TRUE(parseResult.succeeded()) << joyeer::parser::dump(parseResult.diagnostics);

        resolution = joyeer::semantic::NameResolver().resolve(
                { { "runtime.fixture", { parseResult.root }, true } });
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_NE(checking.model, nullptr);
        }

        joyeer::typing::TypeId declaredType(const joyeer::syntax::NodePtr& node) const {
        const auto symbol = resolution.model->declaredSymbol(node);
        EXPECT_TRUE(symbol.has_value());
        const auto type = symbol.has_value()
            ? checking.model->typeOf(*symbol)
            : std::optional<joyeer::typing::TypeId>();
        EXPECT_TRUE(type.has_value());
        return type.value_or(joyeer::typing::invalidTypeId);
        }

        bool hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId id) const {
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                [id](const auto& diagnostic) { return diagnostic.id == id; });
        }

        Diagnostics lexerDiagnostics;
        SourceFile::Ptr source;
        joyeer::parser::ParseResult parseResult;
        joyeer::semantic::NameResolutionResult resolution;
        joyeer::typing::TypeCheckingResult checking;
    };

    TEST_F(TypeCheckingTest, TypesMethodsReceiversAndCalls) {
        check("struct Counter {\n"
              "    var count: Int\n"
              "    func read(): Int { return self.count }\n"
              "    mutating func bump(by amount: Int) { &self.count += amount }\n"
              "    consuming func finish(): Int { return self.count }\n"
              "    func doubled(): Self { return Self(count: self.count * 2) }\n"
              "}\n"
              "extension Counter {\n"
              "    func isAbove(limit: Int): Bool { return self.count > limit }\n"
              "}\n"
              "func run() {\n"
              "    var c = Counter(count: 1)\n"
              "    let a: Int = c.read()\n"
              "    &c.bump(by: 2)\n"
              "    let d: Counter = c.doubled()\n"
              "    let big: Bool = c.isAbove(limit: 1)\n"
              "    let total: Int = consume c.finish()\n"
              "}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);

        const auto& model = *resolution.model;
        const auto counter = [&]() {
            for (const auto& symbol : model.symbols()) {
                if (symbol.kind == joyeer::semantic::SymbolKind::structure &&
                    symbol.name == "Counter") {
                    return *checking.model->typeOf(symbol.id);
                }
            }
            return joyeer::typing::invalidTypeId;
        }();
        for (const auto& symbol : model.symbols()) {
            if (symbol.kind != joyeer::semantic::SymbolKind::method || model.isPreludeSource(symbol.span)) continue;
            const auto* signature = checking.model->callable(symbol.id);
            ASSERT_NE(signature, nullptr) << symbol.name;
            EXPECT_EQ(signature->receiverType, std::optional<joyeer::typing::TypeId>(counter))
                    << symbol.name;
        }
        for (const auto& symbol : model.symbols()) {
            if (symbol.kind != joyeer::semantic::SymbolKind::method) continue;
            const auto* signature = checking.model->callable(symbol.id);
            if (symbol.name == "bump") EXPECT_EQ(signature->parameters.size(), 1u);
            if (symbol.name == "read") EXPECT_TRUE(signature->parameters.empty());
            if (symbol.name == "doubled") EXPECT_EQ(signature->result, counter);
        }
    }

    TEST_F(TypeCheckingTest, RequiresTheReceiverMarkerMatchingTheMethodEffect) {
        const std::string prelude =
                "struct C {\n"
                "    var n: Int\n"
                "    func read(): Int { return self.n }\n"
                "    mutating func bump() { &self.n += 1 }\n"
                "    consuming func done(): Int { return self.n }\n"
                "}\n";
        using Id = joyeer::typing::TypeCheckingDiagnosticId;

        check(prelude + "func run() { var c = C(n: 1)\n c.bump() }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::invalidAccessMarker));

        check(prelude + "func run() { var c = C(n: 1)\n let x = &c.read() }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::invalidAccessMarker));

        check(prelude + "func run() { var c = C(n: 1)\n let x = c.done() }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::invalidConsumeArgument));

        check(prelude + "func run() { var c = C(n: 1)\n let x = consume c.read() }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::invalidConsumeArgument));

        check(prelude + "func run() { let c = C(n: 1)\n &c.bump() }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::assignmentToImmutable));
    }

    TEST_F(TypeCheckingTest, MethodCallsNeedAValueReceiverAndMatchingLabels) {
        using Id = joyeer::typing::TypeCheckingDiagnosticId;
        check("struct C {\n"
              "    var n: Int\n"
              "    func read(): Int { return self.n }\n"
              "}\n"
              "func run() { let x = C.read() }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::invalidCallArguments));
    }

    TEST_F(TypeCheckingTest, ReceiverAccessParticipatesInCallExclusivity) {
        using Id = joyeer::typing::TypeCheckingDiagnosticId;
        const std::string prelude =
                "struct C {\n"
                "    var n: Int\n"
                "    func read(): Int { return self.n }\n"
                "    mutating func add(by v: Int) { &self.n += v }\n"
                "    mutating func merge(other: C) { &self.n += other.n }\n"
                "    func plus(other: C): Int { return self.n + other.n }\n"
                "}\n";

        check(prelude + "func run() { var c = C(n: 1)\n &c.add(by: c.read()) }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::overlappingAccess));

        check(prelude + "func run() { var c = C(n: 1)\n &c.merge(other: c) }\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::overlappingAccess));

        // Two borrows of one value, and a mutation of a distinct value, are fine.
        check(prelude + "func run(): Int { let c = C(n: 1)\n return c.plus(other: c) }\n");
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        check(prelude + "func run() { var a = C(n: 1)\n let b = C(n: 2)\n &a.merge(other: b) }\n");
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }

    TEST_F(TypeCheckingTest, BorrowingReceiversAreImmutableInsideTheMethod) {
        using Id = joyeer::typing::TypeCheckingDiagnosticId;
        check("struct C {\n"
              "    var n: Int\n"
              "    func set() { &self.n = 1 }\n"
              "}\n");
        EXPECT_TRUE(hasMethodDiagnostic(Id::assignmentToImmutable));
    }

    TEST_F(TypeCheckingTest, MethodsOnEnumsAndChainedTemporaries) {
        check("struct C {\n"
              "    var n: Int\n"
              "    func doubled(): Self { return Self(n: self.n * 2) }\n"
              "    func read(): Int { return self.n }\n"
              "}\n"
              "enum Light {\n"
              "    Red,\n"
              "    Green\n"
              "    func other(): Self { return Self.Green }\n"
              "}\n"
              "func run(): Int {\n"
              "    let light: Light = Light.Red\n"
              "    let next: Light = light.other()\n"
              "    return C(n: 4).doubled().read()\n"
              "}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }

    TEST_F(TypeCheckingTest, DictionaryGetInfersExactOptionalFromReceiver) {
        check("func lookup(values: [String: Int?], units: [UInt8: Void], flags: [Bool: Int]) {\n"
              "let nested = values.get(key: \"answer\")\n"
              "let unit = units.get(key: b'X')\n"
              "let flag = flags.get(key: true)\n"
              "let byte = units.get(key: b'A')\n"
              "}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[0]);
        auto& types = checking.model->types();
        const auto* nested = types.type(declaredType(function->body->items[0]));
        ASSERT_NE(nested, nullptr);
        EXPECT_EQ(nested->kind, joyeer::typing::TypeKind::optional);
        ASSERT_EQ(nested->arguments.size(), 1u);
        const auto* payload = types.type(nested->arguments[0]);
        ASSERT_NE(payload, nullptr);
        EXPECT_EQ(payload->kind, joyeer::typing::TypeKind::optional);
        EXPECT_EQ(payload->arguments, std::vector<joyeer::typing::TypeId> { types.intType() });
        EXPECT_EQ(types.displayName(declaredType(function->body->items[1])), "Void?");
        EXPECT_EQ(types.displayName(declaredType(function->body->items[2])), "Int?");
        EXPECT_EQ(declaredType(function->body->items[1]), declaredType(function->body->items[3]));
    }

    TEST_F(TypeCheckingTest, RejectsDictionaryGetTypeAndAccessErrors) {
        for (const auto* source : {
                 "func run(values: [String: Int]): Int? { return values.get(key: 1) }\n",
                 "func run(values: [String: Int]): String? { return values.get(key: \"x\") }\n",
                 "func run(values: [String: Int]): Int { return values.get(key: \"x\") }\n",
                 "func run(values: [String: Int?]): Int? { return values.get(key: \"x\") }\n",
                 "func run(values: inout [String: Int]) { &values.get(key: \"x\") }\n",
                 "func run(values: [String: Int], key: inout String) { values.get(key: &key) }\n",
                 "func run(values: [String: Int], key: consuming String) { values.get(key: consume key) }\n",
                 "func run(values: [UInt8: Int]) { values.get(key: 256) }\n",
             }) {
            SCOPED_TRACE(source);
            ASSERT_NO_FATAL_FAILURE(check(source));
            EXPECT_FALSE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        }
    }

    TEST_F(TypeCheckingTest, DictionaryGetBorrowsReceiverThroughoutKeyEvaluation) {
        check("func change(values: inout [String: String]): String {\n"
              "&values[\"name\"] = \"changed\"\nreturn \"name\"\n}\n"
              "func run(values: inout [String: String]): String? {\n"
              "return values.get(key: change(values: &values))\n}\n");
        EXPECT_FALSE(checking.succeeded());
        EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess;
                })) << joyeer::typing::dump(checking.diagnostics);
    }

    TEST_F(TypeCheckingTest, RejectsMalformedDeferredDictionaryGetCalls) {
        for (const auto* arguments : {
                 "()",
                 "(\"x\")",
                 "(value: \"x\")",
                 "(key: \"x\", key: \"y\")",
                 "(key: \"x\", extra: 1)",
             }) {
            SCOPED_TRACE(arguments);
            ASSERT_NO_FATAL_FAILURE(check(
                    "func run(values: [Int: [String: Int]]): Int? {\n"
                    "return values[0].get" + std::string(arguments) + "\n}\n"));
            EXPECT_FALSE(checking.succeeded());
            ASSERT_FALSE(checking.diagnostics.empty());
            const auto& diagnostic = checking.diagnostics.front();
            EXPECT_EQ(diagnostic.id, joyeer::typing::TypeCheckingDiagnosticId::invalidCallArguments)
                    << joyeer::typing::dump(checking.diagnostics);
            EXPECT_STREQ(joyeer::typing::diagnosticName(diagnostic.id),
                    "type-checking.invalid-call-arguments");
            EXPECT_GT(diagnostic.span.length, 0u);
            if (std::string(arguments) == "(value: \"x\")") {
                EXPECT_EQ(diagnostic.span.offset, source->content.find("value:"));
                EXPECT_EQ(diagnostic.message, "call target 'get' has no parameter labeled 'value:'");
            }
        }
    }

    TEST_F(TypeCheckingTest, DeferredAppendUsesSharedCallArgumentValidation) {
        check("func run(values: inout [[Int]]) {\n"
              "&values[0].append(element: 1)\n}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);

        check("func run(values: inout [[Int]]) {\n"
              "&values[0].append(value: 1)\n}\n");
        EXPECT_FALSE(checking.succeeded());
        ASSERT_FALSE(checking.diagnostics.empty());
        EXPECT_EQ(checking.diagnostics.front().id,
                joyeer::typing::TypeCheckingDiagnosticId::invalidCallArguments)
                << joyeer::typing::dump(checking.diagnostics);
    }

    TEST_F(TypeCheckingTest, DeferredDictionaryGetPreservesArgumentTyping) {
        check("func run(values: [Int: [String: Int?]]): Optional<Int?> {\n"
              "return values[0].get(key: \"x\")\n}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);

        check("func run(values: [Int: [String: Int]]): Int? {\n"
              "return values[0].get(key: 1)\n}\n");
        EXPECT_FALSE(checking.succeeded());
        EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch;
                })) << joyeer::typing::dump(checking.diagnostics);
    }

    TEST_F(TypeCheckingTest, UnitValuesUseExistingVoidType) {
        check("func make(): Result<Void, String> { return .Ok(()) }\n"
              "func run(value: ()): Void {\n"
              "let local = ()\n"
              "let units: [Void] = [local, value]\n"
              "let optional: Void? = ()\n"
              "match make() { .Ok(()) => (), .Err(_) => () }\n"
              "}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto run = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[1]);
        EXPECT_EQ(declaredType(run->parameters[0]), checking.model->types().voidType());
        EXPECT_EQ(declaredType(run->body->items[0]), checking.model->types().voidType());
    }

    TEST_F(TypeCheckingTest, TypesResultAndOptionalPropagationWithoutFlattening) {
        check(R"JOYEER(func change(value: Result<Int, String>): Result<String, String> {
let amount: Int = value?
return .Ok("done")
}
func optional(value: Int?): String? {
let amount: Int = value?
return "done"
}
func nested(value: Result<Result<Int, String>, String>): Result<Int, String> {
return value?
}
func twice(value: Result<Result<Int, String>, String>): Result<Int, String> {
let number: Int = (value?)?
return .Ok(number)
}
func unit(value: Result<Void, String>): Result<Int, String> {
let done: Void = value?
return .Ok(1)
}
func contextual(): Result<Int, String> {
let number: Int = .Ok(42)?
return .Ok(number)
}
func contextualOptional(): Int? {
let number: Int = .Some(9)?
return number
}
func length(value: Result<String, String>): Result<Int, String> {
let count = (value?).count
return .Ok(count)
}
)JOYEER");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto change = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[0]);
        const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                change->body->items[0]);
        EXPECT_EQ(checking.model->typeOf(binding->initializer), checking.model->types().intType());
        const auto nested = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[2]);
        const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                nested->body->items[0]);
        EXPECT_EQ(
                checking.model->typeOf(returned->value),
                checking.model->types().resultType(
                        checking.model->types().intType(),
                        checking.model->types().stringType()));
    }

    TEST_F(TypeCheckingTest, PropagatedOperandsKeepCallExclusivityChecks) {
        check("func select(value: Int): Int? { return value }\n"
              "func update(dst: inout Int, src: Int) { &dst = src }\n"
              "func run(value: inout Int): Int? {\n"
              "update(dst: &value, src: select(value: value)?)\n"
              "return value\n"
              "}\n");
        EXPECT_FALSE(checking.succeeded());
        EXPECT_TRUE(std::any_of(
                checking.diagnostics.begin(),
                checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id ==
                            joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess;
                })) << joyeer::typing::dump(checking.diagnostics);
    }

    TEST_F(TypeCheckingTest, RejectsInvalidPropagationKindsAndContexts) {
        using joyeer::typing::TypeCheckingDiagnosticId;
        struct Case {
            const char* source;
            TypeCheckingDiagnosticId diagnostic;
        };
        for (const auto& test : std::vector<Case> {
                 { "func run(): Int? { return .Some(42?) }\n",
                   TypeCheckingDiagnosticId::invalidPropagationOperand },
                 { "func load(): Result<String, IOError> { return .Ok(\"x\") }\n"
                   "func main(args: [String]): Int {\n"
                   "let value = load()?\n"
                   "return 0\n"
                   "}\n",
                   TypeCheckingDiagnosticId::invalidPropagationContext },
                 { "func run(value: Int?): Result<Int, String> { return .Ok(value?) }\n",
                   TypeCheckingDiagnosticId::invalidPropagationContext },
                 { "func run(value: Result<Int, String>): Int? { return value? }\n",
                   TypeCheckingDiagnosticId::invalidPropagationContext },
                 { "let source: Int? = 1\nlet value = source?\n",
                   TypeCheckingDiagnosticId::invalidPropagationContext },
                 { "func run(value: Result<Int, String>): Result<Int, String?> { return .Ok(value?) }\n",
                   TypeCheckingDiagnosticId::mismatchedPropagationError },
             }) {
            SCOPED_TRACE(test.source);
            check(test.source);
            EXPECT_FALSE(checking.succeeded());
            EXPECT_TRUE(std::any_of(
                    checking.diagnostics.begin(),
                    checking.diagnostics.end(),
                    [&test](const auto& diagnostic) {
                        return diagnostic.id == test.diagnostic;
                    })) << joyeer::typing::dump(checking.diagnostics);
        }
    }

    TEST_F(TypeCheckingTest, RejectsInvalidUnitPayloadsAndPatterns) {
        for (const auto* text : {
                "func run(): Result<Void, String> { return .Ok(42) }\n",
                "func run(): Int { return () }\n",
                "func run(value: Int): Bool { return match value { () => true } }\n",
                "func run(value: Void?): Bool { return match value { () => true, _ => false } }\n",
                "func run(): Result<Int, String> { return .Ok(()) }\n"}) {
            SCOPED_TRACE(text);
            check(text);
            EXPECT_FALSE(checking.succeeded());
            EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                    [](const auto& diagnostic) {
                        return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch;
                    })) << joyeer::typing::dump(checking.diagnostics);
        }
    }

    TEST_F(TypeCheckingTest, UnitEqualityIsRejectedRatherThanLoweringInvalidLLVM) {
        check("func compare(): Bool { return () == () }\n");
        EXPECT_FALSE(checking.succeeded());
        EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id ==
                            joyeer::typing::TypeCheckingDiagnosticId::invalidOperatorOperands;
                }));
    }

    TEST_F(TypeCheckingTest, ResolvesExactCompositeDeclarationTypes) {
        check(R"JOYEER(struct Packet {
    var values: [Result<Int, String?>]
    }
    func decode(input: [String: UInt8?]): Result<[Int], String> {
    }
    )JOYEER");

        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto packet = std::static_pointer_cast<joyeer::syntax::StructDeclSyntax>(
            parseResult.root->items[0]);
        const auto decode = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[1]);

        EXPECT_EQ(
            checking.model->types().displayName(declaredType(packet->fields[0])),
            "[Result<Int, String?>]");
        EXPECT_EQ(
            checking.model->types().displayName(declaredType(decode->parameters[0])),
            "[String: UInt8?]");

        const auto functionSymbol = resolution.model->declaredSymbol(decode);
        ASSERT_TRUE(functionSymbol.has_value());
        const auto* signature = checking.model->callable(*functionSymbol);
        ASSERT_NE(signature, nullptr);
        ASSERT_EQ(signature->parameters.size(), 1u);
        EXPECT_EQ(
            checking.model->types().displayName(signature->result),
            "Result<[Int], String>");
        EXPECT_EQ(checking.model->typeOf(decode->returnType), signature->result);
    }

    TEST_F(TypeCheckingTest, ResolvesNominalTypesAndSynthesizedSignatures) {
        check(R"JOYEER(struct Item { var count: Int }
    enum Value { Item(Item), Empty, }
    func wrap(item: Item): Value {
    }
    )JOYEER");

        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto item = std::static_pointer_cast<joyeer::syntax::StructDeclSyntax>(
            parseResult.root->items[0]);
        const auto value = std::static_pointer_cast<joyeer::syntax::EnumDeclSyntax>(
            parseResult.root->items[1]);
        const auto wrap = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[2]);

        EXPECT_EQ(
            checking.model->types().type(declaredType(item))->kind,
            TypeKind::structure);
        EXPECT_EQ(
            checking.model->types().type(declaredType(value))->kind,
            TypeKind::enumeration);

        const auto itemSymbol = resolution.model->declaredSymbol(item);
        ASSERT_TRUE(itemSymbol.has_value());
        const auto* itemSemanticSymbol = resolution.model->symbol(*itemSymbol);
        ASSERT_NE(itemSemanticSymbol, nullptr);
        ASSERT_TRUE(itemSemanticSymbol->synthesizedInitializer.has_value());
        const auto* initializer = checking.model->callable(
            *itemSemanticSymbol->synthesizedInitializer);
        ASSERT_NE(initializer, nullptr);
        EXPECT_EQ(
            checking.model->types().displayName(initializer->result),
            "Item");

        const auto caseSymbol = resolution.model->declaredSymbol(value->cases[0]);
        ASSERT_TRUE(caseSymbol.has_value());
        const auto* enumCase = checking.model->callable(*caseSymbol);
        ASSERT_NE(enumCase, nullptr);
        ASSERT_EQ(enumCase->parameters.size(), 1u);
        EXPECT_EQ(checking.model->types().displayName(enumCase->parameters[0]), "Item");

        const auto functionSymbol = resolution.model->declaredSymbol(wrap);
        ASSERT_TRUE(functionSymbol.has_value());
        EXPECT_EQ(
            checking.model->types().displayName(
                checking.model->callable(*functionSymbol)->result),
            "Value");
    }

    TEST_F(TypeCheckingTest, DefaultsOmittedFunctionReturnTypeToVoid) {
        check(R"JOYEER(func log(value: String) {
    }
    )JOYEER");

        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
        const auto functionSymbol = resolution.model->declaredSymbol(function);
        ASSERT_TRUE(functionSymbol.has_value());
        EXPECT_EQ(
            checking.model->callable(*functionSymbol)->result,
            checking.model->types().voidType());
    }

    TEST_F(TypeCheckingTest, DiagnosesInvalidTypeArgumentCounts) {
        check(R"JOYEER(struct Box { var value: Int }
    func broken(first: Array, second: Int<String>, third: Result<Int>, fourth: Box<Int>) {
    }
    )JOYEER");

        ASSERT_EQ(checking.diagnostics.size(), 4u)
            << joyeer::typing::dump(checking.diagnostics);
        EXPECT_TRUE(std::all_of(
            checking.diagnostics.begin(),
            checking.diagnostics.end(),
            [](const auto& diagnostic) {
            return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                invalidTypeArgumentCount;
            }));
    }

TEST_F(TypeCheckingTest, InfersLiteralAndNameBindingTypes) {
    check(R"JOYEER(func values() {
let integer = 42
let boolean = true
let string = "text"
let byte = b'x'
let copy = integer
}
)JOYEER");

    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    ASSERT_EQ(function->body->items.size(), 5u);
    const std::vector<std::string> expected {
        "Int", "Bool", "String", "UInt8", "Int",
    };
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[index]);
        EXPECT_EQ(
                checking.model->types().displayName(declaredType(binding)),
                expected[index]);
        EXPECT_EQ(checking.model->typeOf(binding->initializer), declaredType(binding));
    }
}

            TEST_F(TypeCheckingTest, TypesStringUtf8AsOwnedByteArray) {
            check(R"JOYEER(func bytes(text: String): [UInt8] {
            return text.utf8()
        }
        )JOYEER");

            ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
            const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[0]);
            const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                function->body->items[0]);
            const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                returned->value);
            ASSERT_TRUE(checking.model->typeOf(call).has_value());
            EXPECT_EQ(
                checking.model->types().displayName(*checking.model->typeOf(call)),
                "[UInt8]");
            const auto target = checking.model->callTarget(call);
            ASSERT_TRUE(target.has_value());
            const auto* signature = checking.model->callable(*target);
            ASSERT_NE(signature, nullptr);
            EXPECT_TRUE(signature->parameters.empty());
            EXPECT_EQ(
                checking.model->types().displayName(signature->result),
                "[UInt8]");
            }

    TEST_F(TypeCheckingTest, TypesBuiltinIOErrorCasesAndExhaustiveMatch) {
        check(R"JOYEER(func errorCode(error: IOError): Int {
    return match error {
        .NotFound(code) => code,
        .PermissionDenied(code) => code,
        .InvalidPath(code) => code,
        .Other(code) => code,
    }
}
)JOYEER");

        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[0]);
        const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                function->body->items[0]);
        EXPECT_EQ(checking.model->typeOf(returned->value), checking.model->types().intType());
    }

    TEST_F(TypeCheckingTest, TypesExplicitByteConversions) {
        check(R"JOYEER(func convert(value: UInt8) {
    let integer = byteToInt(value: value)
    let text = byteToString(value: value)
    print(value: integer)
    print(value: text)
    }
    )JOYEER");

        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[0]);
        const auto integer = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[0]);
        const auto text = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[1]);
        EXPECT_EQ(checking.model->types().displayName(declaredType(integer)), "Int");
        EXPECT_EQ(checking.model->types().displayName(declaredType(text)), "String");
    }

    TEST_F(TypeCheckingTest, RejectsNonByteConversionArguments) {
        check(R"JOYEER(func invalid() {
    let nonByte: Int = 1
    let integer = byteToInt(value: nonByte)
    let text = byteToString(value: "x")
    }
    )JOYEER");

        ASSERT_EQ(checking.diagnostics.size(), 2u)
                << joyeer::typing::dump(checking.diagnostics);
        EXPECT_TRUE(std::all_of(
                checking.diagnostics.begin(),
                checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch;
                }));
    }

TEST_F(TypeCheckingTest, ContextuallyTypesEmptyCollectionsNilAndOptionalPromotion) {
    check(R"JOYEER(func values() {
let integers: [Int] = []
let lookup: [String: Int] = [:]
let absent: Int? = nil
let present: Int? = 42
}
)JOYEER");

    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    const std::vector<std::string> expected {
        "[Int]", "[String: Int]", "Int?", "Int?",
    };
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[index]);
        EXPECT_EQ(
                checking.model->types().displayName(declaredType(binding)),
                expected[index]);
    }
}

TEST_F(TypeCheckingTest, DiagnosesBindingAndCollectionElementMismatches) {
    check(R"JOYEER(func invalid() {
let text: String = 42
let values: [Int] = [1, "two", 3]
let lookup: [String: Int] = ["one": 1, "two": false]
}
)JOYEER");

    ASSERT_EQ(checking.diagnostics.size(), 3u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(std::all_of(
            checking.diagnostics.begin(),
            checking.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch &&
                        diagnostic.help.has_value();
            }));
}

TEST_F(TypeCheckingTest, DiagnosesAmbiguousLiteralsWithoutContext) {
    check(R"JOYEER(func ambiguous() {
let array = []
let dictionary = [:]
let absent = nil
}
)JOYEER");

    ASSERT_EQ(checking.diagnostics.size(), 3u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(std::all_of(
            checking.diagnostics.begin(),
            checking.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                        missingContextualType;
            }));
}

TEST_F(TypeCheckingTest, TypesM006OperatorsAndLoopControl) {
    check(R"JOYEER(func run(flag: Bool, a: Int, b: Int): Int {
let negated = !!flag
let choice = flag || !flag && negated
var total = a / b + a % b
while choice {
    if flag { break }
    total = total + 1
    continue
}
return total
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsM006WrongOperandTypes) {
    for (const auto* expression : { "!1", "true || 1", "1 || false",
                                    "true / false", "byteToInt(value: b'A') % b'A'", "\"x\" / \"y\"" }) {
        SCOPED_TRACE(expression);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run() { let value = " + std::string(expression) + "\n}\n"));
        EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id ==
                            joyeer::typing::TypeCheckingDiagnosticId::invalidOperatorOperands;
                })) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, RejectsLoopControlOutsideCurrentFunctionLoop) {
    for (const auto* statement : { "break", "continue" }) {
        SCOPED_TRACE(statement);
        ASSERT_NO_FATAL_FAILURE(check(
                "func inside() { while true { " + std::string(statement) + " } }\n"
                "func outside() { if true { " + std::string(statement) + " } }\n"));
        ASSERT_EQ(checking.diagnostics.size(), 1u)
                << joyeer::typing::dump(checking.diagnostics);
        EXPECT_EQ(checking.diagnostics[0].id,
                joyeer::typing::TypeCheckingDiagnosticId::invalidLoopControl);
    }
}

TEST_F(TypeCheckingTest, AcceptsBitwiseOperatorsOnIntegers) {
    check(R"JOYEER(func run(a: UInt32, b: UInt32, count: UInt8, signed: Int) {
    let x: UInt32 = a & b | a ^ ~b
    let y: UInt32 = a << count
    let z: Int = signed >> 2
    let w: Bool = (signed & 1) == 0
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsBitwiseOperatorsOnNonIntegersOrMixedTypes) {
    for (const auto* expression : { "true & false", "1.5 | 2.5", "~1.5", "~true",
                                    "1.5 << 1", "1 << 1.5", "narrow & wide", "\"x\" ^ \"y\"" }) {
        SCOPED_TRACE(expression);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run(narrow: UInt8, wide: UInt32) { let value = " +
                std::string(expression) + "\n}\n"));
        EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                [](const auto& diagnostic) {
                    return diagnostic.id ==
                            joyeer::typing::TypeCheckingDiagnosticId::invalidOperatorOperands;
                })) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, AcceptsIntegerConversions) {
    check(R"JOYEER(func run(a: Int, b: UInt8, c: Int8) {
    let x: UInt32 = a as UInt32
    let y: Int = b as Int
    let z: UInt8 = c &as UInt8
    let w: UInt8 = 255 as UInt8
    let v: Int64 = a as Int64
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsInvalidConversions) {
    for (const auto* expression : { "1.5 as Int", "true as Int", "a as Bool", "a as String",
                                    "300 as UInt8", "-1 as UInt", "f as Int" }) {
        SCOPED_TRACE(expression);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run(a: Int, f: Double) { let value = " +
                std::string(expression) + "\n}\n"));
        EXPECT_FALSE(checking.succeeded());
    }
}

TEST_F(TypeCheckingTest, AcceptsCompoundAssignments) {
    check(R"JOYEER(struct Box { var total: Int }
func run(count: UInt8) {
    var n = 1
    n += 2
    n -= 1
    n *= 3
    n /= 2
    n %= 4
    var bits: UInt32 = 1
    bits |= 2
    bits &= 3
    bits ^= 1
    bits <<= count
    bits >>= 1
    var text = "a"
    text += "b"
    var box = Box(total: 1)
    box.total += 1
    var d = 1.5
    d *= 2.0
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsInvalidCompoundAssignments) {
    for (const auto* statement : { "flag += true", "text -= \"b\"", "bits <<= 1.5",
                                   "n %= 1.5", "n += wide", "d %= 2.0", "n &= flag" }) {
        SCOPED_TRACE(statement);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run(wide: UInt32) {\n"
                "var flag = true\nvar text = \"a\"\nvar bits: UInt32 = 1\n"
                "var n = 1\nvar d = 1.5\n" + std::string(statement) + "\n}\n"));
        EXPECT_FALSE(checking.succeeded());
    }
}

TEST_F(TypeCheckingTest, RejectsCompoundAssignmentToDictionaryElements) {
    check("func run() {\nvar d: [String: Int] = [\"a\": 1]\n&d[\"a\"] += 1\n}\n");
    EXPECT_FALSE(checking.succeeded());
}

TEST_F(TypeCheckingTest, TypesTryConversionsAsResultsOfConversionError) {
    check(R"JOYEER(func narrow(value: Int): Result<UInt8, ConversionError> {
    let byte = (value as? UInt8)?
    return .Ok(byte)
}
func run(small: Int8, big: UInt64) {
    let a: Result<UInt32, ConversionError> = small as? UInt32
    let b = big as? Int
    let c: Result<Int8, ConversionError> = 300 as? Int8
    match b {
        .Ok(value) => { print(value: value) },
        .Err(.OutOfRange) => { print(value: "range") },
    }
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsInvalidTryConversions) {
    for (const auto* expression : { "1.5 as? Int", "true as? Int", "a as? Bool", "a as? String",
                                    "a as? UInt8 + 1", "f as? Int" }) {
        SCOPED_TRACE(expression);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run(a: Int, f: Double) { let value = " + std::string(expression) + "\n}\n"));
        EXPECT_FALSE(checking.succeeded());
    }
}

TEST_F(TypeCheckingTest, AcceptsForInOverArraysAndRanges) {
    check(R"JOYEER(struct Box { let value: Int }
func run(items: [String], limit: UInt8) {
    var total = 0
    for item in items { print(value: item) }
    for box in [Box(value: 1)] { total += box.value }
    for i in 0..<10 { total += i }
    for i in 1...total { total += i }
    for k in 0..<limit { print(value: k) }
    for byte in "abc" { print(value: byte) }
    for _ in 0..<3 { }
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsInvalidForInUses) {
    for (const auto* statement : { "for x in 5 { }", "for x in [1] { x = 2 }",
                                   "for k in 0..<limit { let n: Int = k }",
                                   "for s in \"a\"..<\"c\" { }", "for x in [1] { break }\nbreak" }) {
        SCOPED_TRACE(statement);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run(limit: UInt8) {\n" + std::string(statement) + "\n}\n"));
        EXPECT_FALSE(checking.succeeded());
    }
}

TEST_F(TypeCheckingTest, ReportsOneNotIterableDiagnosticForNonSequenceSources) {
    for (const auto* source : { "5", "true", "1.5", "[1: 2]" }) {
        SCOPED_TRACE(source);
        ASSERT_NO_FATAL_FAILURE(check(
                "func run() {\nfor x in " + std::string(source) + " { }\n}\n"));
        ASSERT_EQ(checking.diagnostics.size(), 1u) << joyeer::typing::dump(checking.diagnostics);
        EXPECT_EQ(checking.diagnostics[0].id,
                  joyeer::typing::TypeCheckingDiagnosticId::notIterable);
    }
}

TEST_F(TypeCheckingTest, LoopExitBranchesAndLogicalOperandsCanDiverge) {
    check(R"JOYEER(func run(flag: Bool) {
while if flag { break } else { true } {
    let value = if flag { continue } else { 7 }
    let done = flag || if flag { break } else { continue }
    print(value: value)
}
}
)JOYEER");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, TypesMvpUnaryBinaryAndLogicalOperators) {
    check(R"JOYEER(func operators() {
let arithmetic = 1 + 2 * 3
let concatenated = "left" + "right"
let ordered = 1 < 2
let bytesEqual = b'a' != b'b'
let conjunction = true && false
let negative = -1
}
)JOYEER");

    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    const std::vector<std::string> expected {
        "Int", "String", "Bool", "Bool", "Bool", "Int",
    };
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[index]);
        EXPECT_EQ(
                checking.model->types().displayName(declaredType(binding)),
                expected[index]);
    }
}

TEST_F(TypeCheckingTest, DiagnosesInvalidOperatorOperands) {
    check(R"JOYEER(func invalid() {
let mixed = 1 + "two"
let strings = "one" - "two"
let logical = 1 && 2
let negated = -"text"
}
)JOYEER");

    ASSERT_EQ(checking.diagnostics.size(), 4u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(std::all_of(
            checking.diagnostics.begin(),
            checking.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                        invalidOperatorOperands;
            }));
}

TEST_F(TypeCheckingTest, ChecksControlFlowConditionsAndReturnValues) {
    check(R"JOYEER(func valid(flag: Bool): Int {
if flag { return 1 }
return 2
}
func invalid(): Int {
while 1 { }
return "wrong"
}
)JOYEER");

    ASSERT_EQ(checking.diagnostics.size(), 2u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(std::all_of(
            checking.diagnostics.begin(),
            checking.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch;
            }));
}

TEST_F(TypeCheckingTest, UnifiesIfExpressionBranchesAndNever) {
    check(R"JOYEER(func choose(flag: Bool): Int {
let selected = if flag { 1 } else { 2 }
let returned = if flag { return 3 } else { 4 }
return selected + returned
}
)JOYEER");

    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    for (size_t index = 0; index < 2; ++index) {
        const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[index]);
        EXPECT_EQ(
                checking.model->types().displayName(declaredType(binding)),
                "Int");
    }
}

TEST_F(TypeCheckingTest, DiagnosesMismatchedIfExpressionBranches) {
    check(R"JOYEER(func choose(flag: Bool) {
let selected = if flag { 1 } else { "two" }
}
)JOYEER");

    ASSERT_EQ(checking.diagnostics.size(), 1u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_EQ(
            checking.diagnostics[0].id,
            joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
}

        TEST_F(TypeCheckingTest, ChecksFunctionStructEnumAndBuiltinCalls) {
            check(R"JOYEER(struct Box {
        var value: Int
        var names: [String] = []
        }
        enum Value { Number(Int), Text(String), }
        func identity(value: Int): Int { return value }
        func use(): Int {
        let box = Box(value: 42)
        let copied = identity(value: box.value)
        let wrapped = Value.Number(copied)
        print(value: wrapped)
        return copied
        }
        )JOYEER");

            ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
            const auto use = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[3]);
            const auto box = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                use->body->items[0]);
            const auto copied = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                use->body->items[1]);
            const auto wrapped = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                use->body->items[2]);
            EXPECT_EQ(checking.model->types().displayName(declaredType(box)), "Box");
            EXPECT_EQ(checking.model->types().displayName(declaredType(copied)), "Int");
            EXPECT_EQ(checking.model->types().displayName(declaredType(wrapped)), "Value");
        }

        TEST_F(TypeCheckingTest, DiagnosesCallArgumentTypeMismatches) {
            check(R"JOYEER(func acceptValues(values: [Int], enabled: Bool): Int { return 0 }
        func use(): Int {
        return acceptValues(values: [1, "two"], enabled: 1)
        }
        )JOYEER");

            ASSERT_EQ(checking.diagnostics.size(), 2u)
                << joyeer::typing::dump(checking.diagnostics);
            EXPECT_TRUE(std::all_of(
                checking.diagnostics.begin(),
                checking.diagnostics.end(),
                [](const auto& diagnostic) {
                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch;
                }));
        }

        TEST_F(TypeCheckingTest, ResolvesMembersAfterLocalTypeInference) {
            check(R"JOYEER(struct Box { var value: Int }
        func read(box: Box): Int {
        let inferred = box
        return inferred.value
        }
        )JOYEER");

            ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
            const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[1]);
            const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                function->body->items[1]);
            const auto member = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
                returned->value);
            ASSERT_NE(resolution.model->deferredReference(member), nullptr);
            const auto resolved = checking.model->referencedSymbol(member);
            ASSERT_TRUE(resolved.has_value());
            EXPECT_EQ(resolution.model->symbol(*resolved)->name, "value");
            EXPECT_EQ(checking.model->typeOf(member), checking.model->types().intType());
        }

        TEST_F(TypeCheckingTest, TypesStringArrayAndDictionarySubscripts) {
            check(R"JOYEER(func read(text: String, values: [String], lookup: [String: Int]) {
        let byte = text[0]
        let value = values[0]
        let number = lookup["answer"]
        }
        )JOYEER");

            ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
            const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                parseResult.root->items[0]);
            const std::vector<std::string> expected { "UInt8", "String", "Int" };
            for (size_t index = 0; index < expected.size(); ++index) {
            const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                function->body->items[index]);
            EXPECT_EQ(
                checking.model->types().displayName(declaredType(binding)),
                expected[index]);
            }
        }

        TEST_F(TypeCheckingTest, DiagnosesInvalidSubscriptBasesAndIndices) {
            check(R"JOYEER(func invalid(text: String) {
        let wrongIndex = text["zero"]
        let wrongBase = 1[0]
        }
        )JOYEER");

            ASSERT_EQ(checking.diagnostics.size(), 2u)
                << joyeer::typing::dump(checking.diagnostics);
            EXPECT_EQ(
                checking.diagnostics[0].id,
                joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
            EXPECT_EQ(
                checking.diagnostics[1].id,
                joyeer::typing::TypeCheckingDiagnosticId::notSubscriptable);
        }

            TEST_F(TypeCheckingTest, ResolvesContextualUserOptionalAndResultCases) {
                check(R"JOYEER(enum Failure { Missing, Message(String), }
            func some(value: Int): Int? { return .Some(value) }
            func none(): Int? { return .None }
            func ok(value: Int): Result<Int, Failure> { return .Ok(value) }
            func err(): Result<Int, Failure> { return .Err(.Missing) }
            )JOYEER");

                ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
                const std::vector<std::string> expectedCases { "Some", "None", "Ok", "Err" };
                for (size_t index = 0; index < expectedCases.size(); ++index) {
                const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                    parseResult.root->items[index + 1]);
                const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                    function->body->items[0]);
                const auto resolved = checking.model->referencedSymbol(returned->value);
                ASSERT_TRUE(resolved.has_value());
                EXPECT_EQ(resolution.model->symbol(*resolved)->name, expectedCases[index]);
                EXPECT_EQ(checking.model->callTarget(returned->value), resolved);
                }

                const auto err = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                    parseResult.root->items[4]);
                const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                    err->body->items[0]);
                const auto outer = std::static_pointer_cast<joyeer::syntax::ContextualCaseExprSyntax>(
                    returned->value);
                const auto inner = std::static_pointer_cast<joyeer::syntax::ContextualCaseExprSyntax>(
                    outer->arguments[0]->value);
                const auto innerCase = checking.model->referencedSymbol(inner);
                ASSERT_TRUE(innerCase.has_value());
                EXPECT_EQ(resolution.model->symbol(*innerCase)->name, "Missing");
            }

            TEST_F(TypeCheckingTest, DiagnosesInvalidContextualEnumConstructions) {
                check(R"JOYEER(enum Value { Number(Int), Empty, }
            func invalid(): Value {
            let missingContext = .Number(1)
            let wrongPayload: Value = .Number("one", 2)
            return .Absent
            }
            )JOYEER");

                ASSERT_EQ(checking.diagnostics.size(), 4u)
                    << joyeer::typing::dump(checking.diagnostics);
                EXPECT_EQ(
                    checking.diagnostics[0].id,
                    joyeer::typing::TypeCheckingDiagnosticId::missingContextualType);
                EXPECT_EQ(
                    checking.diagnostics[1].id,
                    joyeer::typing::TypeCheckingDiagnosticId::enumCaseArgumentMismatch);
                EXPECT_EQ(
                    checking.diagnostics[2].id,
                    joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
                EXPECT_EQ(
                    checking.diagnostics[3].id,
                    joyeer::typing::TypeCheckingDiagnosticId::unknownEnumCase);
            }

                TEST_F(TypeCheckingTest, TypesMatchPayloadBindingsAndArmResults) {
                    check(R"JOYEER(enum Value { Empty, Number(Int), Text(String), }
                func read(value: Value): Int {
                return match value {
                .Empty => 0,
                .Number(number) => number,
                .Text(text) => text.count,
                }
                }
                )JOYEER");

                    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
                    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                        parseResult.root->items[1]);
                    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                        function->body->items[0]);
                    const auto match = std::static_pointer_cast<joyeer::syntax::MatchExprSyntax>(
                        returned->value);
                    EXPECT_EQ(checking.model->typeOf(match), checking.model->types().intType());

                    const auto numberPattern = std::static_pointer_cast<joyeer::syntax::EnumCasePatternSyntax>(
                        match->arms[1]->pattern);
                    const auto numberBinding =
                        std::static_pointer_cast<joyeer::syntax::BindingPatternSyntax>(
                            numberPattern->arguments[0]->pattern);
                    EXPECT_EQ(
                        checking.model->types().displayName(declaredType(numberBinding)),
                        "Int");
                    const auto textPattern = std::static_pointer_cast<joyeer::syntax::EnumCasePatternSyntax>(
                        match->arms[2]->pattern);
                    const auto textBinding = std::static_pointer_cast<joyeer::syntax::BindingPatternSyntax>(
                        textPattern->arguments[0]->pattern);
                    EXPECT_EQ(
                        checking.model->types().displayName(declaredType(textBinding)),
                        "String");
                }

                TEST_F(TypeCheckingTest, ChecksOptionalAndResultMatchesWithContextualBodies) {
                    check(R"JOYEER(enum Failure { Missing, }
                func unwrap(value: Int?): Result<Int, Failure> {
                return match value {
                .Some(number) => .Ok(number),
                .None => .Err(.Missing),
                }
                }
                )JOYEER");

                    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
                    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                        parseResult.root->items[1]);
                    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                        function->body->items[0]);
                    const auto match = std::static_pointer_cast<joyeer::syntax::MatchExprSyntax>(
                        returned->value);
                    EXPECT_EQ(
                        checking.model->types().displayName(*checking.model->typeOf(match)),
                        "Result<Int, Failure>");
                    for (const auto& arm : match->arms) {
                    const auto casePattern =
                        std::static_pointer_cast<joyeer::syntax::EnumCasePatternSyntax>(arm->pattern);
                    EXPECT_TRUE(checking.model->referencedSymbol(casePattern).has_value());
                    }
                }

                TEST_F(TypeCheckingTest, DiagnosesNonExhaustiveEnumMatches) {
                    check(R"JOYEER(enum Choice { First, Second, Third, }
                func choose(value: Choice): Int {
                return match value {
                .First => 1,
                .Second => 2,
                }
                }
                )JOYEER");

                    ASSERT_EQ(checking.diagnostics.size(), 1u)
                        << joyeer::typing::dump(checking.diagnostics);
                    EXPECT_EQ(
                        checking.diagnostics[0].id,
                        joyeer::typing::TypeCheckingDiagnosticId::nonExhaustiveMatch);
                }

TEST_F(TypeCheckingTest, RejectsRefutableEnumPayloadCoverage) {
    ASSERT_NO_FATAL_FAILURE(check(readFixture(
            "type-checking/err/regression_type_payload_nonexhaustive.joyeer")));

    ASSERT_EQ(checking.diagnostics.size(), 1u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_EQ(
            checking.diagnostics[0].id,
            joyeer::typing::TypeCheckingDiagnosticId::nonExhaustiveMatch);
    EXPECT_EQ(
            checking.diagnostics[0].message,
            "match over 'Choice' is not exhaustive; add the missing cases or a wildcard arm");
}

TEST_F(TypeCheckingTest, RejectsIncompleteNestedAndProductPayloadCoverage) {
    ASSERT_NO_FATAL_FAILURE(check(R"JOYEER(
enum Flag { Value(Bool) }
enum Pair { Values(Bool, Bool) }
enum Inner { First, Second }
enum Outer { Value(Inner) }
enum Literal { Text(String), Byte(UInt8) }

func flag(value: Flag): Int {
    return match value { .Value(true) => 1 }
}
func pair(value: Pair): Int {
    return match value {
        .Values(true, true) => 1,
        .Values(false, false) => 0,
    }
}
func outer(value: Outer): Int {
    return match value { .Value(.First) => 1 }
}
func optional(value: Bool?): Int {
    return match value { .Some(true) => 1, .None => 0 }
}
func resultCode(value: Result<Bool, Bool>): Int {
    return match value { .Ok(true) => 1, .Err(false) => 0 }
}
func literal(value: Literal): Int {
    return match value { .Text("one") => 1, .Byte(b'a') => 0 }
}
)JOYEER"));

    ASSERT_EQ(checking.diagnostics.size(), 6u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(std::all_of(
            checking.diagnostics.begin(),
            checking.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id ==
                        joyeer::typing::TypeCheckingDiagnosticId::nonExhaustiveMatch;
            }));
}

TEST_F(TypeCheckingTest, AcceptsExhaustiveNestedPayloadMatrices) {
    ASSERT_NO_FATAL_FAILURE(check(readFixture("e2e/regression_type_payload_coverage.joyeer")));
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, KeepsWidePayloadProductsSymbolic) {
    std::ostringstream text;
    text << "enum Bits { Value(";
    for (size_t index = 0; index < 80; ++index) {
        if (index != 0) text << ", ";
        text << "Bool";
    }
    text << ") }\nfunc classify(value: Bits): Int {\nreturn match value {\n";
    for (const auto* literal : { "true", "false" }) {
        text << ".Value(";
        for (size_t index = 0; index < 80; ++index) {
            if (index != 0) text << ", ";
            text << (index == 79 ? literal : "_");
        }
        text << ") => 0,\n";
    }
    text << "}\n}\n";

    ASSERT_NO_FATAL_FAILURE(check(text.str()));
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

                TEST_F(TypeCheckingTest, AcceptsBooleanCompletenessAndWildcardForInfiniteDomains) {
                    check(R"JOYEER(func fromBool(value: Bool): Int {
                return match value { true => 1, false => 0, }
                }
                func fromByte(value: UInt8): Int {
                return match value { b'a' => 1, _ => 0, }
                }
                )JOYEER");

                    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
                }

                TEST_F(TypeCheckingTest, DiagnosesMatchPayloadAndArmTypeErrors) {
                    check(R"JOYEER(enum Value { Empty, Number(Int), }
                func invalid(value: Value) {
                let selected = match value {
                .Empty => 0,
                .Number("not an int") => "wrong arm type",
                }
                }
                )JOYEER");

                    ASSERT_EQ(checking.diagnostics.size(), 2u)
                        << joyeer::typing::dump(checking.diagnostics);
                    EXPECT_EQ(
                        checking.diagnostics[0].id,
                        joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
                    EXPECT_EQ(
                        checking.diagnostics[1].id,
                        joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
                }

                    TEST_F(TypeCheckingTest, EnforcesAssignmentMutabilityAndAccessMarkers) {
                        check(R"JOYEER(struct Box {
                    var value: Int
                    let fixed: Int = 0
                    }
                    func mutate(box: inout Box) {
                    &box.value = 1
                    var local = box
                    local.value = 2
                    let frozen = box
                    frozen.value = 3
                    local.fixed = 4
                    box.value = 5
                    &local.value = 6
                    }
                    )JOYEER");

                        ASSERT_EQ(checking.diagnostics.size(), 4u)
                            << joyeer::typing::dump(checking.diagnostics);
                        EXPECT_EQ(
                            checking.diagnostics[0].id,
                            joyeer::typing::TypeCheckingDiagnosticId::assignmentToImmutable);
                        EXPECT_EQ(
                            checking.diagnostics[1].id,
                            joyeer::typing::TypeCheckingDiagnosticId::assignmentToImmutable);
                        EXPECT_EQ(
                            checking.diagnostics[2].id,
                            joyeer::typing::TypeCheckingDiagnosticId::invalidAccessMarker);
                        EXPECT_EQ(
                            checking.diagnostics[3].id,
                            joyeer::typing::TypeCheckingDiagnosticId::invalidAccessMarker);
                    }

                    TEST_F(TypeCheckingTest, EnforcesInoutCallArgumentConventions) {
                        check(R"JOYEER(func update(value: inout Int) { &value = 1 }
                    func inspect(value: Int) { }
                    func use() {
                    var number = 0
                    update(value: &number)
                    update(value: number)
                    update(value: &1)
                    inspect(value: &number)
                    }
                    )JOYEER");

                        ASSERT_EQ(checking.diagnostics.size(), 3u)
                            << joyeer::typing::dump(checking.diagnostics);
                        EXPECT_TRUE(std::all_of(
                            checking.diagnostics.begin(),
                            checking.diagnostics.end(),
                            [](const auto& diagnostic) {
                            return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                                invalidInoutArgument;
                            }));
                    }

                    TEST_F(TypeCheckingTest, EnforcesConsumingCallArgumentConventions) {
                        check(R"JOYEER(func take(value: consuming String) { print(value: value) }
                    func inspect(value: String) { print(value: value) }
                    func valid() {
                    let text = "owned"
                    take(value: consume text)
                    take(value: consume ("temporary" + "!"))
                    var values = ["item"]
                    take(value: consume values[0])
                    }
                    func invalidBorrow(value: borrowing String) {
                    take(value: consume value)
                    }
                    func invalid() {
                    let text = "owned"
                    take(value: text)
                    inspect(value: consume text)
                    }
                    )JOYEER");

                        ASSERT_EQ(checking.diagnostics.size(), 3u)
                            << joyeer::typing::dump(checking.diagnostics);
                        EXPECT_TRUE(std::all_of(
                            checking.diagnostics.begin(),
                            checking.diagnostics.end(),
                            [](const auto& diagnostic) {
                            return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                                invalidConsumeArgument;
                            }));
                    }

                        TEST_F(TypeCheckingTest, TreatsExplicitBorrowingAsTheDefaultEffect) {
                            check(R"JOYEER(func inspect(value: borrowing String) {
                        print(value: value)
                        }
                        func valid() {
                        let text = "retained"
                        inspect(value: text)
                        print(value: text)
                        }
                        func invalid() {
                        let text = "retained"
                        inspect(value: &text)
                        }
                        )JOYEER");

                            ASSERT_EQ(checking.diagnostics.size(), 1u)
                                << joyeer::typing::dump(checking.diagnostics);
                            EXPECT_EQ(
                                checking.diagnostics[0].id,
                                joyeer::typing::TypeCheckingDiagnosticId::invalidInoutArgument);
                        }

                        TEST_F(TypeCheckingTest, EnforcesInitializingCallArgumentConventions) {
                            check(R"JOYEER(func initialize(out: initializing String) {
                        &out = "value"
                        }
                        func forward(out: initializing String) {
                        initialize(out: &out)
                        }
                        func valid() {
                        var text: String
                        initialize(out: &text)
                        print(value: text)
                        }
                        func invalid() {
                        var missingMarker: String
                        initialize(out: missingMarker)
                        let frozen: String
                        initialize(out: &frozen)
                        var values: [String] = ["item"]
                        initialize(out: &values[0])
                        }
                        )JOYEER");

                            ASSERT_EQ(checking.diagnostics.size(), 3u)
                                << joyeer::typing::dump(checking.diagnostics);
                            EXPECT_TRUE(std::all_of(
                                checking.diagnostics.begin(),
                                checking.diagnostics.end(),
                                [](const auto& diagnostic) {
                                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                                    invalidInitializingArgument;
                                }));
                        }

                            TEST_F(TypeCheckingTest, EnforcesCallSiteExclusivity) {
                                check(R"JOYEER(func inspect(first: Int, second: Int) { }
                            func update(dst: inout Int, src: Int) { &dst = src }
                            func take(value: consuming Int, other: Int) { }
                            func initialize(out: initializing Int, other: Int) { &out = other }
                            func swap(first: inout Int, second: inout Int) { }
                            func mutate(value: inout Int): Int { &value = value + 1
                            return value
                            }
                            func validBorrowing() {
                            let value = 1
                            inspect(first: value, second: value)
                            }
                            func invalidInout() {
                            var value = 1
                            update(dst: &value, src: value)
                            }
                            func invalidConsuming() {
                            var value = 1
                            take(value: consume value, other: value)
                            }
                            func invalidInitializing(index: Int) {
                            var value: Int
                            initialize(out: &value, other: value)
                            }
                            func invalidExclusive() {
                            var value = 1
                            swap(first: &value, second: &value)
                            }
                            func invalidBuiltin() {
                            var values = [1]
                            &values.append(element: values[0])
                            }
                            func invalidComputed(index: Int) {
                            var value = 1
                            update(dst: &value, src: value + index)
                            }
                            func invalidNested() {
                            var value = 1
                            update(dst: &value, src: mutate(value: &value))
                            }
                            )JOYEER");

                                ASSERT_EQ(checking.diagnostics.size(), 7u)
                                    << joyeer::typing::dump(checking.diagnostics);
                                EXPECT_TRUE(std::all_of(
                                    checking.diagnostics.begin(),
                                    checking.diagnostics.end(),
                                    [](const auto& diagnostic) {
                                    return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                                        overlappingAccess;
                                    }));
                            }

                            TEST_F(TypeCheckingTest, RecordsTheConflictingAccessAsANote) {
                                check(R"JOYEER(func update(dst: inout Int, src: borrowing Int) {
                            &dst = src
                            }
                            func invalid() {
                            var value = 1
                            update(dst: &value, src: value)
                            }
                            )JOYEER");

                                ASSERT_EQ(checking.diagnostics.size(), 1u)
                                    << joyeer::typing::dump(checking.diagnostics);
                                const auto& diagnostic = checking.diagnostics[0];
                                EXPECT_EQ(
                                    diagnostic.id,
                                    joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
                                ASSERT_EQ(diagnostic.notes.size(), 1u);
                                EXPECT_EQ(
                                    source->content.substr(
                                        diagnostic.span.offset,
                                        diagnostic.span.length),
                                    "value");
                                EXPECT_EQ(
                                    source->content.substr(
                                        diagnostic.notes[0].offset,
                                        diagnostic.notes[0].length),
                                    "value");
                                EXPECT_LT(diagnostic.notes[0].offset, diagnostic.span.offset);
                                EXPECT_EQ(
                                    diagnostic.notes[0].message,
                                    "conflicting inout access occurs here");
                            }

                                TEST_F(TypeCheckingTest, ProvidesAccessMarkerFixIts) {
                                    check(R"JOYEER(func update(value: inout Int) { &value = 1 }
                                func take(value: consuming String) { print(value: value) }
                                func inspect(value: borrowing String) { print(value: value) }
                                func invalid() {
                                var number = 0
                                update(value: number)
                                let text = "owned"
                                take(value: &text)
                                inspect(value: &text)
                                }
                                )JOYEER");

                                    ASSERT_EQ(checking.diagnostics.size(), 3u)
                                        << joyeer::typing::dump(checking.diagnostics);
                                    const std::vector<std::string> replacements { "&", "consume ", "" };
                                    for (size_t index = 0; index < replacements.size(); ++index) {
                                        ASSERT_TRUE(checking.diagnostics[index].help.has_value());
                                        ASSERT_TRUE(checking.diagnostics[index].fixIt.has_value());
                                        EXPECT_EQ(
                                            checking.diagnostics[index].fixIt->replacement,
                                            replacements[index]);
                                    }
                                    EXPECT_EQ(checking.diagnostics[0].fixIt->length, 0u);
                                    EXPECT_EQ(checking.diagnostics[1].fixIt->length, 1u);
                                    EXPECT_EQ(checking.diagnostics[2].fixIt->length, 1u);
                                }

                            TEST_F(TypeCheckingTest, AllowsDisjointStructFields) {
                                check(R"JOYEER(struct Pair { var left: Int
                            var right: Int
                            }
                            func update(dst: inout Int, src: Int) { &dst = src }
                            func swap(first: inout Int, second: inout Int) { }
                            func valid() {
                            var pair = Pair(left: 1, right: 2)
                            update(dst: &pair.left, src: pair.right)
                            swap(first: &pair.left, second: &pair.right)
                            }
                            )JOYEER");

                                EXPECT_TRUE(checking.succeeded())
                                    << joyeer::typing::dump(checking.diagnostics);
                            }

                            TEST_F(TypeCheckingTest, ConservativelyRejectsDifferentArrayIndices) {
                                check(R"JOYEER(func update(dst: inout Int, src: Int) { &dst = src }
                            func invalid() {
                            var values = [1, 2]
                            update(dst: &values[0], src: values[1])
                            }
                            )JOYEER");

                                ASSERT_EQ(checking.diagnostics.size(), 1u)
                                    << joyeer::typing::dump(checking.diagnostics);
                                EXPECT_EQ(
                                    checking.diagnostics[0].id,
                                    joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
                            }

                            TEST_F(TypeCheckingTest, AllowsSnapshotBeforeExclusiveArrayAccess) {
                                check(R"JOYEER(func update(dst: inout Int, src: Int) { &dst = src }
                            func valid() {
                            var values = [1, 2]
                            let snapshot = values[1]
                            update(dst: &values[0], src: snapshot)
                            }
                            )JOYEER");

                                EXPECT_TRUE(checking.succeeded())
                                    << joyeer::typing::dump(checking.diagnostics);
                            }

                            TEST_F(TypeCheckingTest, AllowsSequentialNestedExclusiveEvaluation) {
                                check(R"JOYEER(func mutate(value: inout Int): Int {
                            &value = value + 1
                            return value
                            }
                            func inspect(first: Int, second: Int) { }
                            func valid() {
                            var value = 0
                            inspect(first: mutate(value: &value), second: mutate(value: &value))
                            }
                            )JOYEER");

                                EXPECT_TRUE(checking.succeeded())
                                    << joyeer::typing::dump(checking.diagnostics);
                            }

TEST_F(TypeCheckingTest, RejectsConditionalArgumentAccessWithConflictingNote) {
    ASSERT_NO_FATAL_FAILURE(check(readFixture(
            "type-checking/err/regression_type_conditional_exclusivity.joyeer")));

    ASSERT_EQ(checking.diagnostics.size(), 1u)
            << joyeer::typing::dump(checking.diagnostics);
    const auto& diagnostic = checking.diagnostics[0];
    EXPECT_EQ(diagnostic.id, joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
    EXPECT_EQ(
            diagnostic.message,
            "overlapping inout and borrowing access to 'value' in the same call");
    ASSERT_EQ(diagnostic.notes.size(), 1u);
    EXPECT_EQ(source->content.substr(diagnostic.span.offset, diagnostic.span.length), "value");
    EXPECT_EQ(
            source->content.substr(diagnostic.notes[0].offset, diagnostic.notes[0].length),
            "value");
    EXPECT_LT(diagnostic.notes[0].offset, diagnostic.span.offset);
    EXPECT_EQ(diagnostic.notes[0].message, "conflicting inout access occurs here");
}

TEST_F(TypeCheckingTest, RejectsBorrowingReceiverAccessWithConflictingNote) {
    ASSERT_NO_FATAL_FAILURE(check(readFixture(
            "type-checking/err/regression_type_receiver_exclusivity.joyeer")));

    ASSERT_EQ(checking.diagnostics.size(), 1u)
            << joyeer::typing::dump(checking.diagnostics);
    const auto& diagnostic = checking.diagnostics[0];
    EXPECT_EQ(diagnostic.id, joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
    EXPECT_EQ(
            diagnostic.message,
            "overlapping inout and borrowing access to 'text' in the same call");
    ASSERT_EQ(diagnostic.notes.size(), 1u);
    EXPECT_EQ(source->content.substr(diagnostic.span.offset, diagnostic.span.length), "text");
    EXPECT_EQ(
            source->content.substr(diagnostic.notes[0].offset, diagnostic.notes[0].length),
            "text");
    EXPECT_LT(diagnostic.notes[0].offset, diagnostic.span.offset);
    EXPECT_EQ(diagnostic.notes[0].message, "conflicting inout access occurs here");
}

TEST_F(TypeCheckingTest, TraversesEveryEvaluatedArgumentForm) {
    const std::vector<std::string> expressions {
        "if true { value } else { 0 }",
        "if false { 0 } else { value }",
        "if value == 1 { 0 } else { 0 }",
        "match value { _ => 0 }",
        "match true { true => value, false => 0 }",
        "match false { true => 0, false => value }",
        "if true { let snapshot = value\nsnapshot } else { 0 }",
        "if true { while value < 1 { }\n0 } else { 0 }",
        "if true { while false { print(value: value) }\n0 } else { 0 }",
        "if true { return value } else { 0 }",
        "if true { value = 2\n0 } else { 0 }",
        "if true { var values = [0]\n&values[value] = 1\n0 } else { 0 }",
        "Box(value: value).value",
        "[value][0]",
        "[value].count",
    };
    for (const auto& expression : expressions) {
        SCOPED_TRACE(expression);
        ASSERT_NO_FATAL_FAILURE(check(
                "struct Box { var value: Int }\n"
                "func add(dst: inout Int, src: Int) { &dst = src }\n"
                "func invalid(): Int {\nvar value = 1\n"
                "add(dst: &value, src: " + expression + ")\nreturn value\n}\n"));
        ASSERT_EQ(checking.diagnostics.size(), 1u)
                << joyeer::typing::dump(checking.diagnostics);
        EXPECT_EQ(
                checking.diagnostics[0].id,
                joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
        ASSERT_EQ(checking.diagnostics[0].notes.size(), 1u);
        EXPECT_EQ(
                checking.diagnostics[0].notes[0].message,
                "conflicting inout access occurs here");
    }
}

TEST_F(TypeCheckingTest, TraversesComputedBorrowingReceiversAndResults) {
    const std::vector<std::string> expressions {
        "(text + \"\").utf8().count",
        "(if true { text } else { \"\" }).utf8().count",
        "byteToInt(value: text.utf8()[0])",
        "[text][0].utf8().count",
    };
    for (const auto& expression : expressions) {
        SCOPED_TRACE(expression);
        ASSERT_NO_FATAL_FAILURE(check(
                "func update(text: inout String, count: Int) { &text = \"updated\" }\n"
                "func invalid() {\nvar text = \"original\"\n"
                "update(text: &text, count: " + expression + ")\n}\n"));
        ASSERT_EQ(checking.diagnostics.size(), 1u)
                << joyeer::typing::dump(checking.diagnostics);
        EXPECT_EQ(
                checking.diagnostics[0].id,
                joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
    }
}

TEST_F(TypeCheckingTest, AllowsCompletedEvaluationsSnapshotsAndDisjointFields) {
    ASSERT_NO_FATAL_FAILURE(check(readFixture(
            "e2e/regression_type_evaluation_accesses.joyeer")));
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, AllowsSubscriptIndexEvaluationBeforeExclusiveProjection) {
    ASSERT_NO_FATAL_FAILURE(check(R"JOYEER(
func add(dst: inout Int, src: Int) { &dst = dst + src }
func valid() {
    var values = [1, 2]
    add(dst: &values[values.count - 1], src: 1)
}
)JOYEER"));
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsLaterAccessesWhileDirectBorrowIsStillActive) {
    ASSERT_NO_FATAL_FAILURE(check(R"JOYEER(
func increment(value: inout Int): Int {
    &value = value + 1
    return value
}
func inspect(first: Int, second: Int) { }
func invalid() {
    var value = 0
    inspect(first: value, second: if true { increment(value: &value) } else { 0 })
}
)JOYEER"));
    ASSERT_EQ(checking.diagnostics.size(), 1u)
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_EQ(
            checking.diagnostics[0].id,
            joyeer::typing::TypeCheckingDiagnosticId::overlappingAccess);
    ASSERT_EQ(checking.diagnostics[0].notes.size(), 1u);
    EXPECT_EQ(
            checking.diagnostics[0].notes[0].message,
            "conflicting borrowing access occurs here");
}

                    TEST_F(TypeCheckingTest, TypesMutatingArrayAppendFromTheReceiverElement) {
                        check(R"JOYEER(func build() {
                    var values: [String] = []
                    &values.append(element: "first")
                    let second = "second"
                    &values.append(element: second)
                    print(value: values.count)
                    }
                    )JOYEER");

                        EXPECT_TRUE(checking.succeeded())
                            << joyeer::typing::dump(checking.diagnostics);
                    }

                    TEST_F(TypeCheckingTest, EnforcesArrayAppendReceiverAndElementRules) {
                        check(R"JOYEER(func invalid() {
                    let frozen = [1]
                    &frozen.append(element: 2)
                    var values = [1]
                    values.append(element: 2)
                    &values.append(element: "wrong")
                    }
                    )JOYEER");

                        ASSERT_EQ(checking.diagnostics.size(), 3u)
                            << joyeer::typing::dump(checking.diagnostics);
                        EXPECT_EQ(
                            checking.diagnostics[0].id,
                            joyeer::typing::TypeCheckingDiagnosticId::assignmentToImmutable);
                        EXPECT_EQ(
                            checking.diagnostics[1].id,
                            joyeer::typing::TypeCheckingDiagnosticId::invalidAccessMarker);
                        EXPECT_EQ(
                            checking.diagnostics[2].id,
                            joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
                    }

                    TEST_F(TypeCheckingTest, TypesMutableDictionaryInsertionAndCount) {
                        check(R"JOYEER(func build() {
                    var lookup: [String: Int] = [:]
                    &lookup["answer"] = 42
                    &lookup["answer"] = 43
                    print(value: lookup.count)
                    print(value: lookup["answer"])
                    }
                    )JOYEER");

                        EXPECT_TRUE(checking.succeeded())
                            << joyeer::typing::dump(checking.diagnostics);
                    }

                    TEST_F(TypeCheckingTest, EnforcesDictionaryWriteAccessAndTypes) {
                        check(R"JOYEER(func invalid() {
                    let frozen: [String: Int] = [:]
                    &frozen["key"] = 1
                    var lookup: [String: Int] = [:]
                    lookup["key"] = 1
                    &lookup[1] = 2
                    &lookup["key"] = "wrong"
                    }
                    )JOYEER");

                        ASSERT_EQ(checking.diagnostics.size(), 4u)
                            << joyeer::typing::dump(checking.diagnostics);
                        EXPECT_EQ(
                            checking.diagnostics[0].id,
                            joyeer::typing::TypeCheckingDiagnosticId::assignmentToImmutable);
                        EXPECT_EQ(
                            checking.diagnostics[1].id,
                            joyeer::typing::TypeCheckingDiagnosticId::invalidAccessMarker);
                        EXPECT_EQ(
                            checking.diagnostics[2].id,
                            joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
                        EXPECT_EQ(
                            checking.diagnostics[3].id,
                            joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
                    }

                TEST_F(TypeCheckingTest, ResolvesEverySuccessfulDeferredReference) {
                    check(R"JOYEER(enum Choice { Some(Int), None, }
                struct Box { var value: Int }
                func update(box: Box, choice: Choice): Choice {
                let inferred = box
                let copied = inferred.value
                return match choice {
                .Some(number) => .Some(number + copied),
                .None => .None,
                }
                }
                )JOYEER");

                    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
                    ASSERT_FALSE(resolution.model->deferredReferences().empty());
                    for (const auto& deferred : resolution.model->deferredReferences()) {
                        const auto& node = resolution.model->node(deferred.node);
                        if (deferred.kind == joyeer::semantic::DeferredResolutionKind::callNeedsCalleeType) {
                            EXPECT_TRUE(checking.model->callTarget(node).has_value());
                        } else {
                            EXPECT_TRUE(checking.model->referencedSymbol(node).has_value());
                        }
                    }
                }

                TEST_F(TypeCheckingTest, DiagnosesDeferredMembersThatAreNotCallable) {
                    check(R"JOYEER(struct Box { var value: Int }
                func invalid(box: Box) {
                let inferred = box
                inferred.value()
                }
                )JOYEER");

                    ASSERT_EQ(checking.diagnostics.size(), 1u)
                            << joyeer::typing::dump(checking.diagnostics);
                    EXPECT_EQ(
                            checking.diagnostics[0].id,
                            joyeer::typing::TypeCheckingDiagnosticId::notCallable);
                }

                TEST_F(TypeCheckingTest, TypeChecksTheJsonParserMvpFixture) {
                    check(readFixture("parser/ok/json_mvp.joyeer"));

                    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
                    EXPECT_FALSE(resolution.model->deferredReferences().empty());
                    for (const auto& deferred : resolution.model->deferredReferences()) {
                        const auto& node = resolution.model->node(deferred.node);
                        if (deferred.kind == joyeer::semantic::DeferredResolutionKind::callNeedsCalleeType) {
                            EXPECT_TRUE(checking.model->callTarget(node).has_value());
                        } else {
                            EXPECT_TRUE(checking.model->referencedSymbol(node).has_value());
                        }
                    }
                }


// Decision 0015, stage 4: a struct's `deinit` is type-checked as a method body, a type
// that declares one is noncopyable, noncopyability propagates, and an implicit copy of a
// noncopyable value is rejected where it happens.
constexpr const char* handlePrelude =
        "struct Handle {\n"
        "    var fd: Int\n"
        "    deinit() { }\n"
        "}\n"
        "func open(): Handle { return Handle(fd: 1) }\n"
        "func take(handle: consuming Handle) { }\n";

TEST_F(TypeCheckingTest, ADeinitBodyIsTypeChecked) {
    check("struct Handle {\n"
          "    var fd: Int\n"
          "    deinit() { let open: Int = self.fd }\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);

    check("struct Handle {\n"
          "    var fd: Int\n"
          "    deinit() { let open: Bool = self.fd }\n"
          "}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::typeMismatch));
}

TEST_F(TypeCheckingTest, ADeinitCannotConsumeSelfOrMoveAFieldOut) {
    check("struct Inner {\n"
          "    var n: Int\n"
          "    deinit() { }\n"
          "}\n"
          "struct Outer {\n"
          "    var inner: Inner\n"
          "    deinit() { take(inner: consume self) }\n"
          "}\n"
          "func take(inner: consuming Outer) { }\n");
    EXPECT_TRUE(hasMethodDiagnostic(
            joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument));

    check("struct Inner {\n"
          "    var n: Int\n"
          "    deinit() { }\n"
          "}\n"
          "struct Outer {\n"
          "    var inner: Inner\n"
          "    deinit() { take(inner: consume self.inner) }\n"
          "}\n"
          "func take(inner: consuming Inner) { }\n");
    EXPECT_TRUE(hasMethodDiagnostic(
            joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument));
}

TEST_F(TypeCheckingTest, ADeinitReceiverIsTheStructAndItsResultIsVoid) {
    check("struct Handle {\n"
          "    var fd: Int\n"
          "    deinit() { }\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto& model = *resolution.model;
    const auto handle = std::find_if(model.symbols().begin(), model.symbols().end(),
            [](const auto& symbol) {
                return symbol.kind == joyeer::semantic::SymbolKind::structure &&
                       symbol.name == "Handle";
            });
    ASSERT_NE(handle, model.symbols().end());
    const auto declaration = std::static_pointer_cast<joyeer::syntax::StructDeclSyntax>(
            model.node(*handle->declaration));
    const auto symbol = model.declaredSymbol(declaration->deinitializer);
    ASSERT_TRUE(symbol.has_value());
    const auto* signature = checking.model->callable(*symbol);
    ASSERT_NE(signature, nullptr);
    EXPECT_EQ(signature->result, checking.model->types().voidType());
    EXPECT_EQ(signature->receiverType, checking.model->types().typeForSymbol(handle->id));
    EXPECT_TRUE(signature->parameters.empty());
}

TEST_F(TypeCheckingTest, ANoncopyableTypeIsOneWithADeinitOrANoncopyablePart) {
    check(std::string(handlePrelude) +
          "struct Plain { var n: Int }\n"
          "struct Holder { var inner: Handle }\n"
          "struct Pair {\n var a: Int\n var b: Plain\n}\n"
          "enum Slot {\n Full(Handle),\n Empty\n}\n"
          "enum Mark {\n Set(Int),\n Unset\n}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    auto& model = *checking.model;
    const auto named = [&](const std::string& name) {
        const auto found = std::find_if(resolution.model->symbols().begin(),
                resolution.model->symbols().end(), [&](const auto& symbol) {
                    return (symbol.kind == joyeer::semantic::SymbolKind::structure ||
                            symbol.kind == joyeer::semantic::SymbolKind::enumeration) &&
                           symbol.name == name;
                });
        EXPECT_NE(found, resolution.model->symbols().end());
        return model.types().typeForSymbol(found->id).value();
    };
    auto& types = model.types();
    const auto handle = named("Handle");

    EXPECT_FALSE(model.isCopyable(handle));
    EXPECT_NE(model.noncopyableReason(handle).value_or("").find("deinit"), std::string::npos);
    EXPECT_FALSE(model.isCopyable(named("Holder")));
    EXPECT_FALSE(model.isCopyable(named("Slot")));
    EXPECT_TRUE(model.isCopyable(named("Plain")));
    EXPECT_TRUE(model.isCopyable(named("Pair")));
    EXPECT_TRUE(model.isCopyable(named("Mark")));
    EXPECT_FALSE(model.noncopyableReason(named("Plain")).has_value());

    EXPECT_FALSE(model.isCopyable(types.arrayType(handle)));
    EXPECT_FALSE(model.isCopyable(types.optionalType(handle)));
    EXPECT_FALSE(model.isCopyable(types.dictionaryType(types.stringType(), handle)));
    EXPECT_FALSE(model.isCopyable(types.resultType(handle, types.stringType())));
    EXPECT_FALSE(model.isCopyable(types.resultType(types.intType(), handle)));
    EXPECT_TRUE(model.isCopyable(types.arrayType(named("Plain"))));
    EXPECT_TRUE(model.isCopyable(types.intType()));
    EXPECT_TRUE(model.isCopyable(types.stringType()));
    EXPECT_TRUE(model.isCopyable(types.resultType(types.intType(), types.stringType())));
}

TEST_F(TypeCheckingTest, RejectsImplicitCopiesOfANoncopyableValueAtTheCopySite) {
    const std::vector<std::string> copies {
        // binding and assignment from a place
        "func f() { let a = open()\n let b = a }\n",
        "func f() { let a = open()\n var b = open()\n b = a }\n",
        // returning a borrowed parameter or a field of an owned value
        "func f(h: Handle): Handle { return h }\n",
        "struct Holder { var inner: Handle }\n"
        "func f(h: consuming Holder): Handle { return h.inner }\n",
        // building an aggregate from a place
        "struct Holder { var inner: Handle }\n"
        "func f() { let a = open()\n let h = Holder(inner: a) }\n",
        "func f() { let a = open()\n let items = [a] }\n",
        "func f() { let a = open()\n let o: Handle? = .Some(a) }\n",
    };
    for (const auto& body : copies) {
        SCOPED_TRACE(body);
        check(std::string(handlePrelude) + body);
        EXPECT_TRUE(hasMethodDiagnostic(
                joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
                << joyeer::typing::dump(checking.diagnostics);
    }

    check(std::string(handlePrelude) + "func f() { let a = open()\n let b = a }\n");
    ASSERT_FALSE(checking.diagnostics.empty());
    EXPECT_NE(checking.diagnostics[0].message.find("Handle"), std::string::npos);
    EXPECT_NE(checking.diagnostics[0].message.find("deinit"), std::string::npos);
}

TEST_F(TypeCheckingTest, AllowsMovesTemporariesBorrowsAndCopiesOfCopyableParts) {
    const std::vector<std::string> allowed {
        // an owned temporary is not a copy
        "func f() { let a = open() }\n",
        "func f() { var a = open()\n a = open() }\n",
        // `consume` moves; a borrow does not copy
        "func f() { let a = open()\n take(handle: consume a) }\n",
        "func peek(h: Handle): Int { return h.fd }\n"
        "func f() { let a = open()\n let n = peek(h: a) }\n",
        // returning an owning local or a consuming parameter is a move
        "func f(): Handle { let a = open()\n return a }\n",
        "func f(h: consuming Handle): Handle { return h }\n",
        // reading a copyable field of a noncopyable value copies only the field
        "func f() { let a = open()\n let n = a.fd }\n",
        // copyable types are unaffected
        "struct Plain { var n: Int }\n"
        "func f() { let a = Plain(n: 1)\n let b = a\n let items = [a, b] }\n",
    };
    for (const auto& body : allowed) {
        SCOPED_TRACE(body);
        check(std::string(handlePrelude) + body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}
// `consume x` moves an owning local or consuming parameter wherever a value is stored.
TEST_F(TypeCheckingTest, ConsumeMovesANoncopyableValueIntoWhereItIsStored) {
    const std::vector<std::string> moves {
        "struct Holder { var inner: Handle }\n"
        "func f() { let a = open()\n let h = Holder(inner: consume a) }\n",
        "func f() { let a = open()\n let o: Handle? = .Some(consume a) }\n",
        "func f() { let a = open()\n let items = [consume a] }\n",
        "func f() { let a = open()\n var items: [Handle] = []\n &items.append(element: consume a) }\n",
        "func f() { let a = open()\n let b = consume a }\n",
        "func f() { let a = open()\n var b = open()\n b = consume a }\n",
        "struct Holder { var inner: Handle }\n"
        "func f(h: consuming Handle): Holder { return Holder(inner: consume h) }\n",
    };
    for (const auto& body : moves) {
        SCOPED_TRACE(body);
        check(std::string(handlePrelude) + body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ConsumeStillNeedsAnOwningLocalAndAStoringContext) {
    const std::vector<std::string> rejected {
        // a borrowed parameter is not owned
        "struct Holder { var inner: Handle }\n"
        "func f(h: Handle) { let w = Holder(inner: consume h) }\n",
        // `consume` on a plain borrowing call argument is still wrong
        "func use(handle: Handle) { }\n"
        "func f() { let a = open()\n use(handle: consume a) }\n",
    };
    for (const auto& body : rejected) {
        SCOPED_TRACE(body);
        check(std::string(handlePrelude) + body);
        EXPECT_TRUE(hasMethodDiagnostic(
                joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, DictGetCannotCopyANoncopyableValue) {
    check(std::string(handlePrelude) +
          "func f() { var d: [Int: Handle] = [:]\n let g = d.get(key: 1) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(
            joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ADeinitCannotReplaceSelf) {
    check("struct A {\n    var n: Int\n    deinit() { &self = A(n: 1) }\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(
            joyeer::typing::TypeCheckingDiagnosticId::assignmentToSelfInDeinit))
            << joyeer::typing::dump(checking.diagnostics);

    check("struct A {\n    var n: Int\n    deinit() { &self.n = 1 }\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

// The check walks each distinct type once: a type that reuses the same part twice per level
// must not take time exponential in its depth.
TEST_F(TypeCheckingTest, NoncopyabilityIsLinearInTheNumberOfDistinctTypes) {
    std::string text = "struct S0 { var a: Int }\n";
    for (int level = 1; level <= 26; ++level) {
        text += "struct S" + std::to_string(level) + " { var a: S" + std::to_string(level - 1) +
                "\n    var b: S" + std::to_string(level - 1) + " }\n";
    }
    text += "func f(p: S26) { let q = p }\n";
    check(text);
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

// The copy check looks through the value-producing forms, not only a bare place.
TEST_F(TypeCheckingTest, ReportsCopiesThroughIfMatchBlockPropagationAndTrailingExpressions) {
    const std::vector<std::string> copies {
        "func f(flag: Bool) { let a = open()\n let b = open()\n let c = if flag { a } else { b } }\n",
        "func f(flag: Bool) { let a = open()\n let b = open()\n"
        " let items = [if flag { a } else { b }] }\n",
        "func f(n: Int) { let a = open()\n let c = match n { 1 => a, _ => open() } }\n",
        "func f(): Handle { let a = open()\n a }\n",
        "func f(r: Result<Handle, Int>) { let h = r? }\n",
        "func f(): Result<Handle, Int> { let r: Result<Handle, Int> = .Ok(open())\n let h = r?\n return .Ok(open()) }\n",
    };
    for (const auto& body : copies) {
        SCOPED_TRACE(body);
        check(std::string(handlePrelude) + body);
        EXPECT_TRUE(hasMethodDiagnostic(
                joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, AllowsAnIfOrMatchThatProducesFreshNoncopyableValues) {
    const std::vector<std::string> fresh {
        "func f(flag: Bool): Handle { return if flag { open() } else { Handle(fd: 2) } }\n",
        "func f(n: Int): Handle { return match n { 1 => open(), _ => Handle(fd: 3) } }\n",
        "func f(): Handle { open() }\n",
    };
    for (const auto& body : fresh) {
        SCOPED_TRACE(body);
        check(std::string(handlePrelude) + body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

// `Array.removeLast` gives back the last element as an Optional of the element type.
TEST_F(TypeCheckingTest, ArrayRemoveLastReturnsAnOptionalOfTheElementType) {
    check("func f() {\n"
          "    var numbers: [Int] = [1, 2]\n"
          "    let last = &numbers.removeLast()\n"
          "    var words: [String] = []\n"
          "    let word = &words.removeLast()\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    auto& types = checking.model->types();
    EXPECT_EQ(types.displayName(declaredType(function->body->items[1])), "Int?");
    EXPECT_EQ(types.displayName(declaredType(function->body->items[3])), "String?");
}

TEST_F(TypeCheckingTest, ArrayRemoveLastNeedsAMutableReceiver) {
    check("func f() { let numbers: [Int] = [1]\n let last = &numbers.removeLast() }\n");
    EXPECT_FALSE(checking.succeeded());
    check("func f() { var numbers: [Int] = [1]\n let last = numbers.removeLast() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidAccessMarker))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ArrayRemoveLastGivesBackANoncopyableElementAsATemporary) {
    check(std::string(handlePrelude) +
          "func f() { var items: [Handle] = []\n let popped = &items.removeLast() }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    // The Optional it returns holds a handle, so copying it is still an error.
    check(std::string(handlePrelude) +
          "func f() { var items: [Handle] = []\n let popped = &items.removeLast()\n let again = popped }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
            << joyeer::typing::dump(checking.diagnostics);
}

// A pattern may bind a noncopyable payload only by moving it out of an owned scrutinee.
constexpr const char* slotPrelude =
        "struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
        "enum Slot {\n    Full(Handle),\n    Empty,\n}\n"
        "func make(): Slot { return .Full(Handle(fd: 1)) }\n";

TEST_F(TypeCheckingTest, AMatchMayBindANoncopyablePayloadOutOfATemporaryOrAConsumedPlace) {
    const std::vector<std::string> allowed {
        "func f(): Int { return match make() { .Full(h) => h.fd, .Empty => 0 } }\n",
        "func f(slot: consuming Slot): Int { return match consume slot { .Full(h) => h.fd, .Empty => 0 } }\n",
        "func f() { let slot = make()\n match consume slot { .Full(h) => print(value: h.fd), .Empty => () } }\n",
        // a wildcard binds nothing, so a place may be matched without moving
        "func f(slot: Slot): Int { return match slot { .Full(_) => 1, .Empty => 0 } }\n",
    };
    for (const auto& body : allowed) {
        SCOPED_TRACE(body);
        check(std::string(slotPrelude) + body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, AMatchCannotBindANoncopyablePayloadOutOfAPlaceWithoutConsume) {
    check(std::string(slotPrelude) +
          "func f(slot: Slot): Int { return match slot { .Full(h) => h.fd, .Empty => 0 } }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
            << joyeer::typing::dump(checking.diagnostics);
    // `consume` still needs an owning local or consuming parameter.
    check(std::string(slotPrelude) +
          "func f(slot: Slot): Int { return match consume slot { .Full(h) => h.fd, .Empty => 0 } }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, AMatchBindingOwnsItsPayloadAndCanBeMovedOnward) {
    const std::vector<std::string> allowed {
        "func take(handle: consuming Handle) { }\n"
        "func f() { match make() { .Full(h) => take(handle: consume h), .Empty => () } }\n",
        "struct Holder { var inner: Handle }\n"
        "func f(): Holder? { match make() { .Full(h) => return .Some(Holder(inner: consume h)), .Empty => return .None } }\n",
        "func f() { var items: [Handle] = []\n"
        " match make() { .Full(h) => &items.append(element: consume h), .Empty => () } }\n",
    };
    for (const auto& body : allowed) {
        SCOPED_TRACE(body);
        check(std::string(slotPrelude) + body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, AnAmpersandDoesNotHideAPlaceFromTheMovedBindingCheck) {
    const std::vector<std::string> copies {
        "func f(slot: Slot): Int { return match &slot { .Full(h) => h.fd, .Empty => 0 } }\n",
        "func f() { let slot = make()\n match (&slot) { .Full(h) => print(value: h.fd), .Empty => () } }\n",
        // a bound handle is a place too, so matching or copying it needs `consume`
        "func f() { match make() { .Full(h) => print(value: h.fd), .Empty => () } }\n"
        "func g() { let o: Handle? = .Some(Handle(fd: 1))\n match o { .Some(h) => print(value: h.fd), .None => () } }\n",
        "func f() { match make() { .Full(h) => { let k = h }, .Empty => () } }\n",
    };
    for (const auto& body : copies) {
        SCOPED_TRACE(body);
        check(std::string(slotPrelude) + body);
        EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, AWholeNoncopyableScrutineeCannotBeBoundByAPattern) {
    check(std::string(slotPrelude) +
          "func f() { match make() { other => (), } }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
            << joyeer::typing::dump(checking.diagnostics);
}

// Decision 0015, "Linear types": which types are linear, and the type-level rules.
constexpr const char* linearTypes =
        "linear struct Token {\n    var id: Int\n}\n"
        "struct Holder {\n    var token: Token\n}\n"
        "enum Box {\n    Full(Token),\n    Empty,\n}\n";

TEST_F(TypeCheckingTest, ALinearTypeAndWhatHoldsItAreLinearAndNoncopyable) {
    checkToolchain(std::string(linearTypes) +
                   "func f(a: Token, b: Holder, c: Box, d: [Token], e: Token?, g: Result<Token, Int>, h: Int) { }\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items.back());
    const auto& model = *checking.model;
    for (size_t index = 0; index < 6; ++index) {
        const auto type = declaredType(function->parameters[index]);
        EXPECT_TRUE(model.isLinear(type)) << index;
        EXPECT_FALSE(model.isCopyable(type)) << index;
    }
    EXPECT_FALSE(model.isLinear(declaredType(function->parameters[6])));
}

TEST_F(TypeCheckingTest, ALinearStructEndsFieldByFieldAndAnAggregateOnlyItsLinearFields) {
    checkToolchain(std::string(linearTypes) + "func f(a: Token, b: Holder, c: Box) { }\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items.back());
    const auto& model = *checking.model;
    // A declared linear struct has every field as a leaf; Holder needs only its Token field;
    // an enum is consumed whole.
    EXPECT_EQ(model.linearLeafPaths(declaredType(function->parameters[0])).size(), 1u);
    const auto holder = model.linearLeafPaths(declaredType(function->parameters[1]));
    ASSERT_EQ(holder.size(), 1u);
    EXPECT_EQ(holder[0].size(), 2u);
    EXPECT_EQ(model.linearLeafPaths(declaredType(function->parameters[2])),
              (std::vector<std::vector<joyeer::semantic::SymbolId>> { {} }));
}

TEST_F(TypeCheckingTest, TheLinearModifierIsReservedForToolchainSources) {
    check("linear struct Token {\n    var id: Int\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearReserved))
            << joyeer::typing::dump(checking.diagnostics);
    checkToolchain("linear struct Token {\n    var id: Int\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ALinearValueHasNoDeinitToRun) {
    checkToolchain("linear struct Token {\n    var id: Int\n    deinit() { }\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearInDeinitType))
            << joyeer::typing::dump(checking.diagnostics);
    // a struct with a deinit may not hold a linear field either: its destruction would drop it
    checkToolchain(std::string(linearTypes) +
                   "struct Owner {\n    var token: Token\n    deinit() { }\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearInDeinitType))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ADictionaryCannotHoldALinearKeyOrValue) {
    checkToolchain(std::string(linearTypes) + "func f() { var d: [Int: Token] = [:] }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearCollectionElement))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ForInCannotIterateAnArrayOfLinearValues) {
    checkToolchain(std::string(linearTypes) +
                   "func f(items: [Token]) { for item in items { } }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearCollectionElement))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ALinearStructNeedsAFieldToEnd) {
    checkToolchain("linear struct Zed {\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearStructWithoutFields))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ADictionaryOfLinearValuesIsRejectedInParametersResultsAndFields) {
    const std::vector<std::string> bodies {
        "func f(d: [Int: Token]): Int { return 0 }\n",
        "struct S {\n    var d: [Int: Token]\n}\n",
        "func g(): [Int: Token] { return [:] }\n",
    };
    for (const auto& body : bodies) {
        SCOPED_TRACE(body);
        checkToolchain(std::string(linearTypes) + body);
        EXPECT_TRUE(hasMethodDiagnostic(
                joyeer::typing::TypeCheckingDiagnosticId::linearCollectionElement))
                << joyeer::typing::dump(checking.diagnostics);
    }
}


// `Array.consumeEmpty` is a consuming builtin member (spec 2.6.4): it needs `consume` on an owned
// receiver and returns Void.
TEST_F(TypeCheckingTest, ArrayConsumeEmptyIsAConsumingMemberReturningVoid) {
    const std::vector<std::string> allowed {
        "func f() { var items: [Int] = []\n consume items.consumeEmpty() }\n",
        "func f(items: consuming [String]) { consume items.consumeEmpty() }\n",
    };
    for (const auto& body : allowed) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ArrayConsumeEmptyNeedsConsumeOnAnOwnedReceiver) {
    check("func f() { var items: [Int] = []\n items.consumeEmpty() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f() { var items: [Int] = []\n &items.consumeEmpty() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    // A borrowed parameter is not owned, so it cannot be consumed.
    check("func f(items: [Int]) { consume items.consumeEmpty() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f(items: inout [Int]) { consume items.consumeEmpty() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

// A builtin consuming member works on the receiver's own storage: a named place is required (spec 2.6.4).
TEST_F(TypeCheckingTest, ArrayConsumeEmptyRejectsATemporaryAnElementAndATopLevelConstant) {
    const std::vector<std::string> rejected {
        "func make(): [Int] { return [] }\nfunc f() { consume make().consumeEmpty() }\n",
        "func f() { consume [1].consumeEmpty() }\n",
        "func f() { var grid: [[Int]] = [[]]\n consume grid[0].consumeEmpty() }\n",
        "let table: [Int] = []\nfunc f() { consume table.consumeEmpty() }\n",
    };
    for (const auto& body : rejected) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ArrayConsumeEmptyAcceptsAFieldALetBindingAndAConsumingMethodsSelf) {
    const std::vector<std::string> allowed {
        "struct Box {\n var xs: [Int]\n}\nfunc f() { var box = Box(xs: [])\n consume box.xs.consumeEmpty() }\n",
        "func f() { let items: [Int] = []\n consume items.consumeEmpty() }\n",
        "struct Box {\n var xs: [Int]\n consuming func end() { consume self.xs.consumeEmpty() }\n}\n",
    };
    for (const auto& body : allowed) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ArrayConsumeEmptyOnAFieldOfABorrowedStructIsRejected) {
    const std::vector<std::string> rejected {
        "struct Box {\n var xs: [Int]\n}\nfunc f(box: Box) { consume box.xs.consumeEmpty() }\n",
        "struct Box {\n var xs: [Int]\n}\nfunc f(box: inout Box) { consume box.xs.consumeEmpty() }\n",
        "struct Box {\n var xs: [Int]\n func end() { consume self.xs.consumeEmpty() }\n}\n",
    };
    for (const auto& body : rejected) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

// An unknown member has its own diagnostic; the receiver marker must not add a second one.
TEST_F(TypeCheckingTest, AnUnknownMemberUnderConsumeReportsOnlyTheUnknownMember) {
    check("func f() { var a = 1\n consume a.consumeEmpty() }\n");
    EXPECT_FALSE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    EXPECT_FALSE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidAccessMarker))
            << joyeer::typing::dump(checking.diagnostics);
}


// Tasks (spec 21): `spawn(function:, input:)` has type `JoinHandle<R>`, and `consume h.join()` gives R.
constexpr const char* taskPrelude =
        "func square(value: consuming Int): Int { return value * value }\n"
        "func greet(name: consuming String): String { return name }\n"
        "func shout(name: consuming String) { }\n"
        "struct Job {\n    var id: Int\n}\n"
        "func run(job: consuming Job): Int { return job.id }\n";

TEST_F(TypeCheckingTest, SpawnHasTheJoinHandleTypeOfTheTargetsResult) {
    check(std::string(taskPrelude) +
          "func f() {\n"
          "    let a = spawn(function: square, input: consume 7)\n"
          "    let b = spawn(function: greet, input: consume \"x\")\n"
          "    let c = spawn(function: shout, input: consume \"y\")\n"
          "    let d = spawn(function: run, input: consume Job(id: 3))\n"
          "    consume a.join()\n"
          "    consume b.join()\n"
          "    consume c.join()\n"
          "    consume d.join()\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[5]);
    auto& types = checking.model->types();
    EXPECT_EQ(types.displayName(declaredType(function->body->items[0])), "JoinHandle<Int>");
    EXPECT_EQ(types.displayName(declaredType(function->body->items[1])), "JoinHandle<String>");
    EXPECT_EQ(types.displayName(declaredType(function->body->items[2])), "JoinHandle<Void>");
    EXPECT_EQ(types.displayName(declaredType(function->body->items[3])), "JoinHandle<Int>");
}

TEST_F(TypeCheckingTest, JoinGivesTheResultTypeAndAHandleCanBeNamedInATypePosition) {
    check(std::string(taskPrelude) +
          "func wait(handle: consuming JoinHandle<Int>): Int { return consume handle.join() }\n"
          "func f(): Int {\n"
          "    let h: JoinHandle<Int> = spawn(function: square, input: consume 2)\n"
          "    return wait(handle: consume h)\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check(std::string(taskPrelude) +
          "func f() {\n"
          "    let h = spawn(function: square, input: consume 2)\n"
          "    let s: String = consume h.join()\n"
          "}\n");
    EXPECT_FALSE(checking.succeeded());
}

TEST_F(TypeCheckingTest, SpawnRejectsATargetThatIsNotANamedFunction) {
    check(std::string(taskPrelude) + "func f() {\n    let n = 1\n    let h = spawn(function: n, input: consume 1)\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetNotFunction))
            << joyeer::typing::dump(checking.diagnostics);
    check(std::string(taskPrelude) + "func f() {\n    let h = spawn(function: square(value: 1), input: consume 1)\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetNotFunction))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SpawnRejectsMainAMethodAndANativeBinding) {
    check("func main() {\n    let h = spawn(function: main, input: consume 1)\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetUnsupported))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SpawnRejectsATargetWithoutExactlyOneConsumingParameter) {
    const std::vector<std::string> rejected {
        "func none(): Int { return 1 }\nfunc f() { let h = spawn(function: none, input: consume 1) }\n",
        "func two(a: consuming Int, b: consuming Int): Int { return a }\nfunc f() { let h = spawn(function: two, input: consume 1) }\n",
        "func borrowed(a: Int): Int { return a }\nfunc f() { let h = spawn(function: borrowed, input: consume 1) }\n",
        "func change(a: inout Int) { }\nfunc f() { let h = spawn(function: change, input: consume 1) }\n",
    };
    for (const auto& body : rejected) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetParameters))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, SpawnRejectsATargetThatNeverReturns) {
    check("func forever(a: consuming Int): Never { return forever(a: consume a) }\n"
          "func f() { let h = spawn(function: forever, input: consume 1) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetNever))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SpawnNeedsConsumeOnTheInputAndTheRightLabels) {
    check(std::string(taskPrelude) + "func f() { let h = spawn(function: square, input: 1) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SpawnInputMustMatchTheTargetsParameterType) {
    check(std::string(taskPrelude) + "func f() { let h = spawn(function: square, input: consume \"text\") }\n");
    EXPECT_FALSE(checking.succeeded());
}

TEST_F(TypeCheckingTest, JoinNeedsConsumeAndAnOwnedHandle) {
    check(std::string(taskPrelude) +
          "func f() {\n    let h = spawn(function: square, input: consume 1)\n    h.join()\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    check(std::string(taskPrelude) +
          "func f(handle: JoinHandle<Int>) { consume handle.join() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ATaskHandleIsNoncopyable) {
    check(std::string(taskPrelude) +
          "func f() {\n    let h = spawn(function: square, input: consume 1)\n    let g = h\n    consume g.join()\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
            << joyeer::typing::dump(checking.diagnostics);
}

// A user declaration of a prelude name wins in its own scope (0013 Q15, spec 21.2).
TEST_F(TypeCheckingTest, AUserDeclaredSpawnOrJoinHandleWinsOverThePrelude) {
    check("func spawn(function: Int, input: Int): Int { return function + input }\n"
          "func f(): Int { return spawn(function: 1, input: 2) }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check("struct JoinHandle {\n    var id: Int\n}\n"
          "func f(): Int {\n    let h = JoinHandle(id: 4)\n    return h.id\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}


TEST_F(TypeCheckingTest, SpawnRejectsAMethodAndABuiltinWithOneDiagnosticEach) {
    check("struct S {\n    var a: Int\n    func m(value: consuming Int): Int { return value }\n}\n"
          "func f() { let h = spawn(function: S.m, input: consume 1) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetUnsupported))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f() { let h = spawn(function: print, input: consume 1) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::spawnTargetUnsupported))
            << joyeer::typing::dump(checking.diagnostics);
    // A member of an instance is read as a name, not checked as a value: exactly one error, and no
    // leaked "unresolved reference" from the deferred member lookup.
    check("struct S {\n    var a: Int\n    func m(value: consuming Int): Int { return value }\n}\n"
          "func f() { let s = S(a: 1)\n let h = spawn(function: s.m, input: consume 1) }\n");
    EXPECT_EQ(checking.diagnostics.size(), 1u) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SpawnAcceptsAParenthesizedFunctionName) {
    check(std::string(taskPrelude) +
          "func f(): Int {\n    let h = spawn(function: (square), input: consume 2)\n    return consume h.join()\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

// `consume` covers the receiver of the call it starts (spec 21.3): parenthesize before `?`, and bind a
// joined result before using it as an argument.
TEST_F(TypeCheckingTest, JoinBeforeAPostfixQuestionMarkIsParenthesizedAndAnArgumentIsBoundFirst) {
    check("func fails(value: consuming Int): Result<Int, IOError> { return .Ok(value) }\n"
          "func g(): Result<Int, IOError> {\n"
          "    let h = spawn(function: fails, input: consume 1)\n"
          "    let v = (consume h.join())?\n"
          "    return .Ok(v)\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check(std::string(taskPrelude) +
          "func g() {\n    let h = spawn(function: square, input: consume 1)\n    print(value: consume h.join())\n}\n");
    EXPECT_FALSE(checking.succeeded());
}

TEST_F(TypeCheckingTest, ATaskHandleCannotBeAKeyAValueOrAConstantAndCannotBeCompared) {
    const std::vector<std::string> rejected {
        "func f(table: [Int: JoinHandle<Int>]) { }\n",
        "func f(handles: consuming [JoinHandle<Int>]) { for each in handles { } }\n",
        "func make(value: consuming Int): Int { return value }\n"
        "let global: JoinHandle<Int> = spawn(function: make, input: consume 1)\n",
    };
    for (const auto& body : rejected) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_FALSE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
    check(std::string(taskPrelude) +
          "func f(): Bool {\n    let a = spawn(function: square, input: consume 1)\n"
          "    let b = spawn(function: square, input: consume 2)\n    return a == b\n}\n");
    EXPECT_FALSE(checking.succeeded());
}


// Channels (spec 21.9, decision 0019).
constexpr const char* channelPrelude =
        "func make(): Int {\n"
        "    var sender: ChannelSender<Int>\n"
        "    var receiver: ChannelReceiver<Int>\n"
        "    channel(capacity: 4, sender: &sender, receiver: &receiver)\n"
        "    return 0\n"
        "}\n";

TEST_F(TypeCheckingTest, ChannelInitializesASenderAndAReceiverOfOneElementType) {
    check(channelPrelude);
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SendAndReceiveHaveResultTypesOverChannelClosed) {
    check("func f() {\n"
          "    var sender: ChannelSender<Int>\n"
          "    var receiver: ChannelReceiver<String>\n"
          "    var other: ChannelSender<String>\n"
          "    var third: ChannelReceiver<Int>\n"
          "    channel(capacity: 2, sender: &sender, receiver: &third)\n"
          "    channel(capacity: 2, sender: &other, receiver: &receiver)\n"
          "    let sent = sender.send(value: consume 5)\n"
          "    let got = third.receive()\n"
          "    let text = receiver.receive()\n"
          "    consume sender.close()\n"
          "    consume other.close()\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(parseResult.root->items[0]);
    auto& types = checking.model->types();
    EXPECT_EQ(types.displayName(declaredType(function->body->items[6])), "Result<Void, ChannelClosed>");
    EXPECT_EQ(types.displayName(declaredType(function->body->items[7])), "Result<Int, ChannelClosed>");
    EXPECT_EQ(types.displayName(declaredType(function->body->items[8])), "Result<String, ChannelClosed>");
}

TEST_F(TypeCheckingTest, ChannelRejectsEndsThatAreNotASenderAndAReceiverOfTheSameType) {
    const std::vector<std::string> rejected {
        "func f() { var a: ChannelSender<Int>\n var b: ChannelReceiver<String>\n channel(capacity: 1, sender: &a, receiver: &b) }\n",
        "func f() { var a: ChannelReceiver<Int>\n var b: ChannelSender<Int>\n channel(capacity: 1, sender: &a, receiver: &b) }\n",
        "func f() { var a: ChannelSender<Int>\n var b: Int\n channel(capacity: 1, sender: &a, receiver: &b) }\n",
    };
    for (const auto& body : rejected) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::channelEndTypes))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ChannelNeedsAnInitializingMarkerOnBothEnds) {
    check("func f() { var a: ChannelSender<Int>\n var b: ChannelReceiver<Int>\n channel(capacity: 1, sender: a, receiver: &b) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidInitializingArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, AConstantCapacityBelowOneIsRejected) {
    for (const auto* capacity : {"0", "-3"}) {
        SCOPED_TRACE(capacity);
        check(std::string("func f() { var a: ChannelSender<Int>\n var b: ChannelReceiver<Int>\n channel(capacity: ") +
              capacity + ", sender: &a, receiver: &b) }\n");
        EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::channelCapacity))
                << joyeer::typing::dump(checking.diagnostics);
    }
    check("func f(n: Int) { var a: ChannelSender<Int>\n var b: ChannelReceiver<Int>\n channel(capacity: n, sender: &a, receiver: &b) }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, AChannelElementCannotBeLinear) {
    check("func f(c: consuming ChannelSender<JoinHandle<Int>>) { }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearChannelElement))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f(c: consuming ChannelReceiver<[JoinHandle<Int>]>) { }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearChannelElement))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, AChannelElementMayBeVoidAHandleALargeValueOrAnotherEnd) {
    const std::vector<std::string> allowed {
        "func f(c: consuming ChannelSender<Void>) { }\n",
        "struct Handle {\n var fd: Int\n deinit() { }\n}\nfunc f(c: consuming ChannelSender<Handle>) { }\n",
        "func f(c: consuming ChannelReceiver<[String]>) { }\n",
        "func f(c: consuming ChannelSender<ChannelSender<Int>>) { }\n",
    };
    for (const auto& body : allowed) {
        SCOPED_TRACE(body);
        check(body);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ChannelEndsAreNoncopyableAndSendAndCloseNeedConsume) {
    check("func f(c: consuming ChannelSender<Int>) { let d = c }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f(c: ChannelSender<Int>) { c.send(value: 5) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f(c: consuming ChannelSender<Int>) { c.close() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
    check("func f(c: ChannelSender<Int>) { consume c.close() }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, SendTakesAValueOfTheElementType) {
    check("func f(c: ChannelSender<Int>) { c.send(value: consume \"x\") }\n");
    EXPECT_FALSE(checking.succeeded());
}

TEST_F(TypeCheckingTest, ABorrowedEndCanSendAndReceive) {
    check("func f(s: ChannelSender<Int>, r: ChannelReceiver<Int>) {\n"
          "    let a = s.send(value: consume 1)\n"
          "    let b = r.receive()\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, AUserDeclaredChannelWinsOverThePrelude) {
    check("func channel(capacity: Int): Int { return capacity }\n"
          "func f(): Int { return channel(capacity: 3) }\n"
          "struct ChannelSender {\n    var id: Int\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}


TEST_F(TypeCheckingTest, ALinearChannelElementIsRejectedInAnEnumPayloadToo) {
    check("enum Message {\n    Hand(ChannelSender<JoinHandle<Int>>),\n    Stop,\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearChannelElement))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ALinearChannelElementIsRejectedNestedAndInAStructDeclaredLater) {
    check("struct Holder {\n    var ends: [Optional<ChannelReceiver<Pending>>]\n}\n"
          "struct Pending {\n    var handle: JoinHandle<Int>\n}\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::linearChannelElement))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, TheCapacityArgumentOfChannelFollowsTheOrdinaryMarkerRules) {
    check("func f(n: Int) { var a: ChannelSender<Int>\n var b: ChannelReceiver<Int>\n"
          " channel(capacity: consume n, sender: &a, receiver: &b) }\n");
    EXPECT_TRUE(hasMethodDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument))
            << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ChannelMembersNeedAReceiverValue) {
    check("func f() { ChannelSender.send(value: consume 1) }\n");
    EXPECT_FALSE(checking.succeeded());
    check("func f() { ChannelReceiver.receive() }\n");
    EXPECT_FALSE(checking.succeeded());
}

// A struct or enum comparison has no lowering, so equality on one is a diagnostic, not a compiler crash.
TEST_F(TypeCheckingTest, EqualityOnAnEnumOrStructIsRejected) {
    check("enum Kind {\n    A,\n    B,\n}\nfunc f(x: Kind, y: Kind): Bool { return x == y }\n");
    EXPECT_FALSE(checking.succeeded());
    check("struct P {\n    var n: Int\n}\nfunc f(x: P, y: P): Bool { return x == y }\n");
    EXPECT_FALSE(checking.succeeded());
}

// Function values (spec 2.3.1, 5.6.1; decision 0020).
bool hasTypeDiagnostic(const joyeer::typing::TypeCheckingResult& checking,
                       joyeer::typing::TypeCheckingDiagnosticId id) {
    return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
            [id](const auto& diagnostic) { return diagnostic.id == id; });
}

TEST_F(TypeCheckingTest, FunctionNameIsAValueOfItsFunctionType) {
    check("func square(value: Int): Int { return value * value }\n"
          "func bump(count: inout Int) { &count = count + 1 }\n"
          "func run() {\n"
          "    let f = square\n"
          "    let g: (inout Int): () = bump\n"
          "    let h: (borrowing Int): Int = square\n"
          "    var n = f(3)\n"
          "    g(&n)\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << checking.diagnostics.size();
    const auto run = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(parseResult.root->items[2]);
    const auto f = declaredType(run->body->items[0]);
    EXPECT_EQ(checking.model->types().displayName(f), "(Int): Int");
    EXPECT_EQ(declaredType(run->body->items[2]), f);   // `borrowing` written out is the same type
    EXPECT_EQ(checking.model->types().displayName(declaredType(run->body->items[1])), "(inout Int): Void");
}

TEST_F(TypeCheckingTest, FunctionTypesMatchExactly) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "func f(value: Int): Int { return value }\nfunc run() { let g: (inout Int): Int = f }\n",
            "func f(value: Int): Int { return value }\nfunc run() { let g: (Int, Int): Int = f }\n",
            "func f(value: Int): Int { return value }\nfunc run() { let g: (String): Int = f }\n",
            "func f(value: Int): Int { return value }\nfunc run() { let g: (Int): String = f }\n",
            "func f(value: Int): Never { return f(value: value) }\nfunc run() { let g: (Int): Int = f }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::typeMismatch));
    }
}

TEST_F(TypeCheckingTest, RejectsFunctionsThatCannotBeValues) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "func run() { let p = print }\n",
            "func run() { let s = spawn }\n",
            "struct S {\n    var n: Int\n    func get(): Int { return self.n }\n}\n"
            "func run(s: S) { let m = s.get }\n",
            "func main() { let m = main }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notAFunctionValue));
    }
}

TEST_F(TypeCheckingTest, ChecksCallsThroughFunctionValues) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    const std::string functions =
            "func square(value: Int): Int { return value * value }\n"
            "func bump(count: inout Int) { &count = count + 1 }\n"
            "func take(text: consuming String): Int { return 1 }\n";
    check(functions + "func run() { let f = square\n let n = f(value: 2) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::labeledFunctionValueCall));
    check(functions + "func run() { let f = square\n let n = f(1, 2) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidCallArguments));
    check(functions + "func run() { let g = bump\n var n = 1\n g(n) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidInoutArgument));
    check(functions + "func run() { let t = take\n let s = \"a\"\n let n = t(s) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidConsumeArgument));
    check(functions + "func run() { let t = take\n let s = \"a\"\n let n = t(consume s) }\n");
    EXPECT_TRUE(checking.succeeded());
    // A forgotten label on a call by name keeps the ordinary name-resolution diagnostic.
    Diagnostics lexing;
    const auto text = std::make_shared<SourceFile>(functions + "func run() { let n = square(2) }\n");
    LexParser(&lexing).parse(text);
    const auto parsed = joyeer::parser::Parser(text->tokens).parse();
    const auto resolved = joyeer::semantic::NameResolver().resolve(parsed.root);
    EXPECT_TRUE(std::any_of(resolved.diagnostics.begin(), resolved.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.id == joyeer::semantic::NameResolutionDiagnosticId::missingArgumentLabel;
    })) << joyeer::semantic::dump(resolved.diagnostics);
}

TEST_F(TypeCheckingTest, StoredFunctionValuesNeedAParenthesizedCallee) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    const std::string declarations =
            "func square(value: Int): Int { return value * value }\n"
            "func make(): (Int): Int { return square }\n"
            "struct S {\n    var f: (Int): Int\n}\n";
    check(declarations + "func run(s: S): Int { return (s.f)(2) }\n");
    EXPECT_TRUE(checking.succeeded());
    check(declarations + "func run(s: S): Int { return s.f(2) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notCallable));
    check(declarations + "func run(): Int { return make()(2) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notCallable));
    check(declarations + "func run(): Int { return (make())(2) }\n");
    EXPECT_TRUE(checking.succeeded());
    check(declarations + "func run(t: [(Int): Int]): Int { return (t[0])(2) }\n");
    EXPECT_TRUE(checking.succeeded());
    // An optional function value must be unwrapped first.
    check(declarations + "func run(m: ((Int): Int)?): Int { return m(2) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notCallable));
}

TEST_F(TypeCheckingTest, StructuresCannotStoreNever) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("struct S { var n: Never }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::neverFieldType));
    check("struct S { let n: Never }\nfunc run(s: S) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::neverFieldType));
    check("struct Box<T> { var value: T }\nstruct S { var n: Int }\nfunc run(s: S) {}\n");
    EXPECT_TRUE(checking.succeeded());
}

TEST_F(TypeCheckingTest, FunctionTypesRejectNeverParametersAndEquality) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("func run(f: (Never): Int) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidFunctionType));
    check("func square(value: Int): Int { return value }\nfunc run(): Bool { return square == square }\n");
    EXPECT_FALSE(checking.succeeded());
    check("func square(value: Int): Int { return value }\nfunc run() { print(value: square) }\n");
    EXPECT_FALSE(checking.succeeded());
}

// Copyability and linearity stop at a function type (spec 4.1.1).
TEST_F(TypeCheckingTest, FunctionValuesAreCopyableAndNeverLinear) {
    check("func work(handle: consuming JoinHandle<Int>): Int { return consume handle.join() }\n"
          "struct S {\n    var f: (S): S\n}\n"
          "func run() {\n"
          "    let w: (consuming JoinHandle<Int>): Int = work\n"
          "    let copy = w\n"
          "    let again = w\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded());
    const auto run = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(parseResult.root->items[2]);
    const auto type = declaredType(run->body->items[0]);
    EXPECT_FALSE(checking.model->noncopyableReason(type).has_value());
    EXPECT_FALSE(checking.model->isLinear(type));
}
// A call through a value nested in another call's arguments keeps its effects, so it cannot consume or
// mutate storage the outer call borrows (review of decision 0020).
TEST_F(TypeCheckingTest, NestedCallsThroughValuesKeepTheirEffectsForExclusivity) {
    check("func eat(s: consuming String): Int { return 1 }\n"
          "func outer(a: borrowing String, b: Int) { }\n"
          "func run() {\n"
          "    let e = eat\n"
          "    var t = \"wor\" + \"ld\"\n"
          "    outer(a: t, b: e(consume t))\n"
          "}\n");
    EXPECT_FALSE(checking.succeeded());
    check("func inc(n: inout Int): Int { &n = n + 1\n return n }\n"
          "func outer(a: Int, b: Int) { }\n"
          "func run() {\n"
          "    let g = inc\n"
          "    var x = 1\n"
          "    outer(a: x, b: g(&x))\n"
          "}\n");
    EXPECT_FALSE(checking.succeeded());
    // The same through struct construction, a method receiver and a field callee.
    const std::string replace =
            "func replace(s: inout String): Int { &s = \"b\"\n return 1 }\n"
            "struct P {\n    var a: String\n    var b: Int\n"
            "    func m(b: Int): Int { return b }\n}\n"
            "struct H {\n    var name: String\n    var f: (inout H): Int\n}\n"
            "func show(a: String, b: Int): Int { return b }\n";
    for (const auto* body : {
            "func run() { let f = replace\n var s = \"a\"\n let p = P(a: s, b: f(&s)) }\n",
            "func run() { let f: (inout P): Int = touch\n var p = P(a: \"a\", b: 1)\n let n = p.m(b: f(&p)) }\n"
            "func touch(p: inout P): Int { return 1 }\n",
            "func run(h: H) { var b = h\n let n = show(a: b.name, b: (b.f)(&b)) }\n"}) {
        SCOPED_TRACE(body);
        check(replace + body);
        EXPECT_FALSE(checking.succeeded());
    }
}

TEST_F(TypeCheckingTest, TypeNamesAreNotValues) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "struct P {\n    var n: Int\n}\nfunc run() { let g = P }\n",
            "func run() { let g = ChannelSender }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notAFunctionValue));
    }
    // As a member's base or a callee a type name stays valid.
    check("enum Color {\n    Red,\n}\nstruct P {\n    var n: Int\n}\n"
          "func run(): Int {\n    let c = Color.Red\n    let p = P(n: 1)\n    return p.n\n}\n");
    EXPECT_TRUE(checking.succeeded());
}

TEST_F(TypeCheckingTest, OptionalFunctionValuesPropagateAndPrintParenthesized) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("func call(f: ((Int): Int)?): Int? {\n    return f?(3)\n}\n");
    EXPECT_TRUE(checking.succeeded());
    check("func call(f: ((Int): Int)?): Int {\n    return f(3)\n}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notCallable));
    const auto record = std::find_if(checking.diagnostics.begin(), checking.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.id == Id::notCallable; });
    ASSERT_NE(record, checking.diagnostics.end());
    EXPECT_NE(record->message.find("((Int): Int)?"), std::string::npos) << record->message;
}

TEST_F(TypeCheckingTest, MembersThatMustBeCalledAreNotFunctionValues) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "enum E {\n    A(String),\n    B,\n}\nfunc run() { let a = E.A }\n",
            "func run() {\n    var values: [Int] = []\n    let f = values.append\n}\n",
            "func run(d: [String: Int]) { let g = d.get }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notAFunctionValue));
    }
    // A payload-less case and a property stay ordinary values.
    check("enum E {\n    A(String),\n    B,\n}\nfunc run(): Int {\n    let b = E.B\n    let s = \"x\"\n    return s.count\n}\n");
    EXPECT_TRUE(checking.succeeded());
}

TEST_F(TypeCheckingTest, FunctionTypesCannotBeKeysConstantsOrNeverParameters) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "func square(value: Int): Int { return value }\nfunc run() { let e = [square: 1] }\n",
            "func run(e: [(Int): Int: Int]) { }\n",
            "func square(value: Int): Int { return value }\nlet F: (Int): Int = square\n",
            "func absurd(x: Never): Int { return 1 }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidFunctionType));
    }
}

TEST_F(TypeCheckingTest, GenericCallsUseWitnessesAndCacheTransitiveInstances) {
    check(readFixture("type-checking/generics.joyeer"));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    ASSERT_EQ(checking.model->conformances().size(), 1u);
    EXPECT_EQ(checking.model->conformances()[0].witnesses.size(), 1u);
    ASSERT_EQ(checking.model->instances().size(), 2u);
    std::vector<std::string> names;
    for (const auto& instance : checking.model->instances()) names.push_back(instance.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string> {"read<Number>", "relay<Number>"}));
}

TEST_F(TypeCheckingTest, GenericInstanceNamesDisambiguatePreludeTypes) {
    check("struct Int {}\nfunc sink<T>(value: T) {}\n"
          "func main() { sink(value: 1)\nsink(value: Int()) }\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto& instances = checking.model->instances();
    ASSERT_EQ(instances.size(), 2u);
    EXPECT_NE(instances[0].name, instances[1].name);
}

TEST_F(TypeCheckingTest, GenericInferenceUsesTypedArgumentsBeforeLiteralsAndUnifiesFunctions) {
    check("func choose<T: Copyable>(a: T, b: T): T { return a }\n"
          "func call<T, R>(function: (T): R, value: T): R { return function(value) }\n"
          "func length(value: String): Int { return value.count }\n"
          "func main() {\n"
          "    let typed: UInt8 = 2\n"
          "    let a: UInt8 = choose(a: 1, b: typed)\n"
          "    let b: Double = choose(a: 1, b: 2.5)\n"
          "    let c: Int = call(function: length, value: \"abc\")\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, GenericInferenceDiagnosesConflictsAndUninformativeArguments) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* call : {
            "empty(value: nil)", "items(value: [])", "dictionary(value: [:])",
            "nothing()", "optional(value: 1)"}) {
        SCOPED_TRACE(call);
        check(std::string("func empty<T>(value: T?) {}\nfunc items<T>(value: [T]) {}\n"
                          "func dictionary<T>(value: [String: T]) {}\n"
                          "func nothing<T>(): T? { return nil }\n"
                          "func optional<T>(value: T?) {}\nfunc main() { ") + call + " }\n");
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::cannotInferTypeParameter))
                << joyeer::typing::dump(checking.diagnostics);
    }
    check("func pair<T>(a: T, b: T) {}\nfunc main(a: Int, b: String) { pair(a: a, b: b) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::conflictingTypeArgument));
}

TEST_F(TypeCheckingTest, GenericBodiesAreCheckedEvenWhenUnusedAndParametersStayDistinct) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check(readFixture("type-checking/err/generic_copy.joyeer"));
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::copyOfNoncopyable));
    EXPECT_TRUE(checking.model->instances().empty());
    check("func wrong<T, U>(a: T, b: U) { var x = a\nx = b }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::typeMismatch));
    check("func wrong<E>(value: Result<Int, E>): Result<Int, String> { return .Ok(value?) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::mismatchedPropagationError));
}

TEST_F(TypeCheckingTest, CopyableConstraintsAndEntailmentAreCheckedInGenericBodies) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("struct Ticket { let n: Int\n    deinit() {} }\n"
          "func copy<T: Copyable>(value: T): T { return value }\n"
          "func main() { let t = Ticket(n: 1)\nlet other = copy(value: t) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::unsatisfiedConstraint));
    check("func copy<T: Copyable>(value: T): T { return value }\n"
          "func pass<T>(value: T) { copy(value: value) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::unsatisfiedConstraint));
    check("func copy<T: Copyable>(value: T): T { return value }\n"
          "func pass<T: Copyable>(value: [T]): [T] { return copy(value: value) }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, GenericBodiesRejectCopiesThroughPropagationMatchAndDictionaryGet) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "func f<T>(value: T?): T? { let item = value?\nreturn item }\n",
            "func f<T>(value: T?) { match value { .Some(item) => (), .None => () } }\n",
            "func f<T>(value: [String: T]): T? { return value.get(key: \"x\") }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::copyOfNoncopyable))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, GenericPropagationChecksTheOperandInEveryContext) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* statement : {"value?", "print(value: value?)"}) {
        SCOPED_TRACE(statement);
        check(std::string("func probe<E>(value: Result<Int, E>): Result<Int, E> {\n") +
              statement + "\nreturn .Ok(0)\n}\n");
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::copyOfNoncopyable))
                << joyeer::typing::dump(checking.diagnostics);
        EXPECT_TRUE(checking.model->instances().empty());
    }
    check("func probe<E: Copyable>(value: Result<Int, E>): Result<Int, E> {\n"
          "print(value: value?)\nreturn .Ok(0)\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, GenericDictionaryKeysAreRejectedForEverySpelling) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "func f<T>(value: [T: Int]) {}\n",
            "func f<T>(value: Dict<T, Int>) {}\n",
            "func f<T: Copyable>(key: T) { let values = [key: 42] }\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidTypeArgument))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, GenericInferenceDoesNotRetypeNonliteralArguments) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* arguments : {"a: small, b: 1 + 2", "a: 1 + 2, b: small"}) {
        SCOPED_TRACE(arguments);
        check(std::string("func pair<T>(a: T, b: T) {}\n"
                          "func run(small: UInt8) { pair(") + arguments + ") }\n");
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::conflictingTypeArgument))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, GenericInferenceTreatsParenthesesTransparently) {
    check("func optional<T>(maybe: T?, value: T) {}\n"
          "func array<T>(items: [T], value: T) {}\n"
          "func dictionary<T>(items: [String: T], value: T) {}\n"
          "func pair<T>(a: T, b: T) {}\n"
          "func main() {\n"
          "optional(maybe: (nil), value: 1)\n"
          "optional(maybe: (.None), value: 1)\n"
          "array(items: ([]), value: 1)\n"
          "dictionary(items: ([:]), value: 1)\n"
          "pair(a: 1, b: (2.5))\n}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, GenericSelfResultsRetainEveryCallerConstraint) {
    check("protocol Clone { func cloned(): Self }\nprotocol Read { func read(): Int }\n"
          "func inspect<T: Clone & Read>(value: T): Int { return value.cloned().read() }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, UnusedGenericBodiesRejectBareTypeNamesAsValues) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* value : {"T", "P"}) {
        check(std::string("protocol P {}\nfunc f<T>(value: T) { let invalid = ") + value + " }\n");
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::notAFunctionValue))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ProtocolReferencesRejectTypeArguments) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "protocol P {}\nfunc f<T: P<Int>>(value: T) {}\n",
            "protocol P {}\nstruct S {}\nextension S: P<Int> {}\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidConformance))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, ConformancesCheckWitnessEffectsLabelsTypesAndVisibility) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* witness : {
            "mutating func read(value: Int): Int { return value }",
            "func read(other: Int): Int { return other }",
            "func read(value: consuming Int): Int { return value }",
            "func read(value: String): Int { return 1 }",
            "func read(value: Int): String { return \"x\" }"}) {
        SCOPED_TRACE(witness);
        check(std::string("protocol P { func read(value: Int): Int }\nstruct S {}\nextension S: P {\n") +
              "    " + witness + "\n}\n");
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::witnessMismatch))
                << joyeer::typing::dump(checking.diagnostics);
    }
    check("protocol P { func read(): Int }\nstruct S {}\nextension S: P {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::missingWitness));
    check("public protocol P { func read(): Int }\npublic struct S {}\n"
          "extension S: P { private func read(): Int { return 1 } }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::witnessNotAccessible));
    check("protocol P {}\nstruct S {}\nextension S: P {}\nextension S: P {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::duplicateConformance));
}

TEST_F(TypeCheckingTest, ConstraintsRejectDuplicateAndConflictingRequirements) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* text : {
            "protocol P {}\nfunc f<T: P & P>(value: T) {}\n",
            "protocol P { func read(): Int }\nprotocol Q { func read(): String }\n"
            "func f<T: P & Q>(value: T) {}\n"}) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidConformance))
                << joyeer::typing::dump(checking.diagnostics);
    }
    check("protocol P { func read(): Int }\nprotocol Q { func read(): Int }\n"
          "func f<T: P & Q>(value: T): Int { return value.read() }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsUnsupportedGenericAndProtocolUses) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto& [text, id] : std::vector<std::pair<std::string, Id>> {
            {"protocol P {}\nfunc f(value: P) {}\n", Id::protocolUsedAsType},
            {"protocol P { func read(): Int }\nfunc f() { P.read() }\n", Id::invalidCallArguments},
            {"protocol P { func f() {} }\n", Id::invalidProtocolMember},
            {"protocol Copyable {}\n", Id::reservedProtocolName},
            {"func f<Copyable>(value: Copyable) {}\n", Id::reservedProtocolName},
            {"struct S { func f<T>(value: T) {} }\n", Id::invalidGenericDeclaration},
            {"func f<T>(value: T) { print(value: value) }\n", Id::typeMismatch},
            {"func f<T>(value: T): Int { return value.missing() }\n", Id::unknownMemberOnTypeParameter},
            {"func f<T>(value: T) {}\nfunc main() { let x = f }\n", Id::notAFunctionValue},
            {"func f<T>(value: consuming T) {}\n"
             "func main() { let h = spawn(function: f, input: consume 1)\nconsume h.join() }\n",
             Id::spawnTargetUnsupported},
    }) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(hasTypeDiagnostic(checking, id)) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(TypeCheckingTest, GenericArgumentsRejectLinearAndNeverButAcceptVoidAndFunctions) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("func work(value: consuming Int): Int { return value }\n"
          "func f<T>(value: T) {}\n"
          "func main() { let h = spawn(function: work, input: consume 1)\nf(value: h)\nconsume h.join() }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::linearTypeArgument));
    check("func f<T, E>(value: Result<T, E>) {}\n"
          "func run(value: Result<Int, Never>) { f(value: value) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidTypeArgument));
    check("func f<T>(value: T) {}\nfunc work(): Int { return 1 }\n"
          "func main() { f(value: ())\nf(value: work) }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, RejectsExpandingRecursionWithoutRequiringAnInstance) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check(readFixture("type-checking/err/generic_recursion.joyeer"));
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::expandingGenericRecursion));
    check("func f<T: Copyable>(value: T) { g(value: [value]) }\n"
          "func g<U: Copyable>(value: U) { f(value: value) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::expandingGenericRecursion));
    check("func f<T>(value: T) { f(value: value) }\nfunc main() { f(value: 1) }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    EXPECT_EQ(checking.model->instances().size(), 1u);
}

TEST_F(TypeCheckingTest, InstantiationReasonsChooseTheNearestRootThenTheLeastCallSite) {
    check("func leaf<T>(value: T) {}\n"
          "func early<T>(value: T) { leaf(value: value) }\n"
          "func late<T>(value: T) { leaf(value: value) }\n"
          "func main() { late(value: 1)\nearly(value: 2) }\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto& instances = checking.model->instances();
    const auto leaf = std::find_if(instances.begin(), instances.end(),
            [](const auto& instance) { return instance.name == "leaf<Int>"; });
    ASSERT_NE(leaf, instances.end());
    ASSERT_TRUE(leaf->requiredBy.has_value());
    EXPECT_EQ(instances[*leaf->requiredBy].name, "early<Int>");
    for (const auto& instance : instances) {
        if (instance.requiredBy.has_value()) continue;
        ASSERT_TRUE(instance.requiredByFunction.has_value());
        EXPECT_EQ(resolution.model->symbol(*instance.requiredByFunction)->name, "main");
    }
}

TEST_F(TypeCheckingTest, InstantiationLimitsBoundTypeDepthAndNonexpandingCallChains) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    const std::string nested = std::string(65, '[') + "Int" + std::string(65, ']');
    check("func f<T>(value: T) {}\nfunc root(value: " + nested + ") { f(value: value) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::instantiationLimit));
    std::string text;
    for (size_t index = 0; index < 66; ++index) {
        text += "func f" + std::to_string(index) + "<T>(value: T) { ";
        if (index != 65) text += "f" + std::to_string(index + 1) + "(value: value)";
        text += " }\n";
    }
    check(text + "func main() { f0(value: 1) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::instantiationLimit));
}

TEST_F(TypeCheckingTest, InstantiationLimitsBoundExponentiallyExpandedTypeArguments) {
    std::string text;
    for (size_t index = 0; index < 17; ++index) {
        text += "func f" + std::to_string(index) + "<T: Copyable>(value: T) { ";
        if (index != 16) {
            text += "let next: Result<T, T> = .Ok(value)\nf" +
                    std::to_string(index + 1) + "(value: next)";
        }
        text += " }\n";
    }
    check(text + "func main() { f0(value: 1) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, joyeer::typing::TypeCheckingDiagnosticId::instantiationLimit))
            << joyeer::typing::dump(checking.diagnostics);
}
} // namespace

TEST_F(TypeCheckingTest, AConflictingTypeArgumentDoesNotCascadeIntoAFunctionTypeMismatch) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("func ap<T>(v: T, f: (borrowing T): Int): Int { return f(v) }\n"
          "func inc(x: borrowing Int): Int { return x + 1 }\n"
          "func main() { let r = ap(v: \"s\", f: inc) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::conflictingTypeArgument));
    EXPECT_FALSE(hasTypeDiagnostic(checking, Id::typeMismatch)) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, TheProgramEntryPointCannotBeGeneric) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("func main<T>() {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidGenericDeclaration));
}

// Generic structs and enums (spec 20.12, decision 0022).
TEST_F(TypeCheckingTest, GenericStructConstructionAndMembersUseTheInferredTypeArguments) {
    check("struct Box<T: Copyable> {\n"
          "    var value: T\n"
          "    func get(): T { return self.value }\n"
          "}\n"
          "func main() {\n"
          "    let b = Box(value: 1)\n"
          "    let n = b.get()\n"
          "    let v = b.value\n"
          "    let s = Box(value: \"text\")\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto main = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(parseResult.root->items[1]);
    const auto& types = checking.model->types();
    EXPECT_EQ(types.displayName(declaredType(main->body->items[0])), "Box<Int>");
    EXPECT_EQ(types.displayName(declaredType(main->body->items[1])), "Int");
    EXPECT_EQ(types.displayName(declaredType(main->body->items[2])), "Int");
    EXPECT_EQ(types.displayName(declaredType(main->body->items[3])), "Box<String>");
}

TEST_F(TypeCheckingTest, GenericEnumCasesInferTheirTypeArguments) {
    check("enum Tree<T> {\n"
          "    Leaf,\n"
          "    Node(T)\n"
          "}\n"
          "func main() {\n"
          "    let t = Tree.Node(1)\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto main = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(parseResult.root->items[1]);
    EXPECT_EQ(checking.model->types().displayName(declaredType(main->body->items[0])), "Tree<Int>");
}

TEST_F(TypeCheckingTest, GenericTypesTakeExactlyTheirTypeArguments) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("struct Box<T> { var value: T }\n"
          "func a(value: Box) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidTypeArgumentCount));
    check("struct Box<T> { var value: T }\n"
          "func a(value: Box<Int, Int>) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidTypeArgumentCount));
    check("struct Plain { var value: Int }\n"
          "func a(value: Plain<Int>) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidTypeArgumentCount));
    check("struct Box<T> { var value: T }\n"
          "func a(value: Box<Int>): Box<Int> { return value }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ExtensionsOfGenericTypesBindTheirOwnParametersToTheDeclarations) {
    check("struct Box<T: Copyable> { var value: T }\n"
          "extension Box<U> {\n"
          "    func other(): U { return self.value }\n"
          "}\n"
          "func main() {\n"
          "    let b = Box(value: 2.5)\n"
          "    let d = b.other()\n"
          "}\n");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto main = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(parseResult.root->items[2]);
    EXPECT_EQ(checking.model->types().displayName(declaredType(main->body->items[1])), "Double");
}

TEST_F(TypeCheckingTest, ExplicitTypeArgumentsOfGenericTypesAreCheckedAgainstTheirConstraints) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    check("protocol Named { func name(): String }\n"
          "struct Box<T: Named> { var value: T }\n"
          "func a(value: Box<Int>) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::unsatisfiedConstraint));
    check("protocol Named { func name(): String }\n"
          "struct Box<T: Named> { var value: T }\n"
          "struct Tag { var id: Int }\n"
          "extension Tag: Named { func name(): String { return \"tag\" } }\n"
          "func a(value: Box<Tag>) {}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check("struct Box<T> { var value: T }\n"
          "func a(value: Box<Never>) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidTypeArgument));
    checkToolchain("linear struct Handle { var id: Int }\n"
                   "struct Box<T> { var value: T }\n"
                   "func a(value: Box<Handle>) {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::linearTypeArgument));
}

TEST_F(TypeCheckingTest, ExtensionHeadersOfGenericTypesListTheirParametersOnce) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    for (const auto* header : { "Box<Int>", "Box<U, V>" }) {
        SCOPED_TRACE(header);
        check(std::string("struct Box<T> { var value: T }\n") + "struct Pair<A, B> { var a: A }\n"
              "extension " + header + " { func nothing() {} }\n");
        EXPECT_TRUE(hasTypeDiagnostic(checking, Id::invalidExtensionHeader));
    }
    check("struct Pair<A, B> { var a: A\n var b: B }\n"
          "extension Pair<X, Y> { func nothing() {} }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, GenericTypesConformUnconditionallyAndSatisfyConstraintsInsideBodies) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    const std::string declarations =
            "protocol Named { func name(): String }\n"
            "struct Box<T> { var value: T }\n"
            "extension Box<U>: Named { func name(): String { return \"box\" } }\n"
            "func describe<N: Named>(item: N): String { return item.name() }\n";
    check(declarations +
          "func wrap<T>(value: consuming T): String { return describe(item: Box(value: consume value)) }\n"
          "func main() { let s = describe(item: Box(value: 1)) }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    // A second conformance under another header name is a duplicate.
    check(declarations + "extension Box<V>: Named {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::duplicateConformance));
    // A witness is still required.
    check("protocol Named { func name(): String }\n"
          "struct Box<T> { var value: T }\n"
          "extension Box<U>: Named {}\n");
    EXPECT_TRUE(hasTypeDiagnostic(checking, Id::missingWitness));
}

// Spec 20.12.1: the expected type binds the parameters of a construction that the arguments leave open.
TEST_F(TypeCheckingTest, ExpectedTypeFillsConstructionTypeArguments) {
    check("struct Box<T> {\n"
          "    var value: T\n"
          "}\n"
          "struct Marker<T> {\n"
          "    var count: Int\n"
          "}\n"
          "func main() {\n"
          "    var a: Box<Double> = Box(value: 1)\n"
          "    var m: Marker<Int> = Marker(count: 0)\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ConstructionWithoutExpectedTypeCannotInferOpenParameter) {
    check("struct Marker<T> {\n"
          "    var count: Int\n"
          "}\n"
          "func main() {\n"
          "    var m = Marker(count: 0)\n"
          "}\n");
    EXPECT_FALSE(checking.succeeded());
    EXPECT_NE(joyeer::typing::dump(checking.diagnostics).find("cannot-infer-type-parameter"), std::string::npos);
}

// Spec 20.12.4: a generic type whose field passes a parameter on inside a larger type of its own cycle
// has infinitely many layouts.
TEST_F(TypeCheckingTest, RejectsExpandingRecursionThroughGenericTypeFields) {
    check("struct Box<T> {\n"
          "    var value: T\n"
          "}\n"
          "struct Grow<T> {\n"
          "    var next: Grow<Box<T>>\n"
          "}\n");
    EXPECT_NE(joyeer::typing::dump(checking.diagnostics).find("expanding-generic-recursion"), std::string::npos);
}

TEST_F(TypeCheckingTest, RejectsExpandingRecursionThroughMutuallyRecursiveGenericTypes) {
    check("struct Box<T> {\n"
          "    var value: T\n"
          "}\n"
          "enum Left<T> {\n"
          "    Go(Right<Box<T>>),\n"
          "    Stop\n"
          "}\n"
          "enum Right<U> {\n"
          "    Go(Left<U>),\n"
          "    Stop\n"
          "}\n");
    EXPECT_NE(joyeer::typing::dump(checking.diagnostics).find("expanding-generic-recursion"), std::string::npos);
}

TEST_F(TypeCheckingTest, AcceptsGenericTypeCyclesThatKeepTheirArguments) {
    check("struct Swap<A, B> {\n"
          "    var first: A\n"
          "    var other: Optional<Swap<B, A>>\n"
          "}\n");
    EXPECT_EQ(joyeer::typing::dump(checking.diagnostics).find("expanding-generic-recursion"), std::string::npos);
}

// A literal that the expected type fixed may be assignable to it without being it.
TEST_F(TypeCheckingTest, ExpectedTypeMayBeOptionalWhileTheLiteralIsNot) {
    check("struct Box<T> {\n"
          "    var value: T\n"
          "}\n"
          "func make(): Box<Double> {\n"
          "    return Box(value: 1)\n"
          "}\n"
          "func main() {\n"
          "    var a: Box<Int?> = Box(value: 1)\n"
          "    var b: Box<Int>? = Box(value: 2)\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(TypeCheckingTest, ExpectedTypeNeverOverridesAnArgument) {
    check("struct Box<T> {\n"
          "    var value: T\n"
          "}\n"
          "func main() {\n"
          "    let n = \"text\"\n"
          "    var a: Box<Int> = Box(value: n)\n"
          "}\n");
    EXPECT_FALSE(checking.succeeded());
}

// Annotations inside bodies get the same type-argument checks as signatures (spec 20.12).
TEST_F(TypeCheckingTest, LocalAnnotationsCheckTheirTypeArguments) {
    check("struct Box<T: Copyable> {\n"
          "    var value: T\n"
          "}\n"
          "struct Wrap<T> {\n"
          "    var inner: T\n"
          "}\n"
          "struct Ticket {\n"
          "    var n: Int\n"
          "    deinit() { }\n"
          "}\n"
          "func main() {\n"
          "    let a: Box<Ticket>? = nil\n"
          "    let b: [Box<Ticket>] = []\n"
          "    let c: Wrap<Never>\n"
          "}\n");
    const auto text = joyeer::typing::dump(checking.diagnostics);
    EXPECT_NE(text.find("unsatisfied-constraint"), std::string::npos) << text;
    EXPECT_NE(text.find("invalid-type-argument"), std::string::npos) << text;
}

TEST_F(TypeCheckingTest, RejectsABindingOfTypeNeverWithoutAValue) {
    check("func main() {\n"
          "    let c: Never\n"
          "}\n");
    EXPECT_NE(joyeer::typing::dump(checking.diagnostics).find("invalid-type-argument"), std::string::npos);
}

// `Tree.Leaf` names no type arguments: the expected type supplies them, and without one it is an error.
TEST_F(TypeCheckingTest, PayloadlessCaseOfAGenericEnumTakesItsTypeFromContext) {
    check("enum Tree<T: Copyable> {\n"
          "    Leaf,\n"
          "    Node(T, [Tree<T>])\n"
          "}\n"
          "func make(): Tree<Int> { return Tree.Leaf }\n"
          "func main() {\n"
          "    let a: Tree<Int> = Tree.Node(1, [Tree.Leaf, .Leaf])\n"
          "    let b: Tree<Int>? = Tree.Leaf\n"
          "}\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check("enum Tree<T: Copyable> {\n"
          "    Leaf,\n"
          "    Node(T)\n"
          "}\n"
          "func main() {\n"
          "    let a = Tree.Leaf\n"
          "}\n");
    EXPECT_NE(joyeer::typing::dump(checking.diagnostics).find("cannot-infer-type-parameter"), std::string::npos);
}
