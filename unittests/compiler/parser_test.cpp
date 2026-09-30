#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using joyeer::parser::DiagnosticId;
using joyeer::syntax::Kind;

std::string readFixture(const std::string& relativePath) {
        const std::string path = std::string(JOYEER_TESTS_DIR) + "/" + relativePath;
        std::ifstream input(path);
        if (!input) {
                throw std::runtime_error("cannot open parser fixture: " + path);
        }
        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
}

std::string applyFixIt(std::string text, const DiagnosticFixIt& fixIt) {
        text.replace(fixIt.offset, fixIt.length, fixIt.replacement);
        return text;
}

class ParserTest : public testing::Test {
protected:
    void parse(const std::string& text, bool requireValidLexing = true) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser lexer(&lexerDiagnostics);
        lexer.parse(source);
                if (requireValidLexing && !lexerDiagnostics.errors.empty()) {
                        for (const auto& error : lexerDiagnostics.errors) {
                                ADD_FAILURE() << "unexpected lexer diagnostic: " << error.message;
                        }
        }

        joyeer::parser::Parser parser(source->tokens);
        result = parser.parse();
        ASSERT_NE(result.root, nullptr);
    }

    bool hasDiagnostic(DiagnosticId id) const {
        return std::any_of(
                result.diagnostics.begin(),
                result.diagnostics.end(),
                [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

    Diagnostics lexerDiagnostics;
    SourceFile::Ptr source;
    joyeer::parser::ParseResult result;
};

TEST_F(ParserTest, EmptySourceProducesEmptySpannedFile) {
    parse("");

    EXPECT_TRUE(result.succeeded());
    EXPECT_TRUE(result.root->items.empty());
    EXPECT_EQ(result.root->span.offset, 0u);
    EXPECT_EQ(result.root->span.length, 0u);
}

TEST_F(ParserTest, ParsesUnitTypesValuesAndPatterns) {
    parse("let value: () = ()\n"
          "func run(): Result<Void, String> { return .Ok(()) }\n"
          "func inspect(value: Void): Bool { return match value { () => true } }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[0]);
    EXPECT_EQ(binding->annotation->kind, Kind::unitType);
    EXPECT_EQ(binding->initializer->kind, Kind::unitExpr);
    EXPECT_EQ(binding->initializer->span.length, 2u);
    const auto tree = joyeer::syntax::dump(result.root);
    EXPECT_NE(tree.find("unit_pattern"), std::string::npos);
}

TEST_F(ParserTest, UnitDoesNotEnableEmptyPayloadClausesOrGeneralTuples) {
    for (const auto* text : {
            "func run() { let value = .Ok() }\n",
            "func run() { let value = (1, 2) }\n",
            "func run(value: (Int, Int)) {}\n",
            "func run(value: Void) { match value { (1, 2) => () } }\n"}) {
        SCOPED_TRACE(text);
        parse(text);
        EXPECT_FALSE(result.succeeded());
    }
}

TEST_F(ParserTest, ParsesMvpTypesFunctionsStructsAndEnums) {
        parse(R"JOYEER(let outcome: Result<[String: JsonValue], JsonError>?
func advance(p: inout Parser, from source: String,): UInt8? { return p.input[p.pos] }
struct Parser { var input: String
var pos: Int = 0
}
enum JsonError { UnexpectedEof, Unexpected(UInt8, at: Int), }
)JOYEER");

        ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.root->items.size(), 4u);

    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(binding->annotation->kind, Kind::optionalType);
    const auto optional = std::static_pointer_cast<joyeer::syntax::OptionalTypeSyntax>(
            binding->annotation);
    ASSERT_EQ(optional->wrapped->kind, Kind::nominalType);
    const auto resultType = std::static_pointer_cast<joyeer::syntax::NominalTypeSyntax>(
            optional->wrapped);
    EXPECT_EQ(resultType->name->rawValue, "Result");
    ASSERT_EQ(resultType->arguments.size(), 2u);
    EXPECT_EQ(resultType->arguments[0]->kind, Kind::dictionaryType);

    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[1]);
    ASSERT_EQ(function->parameters.size(), 2u);
    EXPECT_EQ(function->parameters[0]->label->rawValue, "p");
    EXPECT_EQ(function->parameters[0]->name->rawValue, "p");
        ASSERT_NE(function->parameters[0]->accessKeyword, nullptr);
    EXPECT_EQ(function->parameters[1]->label->rawValue, "from");
    EXPECT_EQ(function->parameters[1]->name->rawValue, "source");
    EXPECT_EQ(function->returnType->kind, Kind::optionalType);

    const auto structure = std::static_pointer_cast<joyeer::syntax::StructDeclSyntax>(
            result.root->items[2]);
    ASSERT_EQ(structure->fields.size(), 2u);
    EXPECT_EQ(structure->fields[0]->name->rawValue, "input");
    EXPECT_EQ(structure->fields[1]->initializer->kind, Kind::literalExpr);

    const auto enumeration = std::static_pointer_cast<joyeer::syntax::EnumDeclSyntax>(
            result.root->items[3]);
    ASSERT_EQ(enumeration->cases.size(), 2u);
    ASSERT_EQ(enumeration->cases[1]->associatedTypes.size(), 2u);
    EXPECT_EQ(enumeration->cases[1]->associatedTypes[0]->label, nullptr);
    ASSERT_NE(enumeration->cases[1]->associatedTypes[1]->label, nullptr);
    EXPECT_EQ(enumeration->cases[1]->associatedTypes[1]->label->rawValue, "at");
}

TEST_F(ParserTest, BuildsPrecedenceAndAssociativityInTheParserAst) {
    parse(R"JOYEER(let x = 1 + 2 * 3
let y = a - b - c
let z = a = b = value
)JOYEER");

    ASSERT_TRUE(result.succeeded());
    const auto x = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(result.root->items[0]);
    const auto add = std::static_pointer_cast<joyeer::syntax::BinaryExprSyntax>(x->initializer);
    EXPECT_EQ(add->op->kind, plus);
    ASSERT_EQ(add->right->kind, Kind::binaryExpr);
    EXPECT_EQ(std::static_pointer_cast<joyeer::syntax::BinaryExprSyntax>(add->right)->op->kind,
              multiply);

    const auto y = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(result.root->items[1]);
    const auto outerSubtract = std::static_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            y->initializer);
    EXPECT_EQ(outerSubtract->op->kind, minus);
    EXPECT_EQ(outerSubtract->left->kind, Kind::binaryExpr);

    const auto z = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(result.root->items[2]);
    const auto outerAssignment = std::static_pointer_cast<joyeer::syntax::AssignmentExprSyntax>(
            z->initializer);
    EXPECT_EQ(outerAssignment->target->kind, Kind::nameExpr);
    EXPECT_EQ(outerAssignment->value->kind, Kind::assignmentExpr);
}

