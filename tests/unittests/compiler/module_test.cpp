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

TEST(CompilationUnitModules, ResolvesUnqualifiedPublicTypesConstructorsFunctionsAndEnumCases) {
    const auto main = parseFile(
            "import project.config\n"
            "public func wrap(items: [Record], lookup: [String: Record], maybe: Record?): Result<Record, Choice> {\n"
            "return .Ok(make())\n}\n"
            "func main() {\n"
            "let item: Record = Record(value: read(item: make()))\n"
            "let choice: Choice = Choice.First\n"
            "match choice {\nChoice.First => print(value: item.value)\nChoice.Second => print(value: 0)\n}\n"
            "let outcome = wrap(items: [item], lookup: [\"item\": item], maybe: .Some(item))\n"
            "match outcome {\n.Ok(saved) => print(value: saved.value)\n.Err(error) => print(value: 0)\n}\n"
            "}\n", 0);
    const auto dependency = parseFile(
            "public struct Record {\npublic let value: Int\n}\n"
            "public enum Choice { First, Second }\n"
            "public func read(item: Record): Int { return item.value }\n"
            "public func make(): Record { return Record(value: 42) }\n", 1);
    const auto result = resolve({{"project.app", {main}}, {"project.config", {dependency}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    ASSERT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(main->items.back());
    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(function->body->items.front());
    EXPECT_EQ(result.model->referencedSymbol(binding->annotation),
              result.model->declaredSymbol(dependency->items.front()));
}

TEST(CompilationUnitModules, ImportsAreFileLocalForQualifiedAndUnqualifiedNames) {
    const auto dependency = parseFile("public func value(): Int { 1 }\n", 2);
    for (const auto& text : {
            "func main(): Int { dep.value() }\n",
            "func main(): Int { value() }\n"}) {
        const auto result = resolve({
                {"", {parseFile(text, 0), parseFile("import dep\nfunc helper() {}\n", 1)}},
                {"dep", {dependency}}});
        EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::undefinedName));
    }
}

TEST(CompilationUnitModules, UnqualifiedTypeImportsAreFileLocal) {
    const auto result = resolve({
            {"project.app", {
                    parseFile("func use(item: Record) {}\n", 0),
                    parseFile("import dep\nfunc helper() {}\n", 1)}},
            {"dep", {parseFile("public struct Record {}\n", 2)}}});
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().id, NameResolutionDiagnosticId::undefinedType);
    EXPECT_EQ(result.diagnostics.front().span.sourceId, 0u);
}

