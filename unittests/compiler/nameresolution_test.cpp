#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using joyeer::semantic::DeferredResolutionKind;
using joyeer::semantic::NameResolutionDiagnosticId;
using joyeer::semantic::SymbolKind;
using joyeer::syntax::Kind;

std::string readFixture(const std::string& relativePath) {
    const std::string path = std::string(JOYEER_TESTS_DIR) + "/" + relativePath;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open name-resolution fixture: " + path);
    }
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

class NameResolutionTest : public testing::Test {
protected:
    void resolve(const std::string& text) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser lexer(&lexerDiagnostics);
        lexer.parse(source);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());

        joyeer::parser::Parser parser(source->tokens);
        parseResult = parser.parse();
        if (!parseResult.diagnostics.empty()) {
            ADD_FAILURE() << joyeer::parser::dump(parseResult.diagnostics);
        }
        ASSERT_TRUE(parseResult.succeeded());

        resolution = joyeer::semantic::NameResolver().resolve(parseResult.root);
        ASSERT_NE(resolution.model, nullptr);
    }

    bool hasDiagnostic(NameResolutionDiagnosticId id) const {
        return std::any_of(
                resolution.diagnostics.begin(),
                resolution.diagnostics.end(),
                [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

    const joyeer::semantic::Symbol& referenced(const joyeer::syntax::NodePtr& node) const {
        const auto id = resolution.model->referencedSymbol(node);
        EXPECT_TRUE(id.has_value());
        const auto* value = id.has_value() ? resolution.model->symbol(*id) : nullptr;
        EXPECT_NE(value, nullptr);
        return *value;
    }

    const joyeer::semantic::Symbol& declared(const joyeer::syntax::NodePtr& node) const {
        const auto id = resolution.model->declaredSymbol(node);
        EXPECT_TRUE(id.has_value());
        const auto* value = id.has_value() ? resolution.model->symbol(*id) : nullptr;
        EXPECT_NE(value, nullptr);
        return *value;
    }

    Diagnostics lexerDiagnostics;
    SourceFile::Ptr source;
    joyeer::parser::ParseResult parseResult;
    joyeer::semantic::NameResolutionResult resolution;
};

TEST_F(NameResolutionTest, ResolvesForwardFunctionsTypesParametersAndFields) {
    resolve(R"JOYEER(func first(item: Item): Int {
return second(item: item)
}
struct Item {
var count: Int
}
func second(item: Item): Int {
return item.count
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    ASSERT_EQ(parseResult.root->items.size(), 3u);

    const auto first = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    const auto itemType = std::static_pointer_cast<joyeer::syntax::NominalTypeSyntax>(
            first->parameters[0]->type);
    EXPECT_EQ(referenced(itemType).kind, SymbolKind::structure);
    EXPECT_EQ(referenced(itemType).name, "Item");

    const auto returnExpression = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            first->body->items[0]);
    const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            returnExpression->value);
    EXPECT_EQ(referenced(call->callee).kind, SymbolKind::function);
    EXPECT_EQ(referenced(call->callee).name, "second");

    const auto second = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[2]);
    const auto secondReturn = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            second->body->items[0]);
    const auto field = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
            secondReturn->value);
    EXPECT_EQ(referenced(field).kind, SymbolKind::structureField);
    EXPECT_EQ(referenced(field).name, "count");
}

TEST_F(NameResolutionTest, ResolvesReadFileFromThePrelude) {
        resolve(R"JOYEER(func load(): Result<String, IOError> {
return readFile(path: "input.json")
}
)JOYEER");

        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                        parseResult.root->items[0]);
        const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                        function->body->items[0]);
        const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                        returned->value);

        EXPECT_EQ(referenced(call->callee).kind, SymbolKind::builtinFunction);
        EXPECT_EQ(referenced(call->callee).name, "readFile");
        const auto target = resolution.model->callTarget(call);
        ASSERT_TRUE(target.has_value());
        EXPECT_EQ(resolution.model->symbol(*target)->name, "readFile");
}