TEST_F(ParserTest, DiagnosesChainedComparisonsAndInvalidAssignmentTargets) {
    parse(R"JOYEER(let chained = a < b < c
let invalid = (a + b) = c
)JOYEER");

    EXPECT_TRUE(hasDiagnostic(DiagnosticId::chainedComparison));
    EXPECT_TRUE(hasDiagnostic(DiagnosticId::invalidAssignmentTarget));
    EXPECT_EQ(result.root->items.size(), 2u);
}

TEST_F(ParserTest, ParsesLogicalOperatorsWithSpecPrecedence) {
    parse("let value = target = !a || b && c == d < e + f * g\n");

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[0]);
    const auto assignment = std::dynamic_pointer_cast<joyeer::syntax::AssignmentExprSyntax>(
            binding->initializer);
    ASSERT_NE(assignment, nullptr);
    const auto disjunction = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            assignment->value);
    ASSERT_NE(disjunction, nullptr);
    EXPECT_EQ(disjunction->op->kind, orOr);
    const auto negation = std::dynamic_pointer_cast<joyeer::syntax::PrefixExprSyntax>(
            disjunction->left);
    ASSERT_NE(negation, nullptr);
    EXPECT_EQ(negation->op->kind, bang);
    const auto conjunction = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            disjunction->right);
    ASSERT_NE(conjunction, nullptr);
    EXPECT_EQ(conjunction->op->kind, andAnd);
    const auto equality = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            conjunction->right);
    ASSERT_NE(equality, nullptr);
    EXPECT_EQ(equality->op->kind, equalEqual);
    const auto comparison = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            equality->right);
    ASSERT_NE(comparison, nullptr);
    EXPECT_EQ(comparison->op->kind, less);
    const auto addition = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            comparison->right);
    ASSERT_NE(addition, nullptr);
    EXPECT_EQ(addition->op->kind, plus);
    const auto product = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            addition->right);
    ASSERT_NE(product, nullptr);
    EXPECT_EQ(product->op->kind, multiply);
}

