#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

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

    TEST_F(TypeCheckingTest, TypesReadFileAsResultOfOwnedStringOrErrorCode) {
        check(R"JOYEER(func load() {
    let loaded = readFile(path: "input.json")
    }
    )JOYEER");

        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
        const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            function->body->items[0]);
        EXPECT_EQ(
            checking.model->types().displayName(declaredType(binding)),
            "Result<String, Int>");

        const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            binding->initializer);
        const auto target = checking.model->callTarget(call);
        ASSERT_TRUE(target.has_value());
        const auto* signature = checking.model->callable(*target);
        ASSERT_NE(signature, nullptr);
        ASSERT_EQ(signature->parameters.size(), 1u);
        EXPECT_EQ(
            checking.model->types().displayName(signature->parameters[0]),
            "String");
        EXPECT_EQ(
            checking.model->types().displayName(signature->result),
            "Result<String, Int>");
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
    let integer = byteToInt(value: 1)
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
                return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::typeMismatch;
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
                    }
                    func invalid() {
                    let text = "owned"
                    take(value: text)
                    inspect(value: consume text)
                    var values = ["item"]
                    take(value: consume values[0])
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
                            func validTemporary(index: Int) {
                            var value = 1
                            update(dst: &value, src: value + index)
                            }
                            )JOYEER");

                                ASSERT_EQ(checking.diagnostics.size(), 5u)
                                    << joyeer::typing::dump(checking.diagnostics);
                                EXPECT_TRUE(std::all_of(
                                    checking.diagnostics.begin(),
                                    checking.diagnostics.end(),
                                    [](const auto& diagnostic) {
                                    return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::
                                        overlappingAccess;
                                    }));
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

} // namespace