TEST_F(NameResolutionTest, ResolvesStringUtf8FromTheBuiltinMemberScope) {
    resolve(R"JOYEER(func bytes(text: String): [UInt8] {
return text.utf8()
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            returned->value);
    const auto member = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
            call->callee);

    EXPECT_EQ(referenced(member).kind, SymbolKind::builtinMember);
    EXPECT_EQ(referenced(member).name, "utf8");
    const auto target = resolution.model->callTarget(call);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(resolution.model->symbol(*target)->name, "utf8");
}

        TEST_F(NameResolutionTest, ResolvesArrayAppendFromTheBuiltinMemberScope) {
            resolve(R"JOYEER(func add(values: inout [Int]) {
        &values.append(element: 42)
        }
        )JOYEER");

            ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
            const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                    parseResult.root->items[0]);
            const auto access = std::static_pointer_cast<joyeer::syntax::AccessExprSyntax>(
                    function->body->items[0]);
            const auto call = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                    access->operand);
            const auto member = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
                    call->callee);

            EXPECT_EQ(referenced(member).kind, SymbolKind::builtinMember);
            EXPECT_EQ(referenced(member).name, "append");
            const auto target = resolution.model->callTarget(call);
            ASSERT_TRUE(target.has_value());
            EXPECT_EQ(resolution.model->symbol(*target)->name, "append");
        }

        TEST_F(NameResolutionTest, ResolvesExplicitByteConversionsFromThePrelude) {
            resolve(R"JOYEER(func convert(value: UInt8): String {
        print(value: byteToInt(value: value))
        return byteToString(value: value)
        }
        )JOYEER");

            ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
            const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                    parseResult.root->items[0]);
            const auto printCall = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                    function->body->items[0]);
            const auto integerCall = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                    printCall->arguments[0]->value);
            const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                    function->body->items[1]);
            const auto stringCall = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
                    returned->value);
            EXPECT_EQ(referenced(integerCall->callee).name, "byteToInt");
            EXPECT_EQ(referenced(stringCall->callee).name, "byteToString");
        }

TEST_F(NameResolutionTest, ResolvesMemberChainsThroughLaterTypeSignatures) {
    resolve(R"JOYEER(func read(node: Node): Int {
return node.child.value
}
struct Node {
var child: Child
}
struct Child {
var value: Int
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    const auto valueMember = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
            returned->value);
    const auto childMember = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
            valueMember->base);

    EXPECT_EQ(referenced(childMember).name, "child");
    EXPECT_EQ(referenced(valueMember).name, "value");
    EXPECT_EQ(resolution.model->deferredReference(valueMember), nullptr);
}

TEST_F(NameResolutionTest, UsesDistinctSymbolsForNestedShadowing) {
    resolve(R"JOYEER(func choose(value: Int): Int {
let copy = value
if true {
let copy = 2
return copy
}
return copy
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[0]);
    const auto outerBinding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            function->body->items[0]);
    const auto conditional = std::static_pointer_cast<joyeer::syntax::IfExprSyntax>(
            function->body->items[1]);
    const auto innerBinding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            conditional->thenBranch->items[0]);
    const auto innerReturn = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            conditional->thenBranch->items[1]);
    const auto outerReturn = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[2]);

    const auto outerSymbol = resolution.model->declaredSymbol(outerBinding);
    const auto innerSymbol = resolution.model->declaredSymbol(innerBinding);
    ASSERT_TRUE(outerSymbol.has_value());
    ASSERT_TRUE(innerSymbol.has_value());
    EXPECT_NE(*outerSymbol, *innerSymbol);
    EXPECT_EQ(resolution.model->referencedSymbol(innerReturn->value), innerSymbol);
    EXPECT_EQ(resolution.model->referencedSymbol(outerReturn->value), outerSymbol);
}