TEST_F(ParserTest, ParsesDivisionAndRemainderLeftAssociatively) {
    parse("let value = 2 + 24 / 3 % 5 * 4\n");

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[0]);
    const auto addition = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            binding->initializer);
    ASSERT_NE(addition, nullptr);
    EXPECT_EQ(addition->op->kind, plus);
    const auto product = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            addition->right);
    ASSERT_NE(product, nullptr);
    EXPECT_EQ(product->op->kind, multiply);
    const auto remainder = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            product->left);
    ASSERT_NE(remainder, nullptr);
    EXPECT_EQ(remainder->op->kind, percentage);
    const auto quotient = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            remainder->left);
    ASSERT_NE(quotient, nullptr);
    EXPECT_EQ(quotient->op->kind, divide);
}

TEST_F(ParserTest, ParsesLogicalOrLeftAssociatively) {
    parse("let value = a || b || c\n");

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[0]);
    const auto outer = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            binding->initializer);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op->kind, orOr);
    const auto inner = std::dynamic_pointer_cast<joyeer::syntax::BinaryExprSyntax>(outer->left);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op->kind, orOr);
    EXPECT_EQ(outer->right->kind, Kind::nameExpr);
}

TEST_F(ParserTest, ParsesPrefixNotAfterReturnAndBeforePostfixChains) {
    parse("func run() { return !!items[0].ready(value: true)? }\n"
          "func bare() { return\n!false }\n");

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    const auto outer = std::dynamic_pointer_cast<joyeer::syntax::PrefixExprSyntax>(
            returned->value);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op->kind, bang);
    const auto inner = std::dynamic_pointer_cast<joyeer::syntax::PrefixExprSyntax>(outer->operand);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op->kind, bang);
    EXPECT_EQ(inner->operand->kind, Kind::propagateExpr);
    const auto bare = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[1]);
    ASSERT_EQ(bare->body->items.size(), 2u);
    EXPECT_EQ(std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            bare->body->items[0])->value, nullptr);
    EXPECT_EQ(bare->body->items[1]->kind, Kind::prefixExpr);
}

TEST_F(ParserTest, RejectsPostfixForceUnwrapAndMissingNotOperand) {
    parse("func run() { value!\nlet kept = 1 }\n");
    EXPECT_TRUE(hasDiagnostic(DiagnosticId::sameLineItems));
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(function->body->items.size(), 2u);
    EXPECT_EQ(function->body->items[0]->kind, Kind::nameExpr);
    EXPECT_EQ(function->body->items[1]->kind, Kind::bindingDecl);

    parse("func run() { ! }\n");
    EXPECT_TRUE(hasDiagnostic(DiagnosticId::expectedExpression));
}

TEST_F(ParserTest, SuggestsMissingDelimiterInsertion) {
    parse(R"JOYEER(func value(): Int {
return (1 + 2
}
)JOYEER");

    const auto found = std::find_if(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == DiagnosticId::expectedToken &&
                        diagnostic.fixIt.has_value();
            });
    ASSERT_NE(found, result.diagnostics.end())
            << joyeer::parser::dump(result.diagnostics);
    ASSERT_TRUE(found->help.has_value());
    EXPECT_EQ(found->fixIt->length, 0u);
    EXPECT_EQ(found->fixIt->replacement, ")");
}

TEST_F(ParserTest, SuggestsMismatchedCloserReplacement) {
        const std::string text = R"JOYEER(func value(): Int {
return (1 + 2]
}
)JOYEER";
        parse(text);

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::expectedToken &&
                                                diagnostic.fixIt.has_value() &&
                                                diagnostic.fixIt->replacement == ")";
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        ASSERT_TRUE(found->help.has_value());
        EXPECT_EQ(found->fixIt->length, 1u);

        parse(applyFixIt(text, *found->fixIt));
        EXPECT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
}

TEST_F(ParserTest, PreservesClosersOwnedByAnyOuterDelimiter) {
        parse(R"JOYEER(func value(): Int {
return [(1 + 2}]
}
)JOYEER");

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::expectedToken &&
                                                diagnostic.fixIt.has_value() &&
                                                diagnostic.fixIt->replacement == ")";
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        EXPECT_EQ(found->fixIt->length, 0u);
}

