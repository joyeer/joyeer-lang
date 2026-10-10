#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/semanticanalysis.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/typechecking.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using joyeer::semantic::ModuleInput;
using joyeer::semantic::NameResolutionDiagnosticId;
using joyeer::syntax::SourceFileSyntax;

SourceFileSyntax::Ptr parseFile(const std::string& text, uint32_t sourceId) {
    Diagnostics diagnostics;
    auto source = std::make_shared<SourceFile>(text);
    LexParser(&diagnostics).parse(source);
    EXPECT_FALSE(diagnostics.hasFailure());
    for (const auto& token : source->tokens) token->span.sourceId = sourceId;
    auto parsed = joyeer::parser::Parser(source->tokens).parse();
    EXPECT_TRUE(parsed.succeeded()) << joyeer::parser::dump(parsed.diagnostics);
    return parsed.root;
}

bool hasDiagnostic(
        const joyeer::semantic::NameResolutionResult& result,
        NameResolutionDiagnosticId id) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [id](const auto& diagnostic) { return diagnostic.id == id; });
}

auto resolve(const std::vector<ModuleInput>& modules) {
    return joyeer::semantic::NameResolver().resolve(modules);
}

TEST(CompilationUnitModules, ResolvesTwoRootFilesAndQualifiedPublicTypesAndFunctions) {
    auto main = parseFile(
            "import project.config\n"
            "func main() {\n"
            "let item: project.config.Record = project.config.Record(value: helper())\n"
            "print(value: project.config.read(item: item))\n"
            "}\n", 0);
    auto helper = parseFile("func helper(): Int { 42 }\n", 1);
    auto dependency = parseFile(
            "public struct Record {\npublic let value: Int\n}\n"
            "public func read(item: Record): Int { item.value }\n"
            "func main(value: Int): Int { value }\n", 2);
    const auto result = resolve({{"", {main, helper}}, {"project.config", {dependency}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    ASSERT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
    const auto analysis = joyeer::analysis::Analyzer().analyze(checked.model);
    EXPECT_TRUE(analysis.succeeded()) << joyeer::analysis::dump(analysis.diagnostics);
    const auto symbol = result.model->declaredSymbol(dependency->items.back());
    ASSERT_TRUE(symbol.has_value());
    EXPECT_FALSE(result.model->symbol(*symbol)->isRootModule);
    EXPECT_EQ(result.model->symbol(*symbol)->span.sourceId, 2u);
}

TEST(CompilationUnitModules, FilePrivateShadowsOnlyWithinItsOwnFile) {
    auto first = parseFile("private func value(): Int { 1 }\nfunc first(): Int { value() }\n", 0);
    auto second = parseFile("func value(): Int { 2 }\nfunc second(): Int { value() }\n", 1);
    const auto result = resolve({{"", {first, second}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto firstFunction = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            first->items.back());
    const auto secondFunction = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(
            second->items.back());
    EXPECT_EQ(result.model->callTarget(firstFunction->body->items.front()),
              result.model->declaredSymbol(first->items.front()));
    EXPECT_EQ(result.model->callTarget(secondFunction->body->items.front()),
              result.model->declaredSymbol(second->items.front()));
}

TEST(CompilationUnitModules, RejectsDuplicateNonPrivateDeclarationsAcrossFilesAndNamespaces) {
    const auto result = resolve({{"", {
            parseFile("func duplicate() {}\n", 0),
            parseFile("struct duplicate {}\n", 1)}}});
    EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::duplicateDeclaration));
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().span.sourceId, 1u);
}

TEST(CompilationUnitModules, RejectsPrivateAndNonPrivateDuplicateInSameFile) {
    const auto result = resolve({{"", {parseFile(
            "private func value() {}\nfunc value() {}\n", 0)}}});
    EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::duplicateDeclaration));
}

TEST(CompilationUnitModules, ImportsAreFileLocalAndNeverInjectUnqualifiedNames) {
    const auto dependency = parseFile("public func value(): Int { 1 }\n", 2);
    for (const auto& text : {
            "func main(): Int { dep.value() }\n",
            "import dep\nfunc main(): Int { value() }\n"}) {
        const auto result = resolve({
                {"", {parseFile(text, 0), parseFile("import dep\nfunc helper() {}\n", 1)}},
                {"dep", {dependency}}});
        EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::undefinedName));
    }
}

TEST(CompilationUnitModules, RejectsImportDeclarationCollisions) {
    const auto result = resolve({
            {"", {parseFile("import project.config\nfunc project() {}\n", 0)}},
            {"project.config", {parseFile("public func value() {}\n", 1)}}});
    EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::importCollision));
}