TEST_F(NameResolutionTest, DiagnosesDuplicateUndefinedNameAndUndefinedType) {
    resolve(R"JOYEER(func broken(value: Missing): Int {
let duplicate = 1
let duplicate = 2
return absent
}
)JOYEER");

    EXPECT_TRUE(hasDiagnostic(NameResolutionDiagnosticId::duplicateDeclaration));
    EXPECT_TRUE(hasDiagnostic(NameResolutionDiagnosticId::undefinedName));
    EXPECT_TRUE(hasDiagnostic(NameResolutionDiagnosticId::undefinedType));
}

TEST_F(NameResolutionTest, ResolvesMembersEnumCasesAndSynthesizedInitializers) {
    resolve(R"JOYEER(enum Value { Empty, Number(Int), }
struct Box {
var value: Value
var count: Int = 0
}
func make(box: Box): Value {
let copied = box.value
let created = Box(value: copied)
return Value.Number(1)
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[2]);
    const auto copied = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            function->body->items[0]);
    const auto field = std::static_pointer_cast<joyeer::syntax::MemberExprSyntax>(
            copied->initializer);
    EXPECT_EQ(referenced(field).kind, SymbolKind::structureField);
    EXPECT_EQ(referenced(field).name, "value");

    const auto created = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(
            function->body->items[1]);
    const auto initializerCall = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            created->initializer);
    const auto initializerTarget = resolution.model->callTarget(initializerCall);
    ASSERT_TRUE(initializerTarget.has_value());
    EXPECT_EQ(resolution.model->symbol(*initializerTarget)->kind,
              SymbolKind::synthesizedInitializer);

    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[2]);
    const auto caseCall = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(
            returned->value);
    const auto caseTarget = resolution.model->callTarget(caseCall);
    ASSERT_TRUE(caseTarget.has_value());
    EXPECT_EQ(resolution.model->symbol(*caseTarget)->kind, SymbolKind::enumCase);
    EXPECT_EQ(resolution.model->symbol(*caseTarget)->name, "Number");
}

TEST_F(NameResolutionTest, ValidatesOrdinaryCallLabelsAndOrder) {
    resolve(R"JOYEER(func target(first: Int, second: Int): Int {
return first
}
func caller(): Int {
return target(second: 2, first: 1)
}
)JOYEER");

    EXPECT_TRUE(hasDiagnostic(NameResolutionDiagnosticId::argumentOutOfOrder));
}

TEST_F(NameResolutionTest, BindsQualifiedEnumPatternsInsideEachMatchArm) {
    resolve(R"JOYEER(enum Value { Empty, Number(Int), }
func read(value: Value): Int {
return match value {
Value.Number(number) => number,
Value.Empty => 0,
}
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[1]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    const auto match = std::static_pointer_cast<joyeer::syntax::MatchExprSyntax>(
            returned->value);
    const auto casePattern = std::static_pointer_cast<joyeer::syntax::EnumCasePatternSyntax>(
            match->arms[0]->pattern);
    EXPECT_EQ(referenced(casePattern).kind, SymbolKind::enumCase);
    EXPECT_EQ(referenced(casePattern).name, "Number");

    const auto payloadBinding = std::static_pointer_cast<joyeer::syntax::BindingPatternSyntax>(
            casePattern->arguments[0]->pattern);
    const auto payloadSymbol = resolution.model->declaredSymbol(payloadBinding);
    ASSERT_TRUE(payloadSymbol.has_value());
    EXPECT_EQ(resolution.model->referencedSymbol(match->arms[0]->body), payloadSymbol);
    EXPECT_NE(resolution.model->introducedScope(match->arms[0]),
              resolution.model->introducedScope(match->arms[1]));
}

TEST_F(NameResolutionTest, RecordsContextualCasesForTypeDirectedResolution) {
    resolve(R"JOYEER(enum Value { Number(Int), }
func wrap(value: Int): Value {
return .Number(value)
}
)JOYEER");

    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[1]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    const auto deferred = resolution.model->deferredReference(returned->value);
    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->kind, DeferredResolutionKind::contextualEnumCaseNeedsType);
    EXPECT_EQ(deferred->name, "Number");
}

TEST_F(NameResolutionTest, DefersMemberLookupWhenLocalTypeNeedsInference) {
        resolve(R"JOYEER(struct Box { var value: Int }
func read(box: Box): Int {
let inferred = box
return inferred.value
}
)JOYEER");

        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
                        parseResult.root->items[1]);
        const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
                        function->body->items[1]);
        const auto deferred = resolution.model->deferredReference(returned->value);
        ASSERT_NE(deferred, nullptr);
        EXPECT_EQ(deferred->kind, DeferredResolutionKind::memberNeedsBaseType);
        EXPECT_EQ(deferred->name, "value");
}