TEST_F(ParserTest, SuggestsMatchArrowReplacement) {
        const std::string text = R"JOYEER(func value(flag: Bool): Int {
return match flag {
true = 1,
false => 0,
}
}
)JOYEER";
        parse(text);

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::expectedToken &&
                                                diagnostic.fixIt.has_value() &&
                                                diagnostic.fixIt->replacement == "=>";
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        ASSERT_TRUE(found->help.has_value());
        EXPECT_EQ(found->fixIt->length, 1u);

        parse(applyFixIt(text, *found->fixIt));
        EXPECT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
}

TEST_F(ParserTest, SuggestsContiguousMatchArrowReplacement) {
        const std::string text = R"JOYEER(func value(flag: Bool): Int {
return match flag {
true -> 1,
false => 0,
}
}
)JOYEER";
        parse(text);

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::expectedToken &&
                                                diagnostic.fixIt.has_value() &&
                                                diagnostic.fixIt->replacement == "=>";
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        EXPECT_EQ(found->fixIt->length, 2u);

        parse(applyFixIt(text, *found->fixIt));
        EXPECT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
}

TEST_F(ParserTest, DoesNotReplaceMatchArrowAcrossTrivia) {
        parse(R"JOYEER(func value(flag: Bool): Int {
return match flag {
true - > 1,
false => 0,
}
}
)JOYEER");

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::expectedToken &&
                                                diagnostic.fixIt.has_value() &&
                                                diagnostic.fixIt->replacement == "=>";
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        EXPECT_EQ(found->fixIt->length, 0u);
}

TEST_F(ParserTest, SuggestsEmptyEnumPayloadDeletion) {
        const std::string text = "enum Value { None() }\n";
        parse(text);

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::missingListElement &&
                                                diagnostic.fixIt.has_value();
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        ASSERT_TRUE(found->help.has_value());
        EXPECT_EQ(found->fixIt->length, 2u);
        EXPECT_TRUE(found->fixIt->replacement.empty());

        parse(applyFixIt(text, *found->fixIt));
        EXPECT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
}

TEST_F(ParserTest, DoesNotDeleteTriviaInsideEmptyEnumPayload) {
        parse("enum Value { None( /* keep */ ) }\n");

        const auto found = std::find_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::missingListElement;
                        });
        ASSERT_NE(found, result.diagnostics.end())
                        << joyeer::parser::dump(result.diagnostics);
        EXPECT_FALSE(found->fixIt.has_value());
}

TEST_F(ParserTest, SuggestsMissingCommaInsertion) {
    parse(R"JOYEER(func value(left: Int right: Int): Int {
return left
}
)JOYEER");

    const auto found = std::find_if(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == DiagnosticId::missingComma &&
                        diagnostic.fixIt.has_value();
            });
    ASSERT_NE(found, result.diagnostics.end())
            << joyeer::parser::dump(result.diagnostics);
    ASSERT_TRUE(found->help.has_value());
    EXPECT_EQ(found->fixIt->length, 0u);
    EXPECT_EQ(found->fixIt->replacement, ",");
}

TEST_F(ParserTest, ParsesPostfixChainsCallsAndContextualCases) {
    parse(R"JOYEER(func parse(p: inout Parser) {
parseValue(p: &p)
let error = .Unexpected(b'{', at: p.pos)
let parser = Parser(input: "x", pos: 0)
return parser.input[p.pos]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded());
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(function->body->items.size(), 4u);

    const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            function->body->items[0]);
    ASSERT_EQ(call->arguments.size(), 1u);
    EXPECT_EQ(call->arguments[0]->label->rawValue, "p");
    ASSERT_NE(call->arguments[0]->accessMarker, nullptr);

    const auto errorBinding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            function->body->items[1]);
    const auto enumCase = std::static_pointer_cast<joyeer::syntax::ContextualCaseExprSyntax>(
            errorBinding->initializer);
    EXPECT_EQ(enumCase->name->rawValue, "Unexpected");
    ASSERT_EQ(enumCase->arguments.size(), 2u);
    EXPECT_EQ(enumCase->arguments[0]->label, nullptr);
    EXPECT_EQ(enumCase->arguments[1]->label->rawValue, "at");

    const auto returnExpr = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[3]);
    EXPECT_EQ(returnExpr->value->kind, Kind::subscriptExpr);
}