TEST(CompilationUnitModules, RejectsUnknownImportAndSingleFileImports) {
    auto file = parseFile("import missing\nfunc main() {}\n", 0);
    EXPECT_TRUE(hasDiagnostic(resolve({{"", {file}}}), NameResolutionDiagnosticId::unknownImport));
    EXPECT_TRUE(hasDiagnostic(joyeer::semantic::NameResolver().resolve(file),
                              NameResolutionDiagnosticId::unknownImport));
}

TEST(CompilationUnitModules, RejectsInternalImportedFunction) {
    const auto result = resolve({
            {"", {parseFile("import dep\nfunc main() { dep.hidden() }\n", 0)}},
            {"dep", {parseFile("func hidden() {}\n", 1)}}});
    EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::inaccessibleDeclaration));
}

TEST(CompilationUnitModules, RejectsPrivateFunctionInAnotherFile) {
    const auto result = resolve({{"", {
            parseFile("func main() { hidden() }\n", 0),
            parseFile("private func hidden() {}\n", 1)}}});
    EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::undefinedName));
}

TEST(CompilationUnitModules, RejectsPrivateMemberInAnotherFileAndInternalImportedMember) {
    for (const bool imported : {false, true}) {
        const auto consumer = parseFile(imported
                ? "import dep\nfunc use(item: dep.Record): Int { item.hidden }\n"
                : "func use(item: Record): Int { item.hidden }\n", 0);
        const auto declaration = parseFile(imported
                ? "public struct Record {\nlet hidden: Int\n}\n"
                : "struct Record {\nprivate let hidden: Int\n}\n", 1);
        const auto result = imported
                ? resolve({{"", {consumer}}, {"dep", {declaration}}})
                : resolve({{"", {consumer, declaration}}});
        EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::inaccessibleDeclaration));
    }
}

TEST(CompilationUnitModules, InferredMemberLookupCannotBypassVisibility) {
    const auto result = resolve({
            {"", {parseFile("import dep\nfunc main() {\nlet item = dep.make()\n"
                           "print(value: item.hidden)\n}\n", 0)}},
            {"dep", {parseFile("public struct Record {\nlet hidden: Int\n}\n"
                              "public func make(): Record { Record(hidden: 1) }\n", 1)}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    EXPECT_FALSE(checked.succeeded());
    EXPECT_TRUE(std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id ==
                        joyeer::typing::TypeCheckingDiagnosticId::inaccessibleDeclaration;
            }));
}

TEST(CompilationUnitModules, SynthesizedInitializerIsAsRestrictedAsItsLeastVisibleField) {
    const auto result = resolve({
            {"", {parseFile("import dep\nfunc main() { dep.Record(hidden: 1) }\n", 0)}},
            {"dep", {parseFile("public struct Record {\nprivate let hidden: Int\n}\n", 1)}}});
    EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::inaccessibleDeclaration));
}

TEST(CompilationUnitModules, RejectsPublicSignaturesFieldsAndEnumPayloadsExposingHiddenTypes) {
    for (const auto& text : {
            "struct Hidden {}\npublic func expose(value: [Hidden]) {}\n",
            "struct Hidden {}\npublic struct Export {\npublic let value: Hidden?\n}\n",
            "struct Hidden {}\npublic enum Export { Value(Hidden) }\n"}) {
        const auto result = resolve({{"", {parseFile(text, 0)}}});
        EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::inaccessibleExposedType));
    }
}

TEST(CompilationUnitModules, ImportedEnumCasesAndQualifiedPatternsUseTypeIdentity) {
    const auto result = resolve({
            {"", {parseFile("import dep\nfunc use(value: dep.Choice): Int {\n"
                           "match value {\ndep.Choice.First => 1\ndep.Choice.Second => 2\n}\n"
                           "}\nfunc main() { print(value: use(value: dep.Choice.First)) }\n", 0)}},
            {"dep", {parseFile("public enum Choice { First, Second }\n", 1)}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    EXPECT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
}

TEST(CompilationUnitModules, ParserPreservesPerFileSpansAndRejectsLateImports) {
    auto source = std::make_shared<SourceFile>("func main() {}\nimport dep\n");
    Diagnostics diagnostics;
    LexParser(&diagnostics).parse(source);
    for (const auto& token : source->tokens) token->span.sourceId = 7;
    const auto parsed = joyeer::parser::Parser(source->tokens).parse();
    EXPECT_FALSE(parsed.succeeded());
    EXPECT_EQ(parsed.root->span.sourceId, 7u);
    EXPECT_EQ(parsed.root->items.front()->span.sourceId, 7u);
    ASSERT_FALSE(parsed.diagnostics.empty());
    EXPECT_EQ(parsed.diagnostics.front().span.sourceId, 7u);
    EXPECT_EQ(parsed.diagnostics.front().span.offset, 15u);
}

} // namespace