TEST_F(NameResolutionTest, SemanticIdsAndDumpAreDeterministic) {
        resolve(R"JOYEER(struct Value { var number: Int }
func read(value: Value): Int {
return value.number
}
)JOYEER");

        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        const auto second = joyeer::semantic::NameResolver().resolve(parseResult.root);
        ASSERT_TRUE(second.succeeded());
        EXPECT_EQ(joyeer::semantic::dump(*resolution.model),
                          joyeer::semantic::dump(*second.model));
}

        TEST_F(NameResolutionTest, SkipsParserRecoveryNodesWithoutCascading) {
            const auto functionName = std::make_shared<Token>(
                    identifier,
                    "recovered",
                    SourceSpan { 5, 9 },
                    0,
                    5,
                    false);
            const auto error = std::make_shared<joyeer::syntax::ErrorExprSyntax>(
                    SourceSpan { 20, 1 });
            const auto body = std::make_shared<joyeer::syntax::BlockExprSyntax>(
                    SourceSpan { 18, 4 },
                    std::vector<joyeer::syntax::NodePtr> { error });
            const auto function = std::make_shared<joyeer::syntax::FunctionDeclSyntax>(
                    SourceSpan { 0, 22 },
                    functionName,
                    std::vector<joyeer::syntax::ParameterDeclSyntax::Ptr> {},
                    nullptr,
                    body);
            const auto root = std::make_shared<joyeer::syntax::SourceFileSyntax>(
                    SourceSpan { 0, 22 },
                    std::vector<joyeer::syntax::NodePtr> {
                        std::make_shared<joyeer::syntax::ErrorDeclSyntax>(SourceSpan { 0, 1 }),
                        function,
                    });

            const auto recovered = joyeer::semantic::NameResolver().resolve(root);
            EXPECT_TRUE(recovered.succeeded()) << joyeer::semantic::dump(recovered.diagnostics);
            ASSERT_NE(recovered.model, nullptr);
            EXPECT_TRUE(recovered.model->declaredSymbol(function).has_value());
            EXPECT_TRUE(recovered.model->containingScope(error).has_value());
        }

TEST_F(NameResolutionTest, DiagnosesUnknownMemberWhenBaseTypeIsKnown) {
    resolve(R"JOYEER(struct Box { var value: Int }
func read(box: Box): Int {
return box.missing
}
)JOYEER");

    EXPECT_TRUE(hasDiagnostic(NameResolutionDiagnosticId::unknownMember));
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            parseResult.root->items[1]);
    const auto returned = std::static_pointer_cast<joyeer::syntax::ReturnExprSyntax>(
            function->body->items[0]);
    EXPECT_EQ(resolution.model->deferredReference(returned->value), nullptr);
}

TEST_F(NameResolutionTest, ResolvesAllLexicalNamesInJsonMvpFixture) {
    resolve(readFixture("parser/ok/json_mvp.joyeer"));

    EXPECT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    EXPECT_FALSE(resolution.model->deferredReferences().empty());
    EXPECT_TRUE(std::all_of(
            resolution.model->deferredReferences().begin(),
            resolution.model->deferredReferences().end(),
            [](const auto& deferred) {
                return deferred.kind == DeferredResolutionKind::contextualEnumCaseNeedsType ||
                       deferred.kind == DeferredResolutionKind::contextualEnumPatternNeedsType;
            }));
}

} // namespace