TEST_F(ParserTest, ParsesPostfixPropagationAfterOperandsBeforeInfixOperators) {
    parse("func run(value: Int?): Int? {\n"
          "return .Some(value? + 1)\n"
          "}\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    const auto some = std::static_pointer_cast<joyeer::syntax::ContextualCaseExprSyntax>(
            returned->value);
    const auto addition = std::static_pointer_cast<joyeer::syntax::BinaryExprSyntax>(
            some->arguments[0]->value);
    ASSERT_EQ(addition->left->kind, Kind::propagateExpr);
    EXPECT_EQ(addition->left->span.offset, source->content.find("value?"));
    EXPECT_EQ(addition->left->span.length, 6u);
    EXPECT_NE(joyeer::syntax::dump(result.root).find("propagate_expr"), std::string::npos);
}

TEST_F(ParserTest, ParsesConsumingParametersAndArguments) {
    parse(R"JOYEER(func take(value: consuming String) { print(value: value) }
func run() {
let text = "owned"
take(value: consume text)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto take = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(take->parameters.size(), 1u);
    EXPECT_EQ(
            take->parameters[0]->accessEffect(),
            joyeer::syntax::AccessEffect::consuming);
    ASSERT_NE(take->parameters[0]->accessKeyword, nullptr);
    EXPECT_EQ(take->parameters[0]->accessKeyword->kind, kwConsuming);

    const auto run = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[1]);
    const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            run->body->items[1]);
    ASSERT_EQ(call->arguments.size(), 1u);
    ASSERT_NE(call->arguments[0]->accessMarker, nullptr);
    EXPECT_EQ(call->arguments[0]->accessMarker->kind, kwConsume);
}

TEST_F(ParserTest, ParsesExplicitBorrowingParameters) {
        parse(R"JOYEER(func inspect(value: borrowing String) { print(value: value) }
)JOYEER");

        ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                        result.root->items[0]);
        ASSERT_EQ(function->parameters.size(), 1u);
        EXPECT_EQ(
                        function->parameters[0]->accessEffect(),
                        joyeer::syntax::AccessEffect::borrowing);
        ASSERT_NE(function->parameters[0]->accessKeyword, nullptr);
        EXPECT_EQ(function->parameters[0]->accessKeyword->kind, kwBorrowing);
}

        TEST_F(ParserTest, ParsesInitializingParametersAndArguments) {
            parse(R"JOYEER(func initialize(out: initializing String) { &out = "value" }
        func run() {
        var text: String
        initialize(out: &text)
        }
        )JOYEER");

            ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
            const auto initialize = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                    result.root->items[0]);
            ASSERT_EQ(initialize->parameters.size(), 1u);
            EXPECT_EQ(
                    initialize->parameters[0]->accessEffect(),
                    joyeer::syntax::AccessEffect::initializing);
            ASSERT_NE(initialize->parameters[0]->accessKeyword, nullptr);
            EXPECT_EQ(initialize->parameters[0]->accessKeyword->kind, kwInitializing);

            const auto run = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                    result.root->items[1]);
            const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                    run->body->items[1]);
            ASSERT_NE(call->arguments[0]->accessMarker, nullptr);
            EXPECT_EQ(call->arguments[0]->accessMarker->kind, ampersand);
        }

TEST_F(ParserTest, ParsesArraysAndDictionariesWithTrailingCommas) {
    parse(R"JOYEER(let array = [1, 2, 3,]
let dictionary = ["one": 1, "two": 2,]
let emptyArray = []
let emptyDictionary = [:]
)JOYEER");

    ASSERT_TRUE(result.succeeded());
    const auto array = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(array->initializer->kind, Kind::arrayExpr);
    EXPECT_EQ(std::static_pointer_cast<joyeer::syntax::ArrayExprSyntax>(array->initializer)
                      ->elements.size(),
              3u);

    const auto dictionary = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            result.root->items[1]);
    ASSERT_EQ(dictionary->initializer->kind, Kind::dictionaryExpr);
    EXPECT_EQ(std::static_pointer_cast<joyeer::syntax::DictionaryExprSyntax>(
                      dictionary->initializer)->entries.size(),
              2u);
}

TEST_F(ParserTest, ParsesWhileIfAndBareOrValuedReturns) {
    parse(R"JOYEER(func normalize(x: inout Int): Int {
while x < 0 { &x = x + 1 }
if x == 0 { return
x }
return if x > 0 { x } else { -x }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded());
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(function->body->items.size(), 3u);
    EXPECT_EQ(function->body->items[0]->kind, Kind::whileStmt);
    EXPECT_EQ(function->body->items[1]->kind, Kind::ifExpr);

    const auto conditional = std::static_pointer_cast<joyeer::syntax::IfExprSyntax>(
            function->body->items[1]);
    ASSERT_EQ(conditional->thenBranch->items.size(), 2u);
    const auto bareReturn = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            conditional->thenBranch->items[0]);
    EXPECT_EQ(bareReturn->value, nullptr);

    const auto valuedReturn = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[2]);
    EXPECT_EQ(valuedReturn->value->kind, Kind::ifExpr);
}

