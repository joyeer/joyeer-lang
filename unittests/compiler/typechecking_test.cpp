#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using joyeer::typing::TypeContext;
using joyeer::typing::TypeKind;

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
        const auto context = std::make_shared<CompileContext>(
            &lexerDiagnostics,
            std::make_shared<SymbolTable>());
        LexParser lexer(context, LexerProfile::jsonParserMvp);
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

        joyeer::typing::TypeId declaredType(const joyeer::syntax::NodePtr& node) const {
        const auto symbol = resolution.model->declaredSymbol(node);
        EXPECT_TRUE(symbol.has_value());
        const auto type = symbol.has_value()
            ? checking.model->typeOf(*symbol)
            : std::optional<joyeer::typing::TypeId>();
        EXPECT_TRUE(type.has_value());
        return type.value_or(joyeer::typing::invalidTypeId);
        }

        Diagnostics lexerDiagnostics;
        SourceFile::Ptr source;
        joyeer::parser::ParseResult parseResult;
        joyeer::semantic::NameResolutionResult resolution;
        joyeer::typing::TypeCheckingResult checking;
    };

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

} // namespace