TEST(CompilationUnitModules, LocalAndModuleDeclarationsShadowImportedNames) {
    const auto main = parseFile(
            "import alpha\nimport beta\n"
            "private func value(): Int { return 1 }\n"
            "func main() {\n"
            "let item: Record = Record(value: value())\n"
            "let value = 3\nprint(value: item.value + value)\n}\n", 0);
    const auto helper = parseFile(
            "struct Record {\nlet value: Int\n}\nfunc value(): Int { return 2 }\n", 1);
    const auto exports =
            "public struct Record {}\npublic func value(): Int { return 42 }\n";
    const auto result = resolve({
            {"project.app", {main, helper}},
            {"alpha", {parseFile(exports, 2)}},
            {"beta", {parseFile(exports, 3)}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    ASSERT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
    const auto function = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(main->items.back());
    const auto binding = std::static_pointer_cast<joyeer::syntax::BindingDeclSyntax>(function->body->items.front());
    EXPECT_EQ(result.model->referencedSymbol(binding->annotation),
              result.model->declaredSymbol(helper->items.front()));
    const auto constructor = std::static_pointer_cast<joyeer::syntax::CallExprSyntax>(binding->initializer);
    EXPECT_EQ(result.model->callTarget(constructor->arguments.front()->value),
              result.model->declaredSymbol(main->items.front()));
}

TEST(CompilationUnitModules, PreludeNamesRemainPreferredOverImports) {
    const auto result = resolve({
            {"project.app", {parseFile(
                    "import alpha\nimport beta\n"
                    "func main() {\nlet value: Int = 42\nprint(value: value)\n}\n", 0)}},
            {"alpha", {parseFile("public struct Int {}\n", 1)}},
            {"beta", {parseFile("public func print(value: Bool) {}\n", 2)}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    EXPECT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
}

TEST(CompilationUnitModules, ReportsImportedAmbiguityAtEachUseIndependentOfImportAndModuleOrder) {
    struct Use {
        const char* text;
        const char* name;
        bool typeOnly;
    };
    for (const auto& use : {
            Use {"func use(item: Record) {}\n", "Record", true},
            Use {"func main() { Record() }\n", "Record", false},
            Use {"func main() { value() }\n", "value", false},
            Use {"func main() { Choice.First }\n", "Choice", false},
            Use {"func use(item: alpha.Choice): Int {\nmatch item {\nChoice.First => 1\n_ => 0\n}\n}\n",
                 "Choice", true}}) {
        for (const bool reversed : {false, true}) {
            const auto text = std::string(reversed
                    ? "import beta\nimport alpha\n"
                    : "import alpha\nimport beta\n") + use.text;
            const auto exports =
                    "public struct Record {}\npublic enum Choice { First }\n"
                    "public func value(): Int { return 1 }\n";
            std::vector<ModuleInput> modules {
                    {"project.app", {parseFile(text, 7)}},
                    {"alpha", {parseFile(exports, 1)}},
                    {"beta", {parseFile(exports, 2)}}};
            if (reversed) std::swap(modules[1], modules[2]);
            const auto result = resolve(modules);
            ASSERT_EQ(result.diagnostics.size(), 1u) << joyeer::semantic::dump(result.diagnostics);
            const auto& diagnostic = result.diagnostics.front();
            EXPECT_EQ(diagnostic.id, NameResolutionDiagnosticId::ambiguousImport);
            EXPECT_EQ(diagnostic.span.sourceId, 7u);
            EXPECT_EQ(diagnostic.span.offset, text.rfind(use.name));
            EXPECT_EQ(diagnostic.span.length, std::string(use.name).size());
            EXPECT_EQ(diagnostic.message,
                    "ambiguous imported " + std::string(use.typeOnly ? "type" : "name") +
                    " '" + use.name + "'; use a module-qualified name: 'alpha." +
                    use.name + "', 'beta." + use.name + "'");
        }
    }
    EXPECT_STREQ(joyeer::semantic::diagnosticName(NameResolutionDiagnosticId::ambiguousImport),
                 "name-resolution.ambiguous-import");
}

TEST(CompilationUnitModules, UnusedAmbiguitiesAndRepeatedImportsAreAllowed) {
    for (const auto& imports : {
            "import alpha\nimport beta\n",
            "import alpha\nimport alpha\n"}) {
        const auto text = std::string(imports) +
                "func main() { print(value: alpha.value()) }\n";
        const auto result = resolve({
                {"project.app", {parseFile(text, 0)}},
                {"alpha", {parseFile("public func value(): Int { return 1 }\n", 1)}},
                {"beta", {parseFile("public func value(): Int { return 2 }\n", 2)}}});
        ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
        const auto checked = joyeer::typing::TypeChecker().check(result.model);
        EXPECT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
    }
    const auto repeated = resolve({
            {"project.app", {parseFile("import alpha\nimport alpha\nfunc main() { value() }\n", 0)}},
            {"alpha", {parseFile("public func value() {}\n", 1)}}});
    EXPECT_TRUE(repeated.succeeded()) << joyeer::semantic::dump(repeated.diagnostics);
}

TEST(CompilationUnitModules, QualifiedNamesDisambiguateImportedTypesFunctionsAndEnumCases) {
    const auto exports =
            "public struct Record {\npublic let value: Int\n}\n"
            "public enum Choice { First }\npublic func value(): Int { return 1 }\n";
    const auto result = resolve({
            {"project.app", {parseFile(
                    "import alpha\nimport beta\n"
                    "func main() {\n"
                    "let first: alpha.Record = alpha.Record(value: alpha.value())\n"
                    "let second: beta.Record = beta.Record(value: beta.value())\n"
                    "let choice: alpha.Choice = alpha.Choice.First\n"
                    "match choice {\nalpha.Choice.First => print(value: first.value + second.value)\n}\n"
                    "}\n", 0)}},
            {"alpha", {parseFile(exports, 1)}},
            {"beta", {parseFile(exports, 2)}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    EXPECT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
}

TEST(CompilationUnitModules, TypeLookupIgnoresImportedValuesButConstructorLookupIsAmbiguous) {
    const auto function = parseFile("public func Token() {}\n", 1);
    const auto type = parseFile("public struct Token {}\n", 2);
    const auto typed = resolve({
            {"project.app", {parseFile("import alpha\nimport beta\nfunc use(item: Token) {}\n", 0)}},
            {"alpha", {function}},
            {"beta", {type}}});
    ASSERT_TRUE(typed.succeeded()) << joyeer::semantic::dump(typed.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(typed.model);
    EXPECT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
    const auto constructed = resolve({
            {"project.app", {parseFile("import alpha\nimport beta\nfunc main() { Token() }\n", 0)}},
            {"alpha", {function}},
            {"beta", {type}}});
    ASSERT_EQ(constructed.diagnostics.size(), 1u);
    EXPECT_EQ(constructed.diagnostics.front().id, NameResolutionDiagnosticId::ambiguousImport);
}

TEST(CompilationUnitModules, UnqualifiedImportsExcludeInternalAndPrivateDeclarations) {
    for (const auto& visibility : {"", "private "}) {
        const auto result = resolve({
                {"project.app", {parseFile(
                        "import dep\nfunc use(item: Record) { hidden() }\n", 0)}},
                {"dep", {parseFile(std::string(visibility) + "struct Record {}\n" +
                                  visibility + "func hidden() {}\n", 1)}}});
        ASSERT_EQ(result.diagnostics.size(), 2u) << joyeer::semantic::dump(result.diagnostics);
        EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::undefinedType));
        EXPECT_TRUE(hasDiagnostic(result, NameResolutionDiagnosticId::undefinedName));
    }
}

TEST(CompilationUnitModules, NonPublicImportsDoNotMakePublicNamesAmbiguous) {
    const auto result = resolve({
            {"project.app", {parseFile(
                    "import alpha\nimport beta\n"
                    "func main() {\nlet item: Record = Record(value: value())\nprint(value: item.value)\n}\n", 0)}},
            {"alpha", {parseFile(
                    "public struct Record {\npublic let value: Int\n}\n"
                    "public func value(): Int { return 42 }\n", 1)}},
            {"beta", {parseFile("struct Record {}\nprivate func value(): Int { return 0 }\n", 2)}}});
    ASSERT_TRUE(result.succeeded()) << joyeer::semantic::dump(result.diagnostics);
    const auto checked = joyeer::typing::TypeChecker().check(result.model);
    EXPECT_TRUE(checked.succeeded()) << joyeer::typing::dump(checked.diagnostics);
}

TEST(CompilationUnitModules, ImportsDoNotReexportDependencyNames) {
    const auto result = resolve({
            {"project.app", {parseFile("import facade\nfunc main() { value() }\n", 0)}},
            {"facade", {parseFile(
                    "import dep\npublic func wrapped(): Int { return value() }\n", 1)}},
            {"dep", {parseFile("public func value(): Int { return 42 }\n", 2)}}});
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().id, NameResolutionDiagnosticId::undefinedName);
    EXPECT_EQ(result.diagnostics.front().span.sourceId, 0u);
}

TEST(CompilationUnitModules, UnqualifiedImportedConstructorsPreserveFieldVisibility) {
    const auto result = resolve({
            {"project.app", {parseFile("import dep\nfunc main() { Record(value: 1) }\n", 0)}},
            {"dep", {parseFile("public struct Record {\nlet value: Int\n}\n", 1)}}});
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().id, NameResolutionDiagnosticId::inaccessibleDeclaration);
}

TEST(CompilationUnitModules, ShortNameExplicitInferredAndContainerMemberAccessPreservesVisibility) {
    for (const auto& text : {
            "import dep\nfunc use(item: Record): Int { return item.hidden }\n",
            "import dep\nfunc main() { let item = make()\nprint(value: item.hidden) }\n",
            "import dep\nfunc main() { let items = [make()]\nprint(value: items[0].hidden) }\n"}) {
        const auto result = resolve({
                {"project.app", {parseFile(text, 0)}},
                {"dep", {parseFile(
                        "public struct Record {\nlet hidden: Int\n}\n"
                        "public func make(): Record { return Record(hidden: 1) }\n", 1)}}});
        if (!result.succeeded()) {
            ASSERT_EQ(result.diagnostics.size(), 1u) << joyeer::semantic::dump(result.diagnostics);
            EXPECT_EQ(result.diagnostics.front().id, NameResolutionDiagnosticId::inaccessibleDeclaration);
        } else {
            const auto checked = joyeer::typing::TypeChecker().check(result.model);
            ASSERT_EQ(checked.diagnostics.size(), 1u) << joyeer::typing::dump(checked.diagnostics);
            EXPECT_EQ(checked.diagnostics.front().id,
                      joyeer::typing::TypeCheckingDiagnosticId::inaccessibleDeclaration);
        }
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