TEST_F(ParserTest, ParsesBreakAndContinueAsSpannedLeafStatements) {
    const std::string text = "func run() {\nbreak\ncontinue\n}\n";
    parse(text);

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    ASSERT_EQ(function->body->items.size(), 2u);
    const auto breakStatement = std::dynamic_pointer_cast<joyeer::syntax::BreakStmtSyntax>(
            function->body->items[0]);
    ASSERT_NE(breakStatement, nullptr);
    EXPECT_EQ(breakStatement->kind, Kind::breakStmt);
    EXPECT_EQ(breakStatement->span.offset, text.find("break"));
    EXPECT_EQ(breakStatement->span.length, 5u);
    EXPECT_EQ(joyeer::syntax::dump(breakStatement),
              "break_stmt@" + std::to_string(text.find("break")) + ":5\n");
    const auto continueStatement = std::dynamic_pointer_cast<joyeer::syntax::ContinueStmtSyntax>(
            function->body->items[1]);
    ASSERT_NE(continueStatement, nullptr);
    EXPECT_EQ(continueStatement->kind, Kind::continueStmt);
    EXPECT_EQ(continueStatement->span.offset, text.find("continue"));
    EXPECT_EQ(continueStatement->span.length, 8u);
    EXPECT_EQ(joyeer::syntax::dump(continueStatement),
              "continue_stmt@" + std::to_string(text.find("continue")) + ":8\n");
    EXPECT_EQ(std::dynamic_pointer_cast<joyeer::syntax::ExprSyntax>(breakStatement), nullptr);
    EXPECT_EQ(std::dynamic_pointer_cast<joyeer::syntax::ExprSyntax>(continueStatement), nullptr);
}

TEST_F(ParserTest, ParsesLoopControlInNestedBlocks) {
    parse("func run() {\nwhile true {\n"
          "if true { break }\nwhile false { continue }\ncontinue\n}\n}\n");

    ASSERT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[0]);
    const auto outer = std::static_pointer_cast<joyeer::syntax::WhileStmtSyntax>(
            function->body->items[0]);
    ASSERT_EQ(outer->body->items.size(), 3u);
    const auto conditional = std::static_pointer_cast<joyeer::syntax::IfExprSyntax>(
            outer->body->items[0]);
    EXPECT_EQ(conditional->thenBranch->items[0]->kind, Kind::breakStmt);
    const auto inner = std::static_pointer_cast<joyeer::syntax::WhileStmtSyntax>(
            outer->body->items[1]);
    EXPECT_EQ(inner->body->items[0]->kind, Kind::continueStmt);
    EXPECT_EQ(outer->body->items[2]->kind, Kind::continueStmt);
}

TEST_F(ParserTest, RejectsLoopControlLabelsAndValuesAndRecoversAtFollowingJumps) {
    for (const auto* statement : {
            "break outer", "continue outer", "break 1", "continue false",
            "break: outer", "continue: outer", "break continue"}) {
        SCOPED_TRACE(statement);
        parse(std::string("func run() {\n") + statement + "\ncontinue\nbreak\n}\n");

        ASSERT_EQ(result.diagnostics.size(), 1u) << joyeer::parser::dump(result.diagnostics);
        EXPECT_EQ(result.diagnostics[0].id, DiagnosticId::sameLineItems);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                result.root->items[0]);
        ASSERT_EQ(function->body->items.size(), 3u);
        EXPECT_EQ(function->body->items[1]->kind, Kind::continueStmt);
        EXPECT_EQ(function->body->items[2]->kind, Kind::breakStmt);
    }
}

TEST_F(ParserTest, RejectsLoopControlOutsideBlockStatementPositions) {
    for (const auto* text : {
            "break\n", "continue\n", "let value = break\n", "let value = continue\n",
            "func run() { call(value: break) }\n",
            "func run() { return continue }\n",
            "func run() { match true { true => break } }\n"}) {
        SCOPED_TRACE(text);
        parse(text);
        EXPECT_FALSE(result.succeeded());
    }
}

TEST_F(ParserTest, ParsesJsonMvpMatchPatternsAndDivergingArm) {
        parse(readFixture("parser/ok/json_mvp.joyeer"));

        ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.root->items.size(), 5u);
    const auto parseValue = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            result.root->items[4]);
    ASSERT_EQ(parseValue->body->items.size(), 3u);

    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            parseValue->body->items[0]);
    const auto firstMatch = std::static_pointer_cast<joyeer::syntax::MatchExprSyntax>(
            binding->initializer);
    ASSERT_EQ(firstMatch->arms.size(), 2u);
    EXPECT_EQ(firstMatch->arms[0]->pattern->kind, Kind::enumCasePattern);
    EXPECT_EQ(firstMatch->arms[1]->body->kind, Kind::returnExpr);

    const auto finalReturn = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            parseValue->body->items[2]);
    const auto secondMatch = std::static_pointer_cast<joyeer::syntax::MatchExprSyntax>(
            finalReturn->value);
    ASSERT_EQ(secondMatch->arms.size(), 3u);
    EXPECT_EQ(secondMatch->arms[0]->pattern->kind, Kind::literalPattern);
    EXPECT_EQ(secondMatch->arms[2]->pattern->kind, Kind::wildcardPattern);
}

TEST_F(ParserTest, RecoversToLaterTopLevelDeclaration) {
    parse("unexpected\nlet kept = 2\n");

    ASSERT_FALSE(result.succeeded());
    ASSERT_EQ(result.root->items.size(), 2u);
    EXPECT_EQ(result.root->items[0]->kind, Kind::errorDecl);
    ASSERT_EQ(result.root->items[1]->kind, Kind::bindingDecl);
    EXPECT_EQ(std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
                      result.root->items[1])->name->rawValue,
              "kept");
}

TEST_F(ParserTest, MissingCloserDoesNotConsumeNextLineDeclaration) {
    parse("let bad = [1, 2\nlet kept = 3\n");

    ASSERT_FALSE(result.succeeded());
    ASSERT_EQ(result.root->items.size(), 2u);
    EXPECT_EQ(result.root->items[1]->kind, Kind::bindingDecl);
    EXPECT_TRUE(hasDiagnostic(DiagnosticId::expectedToken));
}

TEST_F(ParserTest, MissingBodyClosersPreserveLaterFunctions) {
        parse(R"JOYEER(enum Broken { Case
func kept(): Int { return 1 }
)JOYEER");

        ASSERT_FALSE(result.succeeded());
        ASSERT_EQ(result.root->items.size(), 2u);
        EXPECT_EQ(result.root->items[0]->kind, Kind::enumDecl);
        EXPECT_EQ(result.root->items[1]->kind, Kind::functionDecl);
        EXPECT_TRUE(hasDiagnostic(DiagnosticId::expectedToken));
}

TEST_F(ParserTest, DiagnosesAdjacentSameLineBlockItems) {
    parse("func f() { let x = 1 let y = 2 }\n");

    EXPECT_TRUE(hasDiagnostic(DiagnosticId::sameLineItems));
    ASSERT_EQ(result.root->items.size(), 1u);
}

TEST_F(ParserTest, RecoversFromIncompleteParameterAndPayloadLabels) {
        parse(R"JOYEER(func missingType(value:) { return }
enum Broken { Case(value:) }
let kept = 1
)JOYEER");

        ASSERT_FALSE(result.succeeded());
        ASSERT_EQ(result.root->items.size(), 3u);
        const auto expectedTypeCount = std::count_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::expectedType;
                        });
        EXPECT_EQ(expectedTypeCount, 2);
        EXPECT_EQ(result.root->items[2]->kind, Kind::bindingDecl);
}

TEST_F(ParserTest, RejectsEmptyContextualAndPatternPayloadClauses) {
        parse(R"JOYEER(func inspect(value: Value): Int {
        let selected = .Null()
        return match value {
                .Null() => selected,
                _ => 0,
        }
}
)JOYEER");

        ASSERT_FALSE(result.succeeded());
        const auto emptyPayloadCount = std::count_if(
                        result.diagnostics.begin(),
                        result.diagnostics.end(),
                        [](const auto& diagnostic) {
                                return diagnostic.id == DiagnosticId::missingListElement;
                        });
        EXPECT_EQ(emptyPayloadCount, 2);
}

TEST_F(ParserTest, MissingMatchArrowRecoversToFollowingArm) {
        parse(R"JOYEER(func inspect(value: Value): Int {
        return match value {
                .Some(value) value,
                _ => 0,
        }
}
)JOYEER");

        ASSERT_FALSE(result.succeeded());
        ASSERT_TRUE(hasDiagnostic(DiagnosticId::expectedToken));
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                        result.root->items[0]);
        const auto returnExpr = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                        function->body->items[0]);
        const auto match = std::static_pointer_cast<joyeer::syntax::MatchExprSyntax>(
                        returnExpr->value);
        EXPECT_EQ(match->arms.size(), 2u);
}

TEST_F(ParserTest, MatchArmsWithoutCommasMustStartOnNewLines) {
        parse(R"JOYEER(func inspect(value: Value): Int {
        return match value { .A => 1 .B => 2 }
}
)JOYEER");

        ASSERT_FALSE(result.succeeded());
        EXPECT_TRUE(hasDiagnostic(DiagnosticId::missingComma));
}

TEST_F(ParserTest, ConsumesLexerErrorTokenWithoutParserCascade) {
    parse("`\nlet kept = 1\n", false);

    ASSERT_FALSE(lexerDiagnostics.errors.empty());
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.root->items.size(), 2u);
    EXPECT_EQ(result.root->items[0]->kind, Kind::errorDecl);
    EXPECT_EQ(result.root->items[1]->kind, Kind::bindingDecl);
}

TEST_F(ParserTest, TokenDeletionMutationsAlwaysTerminateWithinSourceBounds) {
    const std::string text = R"JOYEER(func f(x: Int): Int {
let y = [x, 2]
return if y[0] > 0 { y[0] } else { 0 }
}
)JOYEER";
    parse(text);
    ASSERT_TRUE(lexerDiagnostics.errors.empty());
    const auto originalTokens = source->tokens;

    for (size_t removed = 0; removed < originalTokens.size(); ++removed) {
        auto mutated = originalTokens;
        mutated.erase(mutated.begin() + static_cast<std::ptrdiff_t>(removed));
        joyeer::parser::Parser parser(mutated);
        const auto mutationResult = parser.parse();
        ASSERT_NE(mutationResult.root, nullptr) << "removed token " << removed;
        EXPECT_LE(static_cast<uint64_t>(mutationResult.root->span.offset) +
                          mutationResult.root->span.length,
                  text.size()) << "removed token " << removed;
    }
}

TEST_F(ParserTest, TokenInsertionAndReplacementMutationsAlwaysTerminate) {
        const std::string text = R"JOYEER(func f(x: Int): Int {
return x + 1
}
)JOYEER";
        parse(text);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());
        const auto originalTokens = source->tokens;

        for (size_t index = 0; index < originalTokens.size(); ++index) {
                auto inserted = originalTokens;
                inserted.insert(inserted.begin() + static_cast<std::ptrdiff_t>(index),
                                                originalTokens[index]);
                joyeer::parser::Parser insertionParser(inserted);
                const auto insertionResult = insertionParser.parse();
                ASSERT_NE(insertionResult.root, nullptr) << "inserted token " << index;

                auto replaced = originalTokens;
                const auto& original = originalTokens[index];
                replaced[index] = std::make_shared<Token>(
                                invalid,
                                original->rawValue,
                                original->span,
                                original->lineNumber,
                                original->columnAt,
                                original->startsLine);
                joyeer::parser::Parser replacementParser(replaced);
                const auto replacementResult = replacementParser.parse();
                ASSERT_NE(replacementResult.root, nullptr) << "replaced token " << index;
                EXPECT_LE(static_cast<uint64_t>(replacementResult.root->span.offset) +
                                                  replacementResult.root->span.length,
                                  text.size()) << "replaced token " << index;
        }
}

TEST_F(ParserTest, AstDumpIsDeterministicAndContainsRoles) {
    parse("let x = 1 + 2 * 3\n");

    const std::string first = joyeer::syntax::dump(result.root);
    const std::string second = joyeer::syntax::dump(result.root);
    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("binding_decl"), std::string::npos);
    EXPECT_NE(first.find("binary_expr"), std::string::npos);
    EXPECT_NE(first.find("op=+"), std::string::npos);
    EXPECT_NE(first.find("op=*"), std::string::npos);
}

TEST_F(ParserTest, ErrorCorpusMatchesDiagnosticSnapshot) {
        parse(readFixture("parser/err/recovery.joyeer"));

        ASSERT_FALSE(result.succeeded());
        EXPECT_EQ(joyeer::parser::dump(result.diagnostics),
                          readFixture("parser/err/recovery.diag.txt"));
}

} // namespace
