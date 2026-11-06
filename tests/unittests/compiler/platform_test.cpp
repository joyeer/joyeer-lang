#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/platform.h"
#include "compiler/frontend/typechecking.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using joyeer::semantic::ModuleInput;
using joyeer::semantic::PlatformDiagnosticId;
using joyeer::semantic::TargetPlatform;

class PlatformTest : public testing::Test {
protected:
    joyeer::syntax::SourceFileSyntax::Ptr parse(const std::string& text, uint32_t sourceId = 0) {
        auto source = std::make_shared<SourceFile>(text);
        sources.push_back(source);
        Diagnostics diagnostics;
        LexParser(&diagnostics).parse(source);
        EXPECT_FALSE(diagnostics.hasFailure());
        for (auto& token : source->tokens) token->span.sourceId = sourceId;
        auto result = joyeer::parser::Parser(source->tokens).parse();
        EXPECT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
        return result.root;
    }

    ModuleInput module(const std::string& text, bool runtimeContext = false) {
        return ModuleInput {"fixture", {parse(text)}, runtimeContext};
    }

    std::vector<joyeer::semantic::PlatformDiagnostic> select(
            const std::vector<ModuleInput>& modules, TargetPlatform target) {
        return joyeer::semantic::selectPlatformDeclarations(modules, target);
    }

    // Names of the functions that remain in the first file of a module.
    static std::vector<joyeer::syntax::FunctionDeclSyntax::Ptr> functions(const ModuleInput& input) {
        std::vector<joyeer::syntax::FunctionDeclSyntax::Ptr> result;
        for (const auto& item : input.files.front()->items) {
            if (item->kind == joyeer::syntax::Kind::functionDecl) {
                result.push_back(std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(item));
            }
        }
        return result;
    }

    static bool has(const std::vector<joyeer::semantic::PlatformDiagnostic>& diagnostics,
                    PlatformDiagnosticId id) {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

    // The internal name of the first parameter tells the variants apart.
    static std::string chosen(const ModuleInput& input) {
        const auto list = functions(input);
        EXPECT_EQ(list.size(), 1u);
        if (list.size() != 1 || list.front()->parameters.empty()) return "";
        return list.front()->parameters.front()->name->rawValue;
    }

    std::vector<SourceFile::Ptr> sources;
};

constexpr auto splitSource = R"(
@platform(windows)
func separator(value win: Int): Int { return 1 }

@platform(posix)
func separator(value unix: Int): Int { return 2 }
)";

TEST_F(PlatformTest, SelectsTheVariantForEachTarget) {
    struct Case { TargetPlatform target; const char* expected; };
    for (const auto& item : {
            Case {TargetPlatform::windows, "win"},
            Case {TargetPlatform::linux, "unix"},
            Case {TargetPlatform::macos, "unix"} }) {
        const std::vector<ModuleInput> modules { module(splitSource) };
        EXPECT_TRUE(select(modules, item.target).empty());
        EXPECT_EQ(chosen(modules.front()), item.expected) << joyeer::semantic::platformName(item.target);
    }
}

TEST_F(PlatformTest, StripsTheAnnotationFromTheSelectedFunction) {
    const std::vector<ModuleInput> modules { module(splitSource) };
    ASSERT_TRUE(select(modules, TargetPlatform::linux).empty());
    EXPECT_TRUE(functions(modules.front()).front()->attributes.empty());
}

TEST_F(PlatformTest, AcceptsAListOfPlatforms) {
    const std::vector<ModuleInput> modules { module(R"(
@platform(linux, macos)
func f(value x: Int): Int { return 1 }
)") };
    ASSERT_TRUE(select(modules, TargetPlatform::macos).empty());
    EXPECT_EQ(functions(modules.front()).size(), 1u);
    const std::vector<ModuleInput> other { module(R"(
@platform(linux, macos)
func f(value x: Int): Int { return 1 }
)") };
    ASSERT_TRUE(select(other, TargetPlatform::windows).empty());
    EXPECT_TRUE(functions(other.front()).empty());
}

TEST_F(PlatformTest, LeavesOtherDeclarationsAlone) {
    const std::vector<ModuleInput> modules { module(R"(
struct Box { var value: Int }
func plain(): Int { return 1 }
@platform(windows)
func only(): Int { return 2 }
)") };
    ASSERT_TRUE(select(modules, TargetPlatform::linux).empty());
    EXPECT_EQ(modules.front().files.front()->items.size(), 2u);
}

TEST_F(PlatformTest, RejectsOverlappingPlatformSets) {
    for (const char* text : {
            "@platform(posix)\nfunc f(): Int { return 1 }\n@platform(linux)\nfunc f(): Int { return 2 }\n",
            "@platform(windows)\nfunc f(): Int { return 1 }\nfunc f(): Int { return 2 }\n",
            "func f(): Int { return 2 }\n@platform(macos)\nfunc f(): Int { return 1 }\n" }) {
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(has(select(modules, TargetPlatform::windows), PlatformDiagnosticId::overlappingVariants)) << text;
    }
}

TEST_F(PlatformTest, ChecksVariantsWhateverTheTarget) {
    for (const auto target : {TargetPlatform::windows, TargetPlatform::linux, TargetPlatform::macos}) {
        const std::vector<ModuleInput> modules { module(
                "@platform(posix)\nfunc f(): Int { return 1 }\n@platform(linux)\nfunc f(): Int { return 2 }\n") };
        EXPECT_TRUE(has(select(modules, target), PlatformDiagnosticId::overlappingVariants));
    }
}

TEST_F(PlatformTest, RejectsVariantsWithDifferentSignatures) {
    for (const char* second : {
            "func f(value x: Int): String { return \"\" }",
            "func f(other x: Int): Int { return 1 }",
            "func f(value x: Bool): Int { return 1 }",
            "func f(value x: Int, value y: Int): Int { return 1 }",
            "func f(value x: inout Int): Int { return 1 }",
            "func f(value x: Int): Int? { return 1 }",
            "func f(value x: [Int]): Int { return 1 }",
            "public func f(value x: Int): Int { return 1 }" }) {
        const std::string text = std::string("@platform(windows)\nfunc f(value x: Int): Int { return 1 }\n") +
                "@platform(posix)\n" + second + "\n";
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(has(select(modules, TargetPlatform::linux), PlatformDiagnosticId::signatureMismatch)) << text;
    }
}

TEST_F(PlatformTest, AllowsDifferentInternalNamesAndBodies) {
    const std::vector<ModuleInput> modules { module(splitSource) };
    EXPECT_TRUE(select(modules, TargetPlatform::windows).empty());
}

TEST_F(PlatformTest, RejectsMalformedAnnotations) {
    for (const char* text : {
            "@platform()\nfunc f(): Int { return 1 }\n",
            "@platform(os: windows)\nfunc f(): Int { return 1 }\n",
            "@platform(\"windows\")\nfunc f(): Int { return 1 }\n",
            "@platform(windows())\nfunc f(): Int { return 1 }\n",
            "@platform(windows)\n@platform(linux)\nfunc f(): Int { return 1 }\n" }) {
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(has(select(modules, TargetPlatform::windows), PlatformDiagnosticId::invalidAnnotation)) << text;
    }
}

TEST_F(PlatformTest, RejectsUnknownPlatformNames) {
    for (const char* name : {"freebsd", "Windows", "unix", "arm64"}) {
        const std::vector<ModuleInput> modules {
            module(std::string("@platform(") + name + ")\nfunc f(): Int { return 1 }\n") };
        EXPECT_TRUE(has(select(modules, TargetPlatform::windows), PlatformDiagnosticId::unknownName)) << name;
    }
}

TEST_F(PlatformTest, ReportsErrorsInVariantsThatTheTargetDrops) {
    const std::vector<ModuleInput> modules { module(
            "@platform(windows)\nfunc f(): Int { return 1 }\n@platform(freebsd)\nfunc g(): Int { return 1 }\n") };
    EXPECT_TRUE(has(select(modules, TargetPlatform::linux), PlatformDiagnosticId::unknownName));
}

TEST_F(PlatformTest, ChecksVariantsAcrossFilesOfOneModule) {
    ModuleInput input {"fixture", {
        parse("@platform(posix)\nfunc f(): Int { return 1 }\n", 0),
        parse("@platform(macos)\nfunc f(): Int { return 2 }\n", 1) }, false};
    EXPECT_TRUE(has(select({input}, TargetPlatform::macos), PlatformDiagnosticId::overlappingVariants));
}

TEST_F(PlatformTest, SelectsAcrossFilesOfOneModule) {
    ModuleInput input {"fixture", {
        parse("@platform(windows)\nfunc f(value a: Int): Int { return 1 }\n", 0),
        parse("@platform(posix)\nfunc f(value b: Int): Int { return 2 }\n", 1) }, false};
    ASSERT_TRUE(select({input}, TargetPlatform::linux).empty());
    EXPECT_TRUE(input.files[0]->items.empty());
    EXPECT_EQ(input.files[1]->items.size(), 1u);
}

TEST_F(PlatformTest, ModulesDoNotShareVariantGroups) {
    const std::vector<ModuleInput> modules {
        ModuleInput {"one", {parse("@platform(posix)\nfunc f(): Int { return 1 }\n", 0)}, false},
        ModuleInput {"two", {parse("@platform(posix)\nfunc f(): Int { return 1 }\n", 1)}, false} };
    EXPECT_TRUE(select(modules, TargetPlatform::linux).empty());
}

TEST_F(PlatformTest, SelectedVariantResolvesAndChecksNormally) {
    for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
        const std::vector<ModuleInput> modules { module(std::string(splitSource) +
                "func main() { print(value: separator(value: 0)) }\n") };
        ASSERT_TRUE(select(modules, target).empty());
        const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(PlatformTest, DroppedVariantsAreNotResolvedOrTypeChecked) {
    const std::vector<ModuleInput> modules { module(R"(
@platform(windows)
func f(): Int { return noSuchFunction() }

@platform(posix)
func f(): Int { return 1 }

func main() { print(value: f()) }
)") };
    ASSERT_TRUE(select(modules, TargetPlatform::linux).empty());
    const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    EXPECT_TRUE(joyeer::typing::TypeChecker().check(resolution.model).succeeded());
}

TEST_F(PlatformTest, SelectedVariantIsStillChecked) {
    const std::vector<ModuleInput> modules { module(R"(
@platform(windows)
func f(): Int { return noSuchFunction() }
)") };
    ASSERT_TRUE(select(modules, TargetPlatform::windows).empty());
    EXPECT_FALSE(joyeer::semantic::NameResolver().resolve(modules).succeeded());
}

TEST_F(PlatformTest, ReferenceWithoutAVariantIsAnUndefinedName) {
    const std::vector<ModuleInput> modules { module(R"(
@platform(windows)
func f(): Int { return 1 }

func main() { print(value: f()) }
)") };
    ASSERT_TRUE(select(modules, TargetPlatform::linux).empty());
    const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
    ASSERT_FALSE(resolution.succeeded());
    EXPECT_EQ(resolution.diagnostics.front().id, joyeer::semantic::NameResolutionDiagnosticId::undefinedName);
}

TEST_F(PlatformTest, CombinesWithCBindings) {
    constexpr auto text = R"(
@platform(windows)
@c_extern(symbol: "native_win")
func nativeCall(value: borrowing Int): Int

@platform(posix)
@c_extern(symbol: "native_posix")
func nativeCall(value: borrowing Int): Int

@c_export(symbol: "runtime_call")
func runtimeCall(input value: Int): Int { return nativeCall(value: value) }
)";
    const std::vector<ModuleInput> modules { module(text, true) };
    ASSERT_TRUE(select(modules, TargetPlatform::linux).empty());
    const auto list = functions(modules.front());
    ASSERT_EQ(list.size(), 2u);
    ASSERT_EQ(list.front()->attributes.size(), 1u);
    EXPECT_EQ(list.front()->attributes.front().name->rawValue, "c_extern");
    EXPECT_EQ(list.front()->attributes.front().arguments.front()->value->kind, joyeer::syntax::Kind::literalExpr);
    const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(PlatformTest, SelectionGrantsNoNativeAuthorization) {
    const std::vector<ModuleInput> modules { module(R"(
@platform(posix)
@c_extern(symbol: "native_posix")
func nativeCall(value: borrowing Int): Int
)", false) };
    // Selection does not authorize a unit, and the check does not depend on the
    // target: the variant is reported even where it is dropped.
    for (const auto target : {TargetPlatform::linux, TargetPlatform::windows}) {
        const std::vector<ModuleInput> unauthorized { module(R"(
@platform(posix)
@c_extern(symbol: "native_posix")
func nativeCall(value: borrowing Int): Int
)", false) };
        EXPECT_TRUE(has(select(unauthorized, target), PlatformDiagnosticId::invalidAnnotation));
    }
}

TEST_F(PlatformTest, CBindingRulesAreCheckedOnVariantsTheTargetDrops) {
    for (const char* text : {
            "@platform(windows)\n@c_extern(symbol: \"x\")\nfunc f(): Int { return 1 }\n",
            "@platform(windows)\n@c_export(symbol: \"x\")\nfunc f(): Int\n",
            "@platform(windows)\n@c_extern(symbol: \"x\")\nfunc f()\n",
            "@platform(windows)\n@c_extern(\"x\")\nfunc f(): Int\n",
            "@platform(windows)\n@c_extern(symbol: 5)\nfunc f(): Int\n",
            "@platform(windows)\n@c_extern(symbol: \"not valid\")\nfunc f(): Int\n",
            "@platform(windows)\n@c_extern(symbol: \"x\")\n@c_export(symbol: \"y\")\nfunc f(): Int\n" }) {
        for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
            const std::vector<ModuleInput> modules { module(text, true) };
            EXPECT_TRUE(has(select(modules, target), PlatformDiagnosticId::invalidAnnotation)) << text;
        }
    }
}

TEST_F(PlatformTest, ASelectedFunctionWithoutBodyOrBindingIsRejected) {
    for (const auto target : {TargetPlatform::linux, TargetPlatform::windows}) {
        const std::vector<ModuleInput> modules { module("@platform(linux)\nfunc f(): Int\n") };
        EXPECT_TRUE(has(select(modules, target), PlatformDiagnosticId::invalidAnnotation));
    }
}

TEST_F(PlatformTest, WithoutSelectionThePlatformAnnotationIsUnknownToTheTypeChecker) {
    // Selection strips the annotation; skipping it leaves an unknown one.
    const std::vector<ModuleInput> modules { module("@platform(linux)\nfunc f(): Int { return 1 }\n") };
    const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
    ASSERT_TRUE(resolution.succeeded());
    EXPECT_FALSE(joyeer::typing::TypeChecker().check(resolution.model).succeeded());
}

TEST_F(PlatformTest, DiagnosticsHaveStableNames) {
    EXPECT_STREQ(joyeer::semantic::diagnosticName(PlatformDiagnosticId::invalidAnnotation),
                 "platform.invalid-annotation");
    EXPECT_STREQ(joyeer::semantic::diagnosticName(PlatformDiagnosticId::unknownName), "platform.unknown-name");
    EXPECT_STREQ(joyeer::semantic::diagnosticName(PlatformDiagnosticId::overlappingVariants),
                 "platform.overlapping-variants");
    EXPECT_STREQ(joyeer::semantic::diagnosticName(PlatformDiagnosticId::signatureMismatch),
                 "platform.signature-mismatch");
    EXPECT_STREQ(joyeer::semantic::diagnosticName(PlatformDiagnosticId::nameCollision),
                 "platform.name-collision");
}

std::vector<PlatformDiagnosticId> ids(const std::vector<joyeer::semantic::PlatformDiagnostic>& diagnostics) {
    std::vector<PlatformDiagnosticId> result;
    for (const auto& diagnostic : diagnostics) result.push_back(diagnostic.id);
    return result;
}

std::string pair(const std::string& windows, const std::string& posix) {
    return "@platform(windows)\n" + windows + "\n@platform(posix)\n" + posix + "\n";
}

TEST_F(PlatformTest, EquivalentSpellingsAreTheSameSignature) {
    for (const auto& text : {
            pair("func f(value x: Int) { }", "func f(value x: Int): Void { }"),
            pair("func f(value x: Int): () { }", "func f(value x: Int): Void { }"),
            pair("func f(value x: Int) { }", "func f(value x: borrowing Int) { }"),
            pair("func f(value x: Int) { }", "internal func f(value y: Int) { }"),
            pair("func f(value x: Optional<Int>): Array<Int> { return [1] }",
                 "func f(value x: Int?): [Int] { return [1] }"),
            pair("func f(value x: Dict<String, Int>): Int { return 0 }",
                 "func f(value x: [String: Int]): Int { return 0 }") }) {
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(select(modules, TargetPlatform::linux).empty()) << text;
    }
}

TEST_F(PlatformTest, ANativeVariantMayPairWithAJoyeerVariantThatOmitsTheResult) {
    constexpr auto text = R"(
@platform(windows)
@c_extern(symbol: "win_touch")
func touch(value: borrowing Int): Void

@platform(posix)
func touch(value: borrowing Int) { }

func main() { touch(value: 1) }
)";
    for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
        const std::vector<ModuleInput> modules { module(text, true) };
        ASSERT_TRUE(select(modules, target).empty());
        const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(PlatformTest, RejectsMoreDifferentSignatures) {
    for (const auto& text : {
            pair("func f(value x: [String: Int]): Int { return 0 }", "func f(value x: [String: Bool]): Int { return 0 }"),
            pair("func f(a x: Int, b y: Int): Int { return 0 }", "func f(b y: Int, a x: Int): Int { return 0 }"),
            pair("func f(value x: Optional<[Int]>) { }", "func f(value x: [Int?]) { }"),
            pair("func f(value x: consuming Int) { }", "func f(value x: inout Int) { }"),
            pair("func f(value x: Int) { }", "private func f(value x: Int) { }") }) {
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(has(select(modules, TargetPlatform::linux), PlatformDiagnosticId::signatureMismatch)) << text;
    }
}

TEST_F(PlatformTest, MismatchMessageShowsBothSignatures) {
    const std::vector<ModuleInput> modules { module(
            pair("func f(value x: Int): Int { return 0 }", "func f(value x: Bool): Int { return 0 }")) };
    const auto diagnostics = select(modules, TargetPlatform::linux);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].message.find("(value: borrowing Bool) -> Int"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("(value: borrowing Int) -> Int"), std::string::npos);
    EXPECT_TRUE(diagnostics[0].related.has_value());
}

TEST_F(PlatformTest, PlatformSetBoundaries) {
    const auto body = std::string("(): Int { return 1 }\n");
    for (const auto target : {TargetPlatform::windows, TargetPlatform::linux, TargetPlatform::macos}) {
        const std::vector<ModuleInput> split { module(
                "@platform(windows)\nfunc f" + body + "@platform(linux)\nfunc f" + body +
                "@platform(macos)\nfunc f" + body) };
        EXPECT_TRUE(select(split, target).empty());
        EXPECT_EQ(functions(split.front()).size(), 1u);
        const std::vector<ModuleInput> grouped { module(
                "@platform(windows, linux)\nfunc f" + body + "@platform(macos)\nfunc f" + body) };
        EXPECT_TRUE(select(grouped, target).empty());
        EXPECT_EQ(functions(grouped.front()).size(), 1u);
    }
    const std::vector<ModuleInput> overlap { module(
            "@platform(posix)\nfunc f" + body + "@platform(macos)\nfunc f" + body) };
    EXPECT_TRUE(has(select(overlap, TargetPlatform::windows), PlatformDiagnosticId::overlappingVariants));
}

TEST_F(PlatformTest, RedundantPlatformNamesAreUnionsAndAreAccepted) {
    for (const char* list : {"windows, windows", "posix, linux", "linux, macos, posix"}) {
        const std::vector<ModuleInput> modules {
            module(std::string("@platform(") + list + ")\nfunc f(): Int { return 1 }\n") };
        EXPECT_TRUE(select(modules, TargetPlatform::windows).empty()) << list;
    }
}

TEST_F(PlatformTest, AnAnnotatedVariantOverlapsAnUnannotatedOneInAnotherFile) {
    ModuleInput input {"fixture", {
        parse("func f(): Int { return 1 }\n", 0),
        parse("@platform(windows)\nfunc f(): Int { return 2 }\n", 1) }, false};
    EXPECT_TRUE(has(select({input}, TargetPlatform::linux), PlatformDiagnosticId::overlappingVariants));
}

TEST_F(PlatformTest, PrivateFunctionsGroupOnlyWithinTheirOwnFile) {
    // §12.2: a private function is visible in one file, so the same name in
    // another file is unrelated.
    ModuleInput input {"fixture", {
        parse("private func helper(): Int { return 1 }\n", 0),
        parse("@platform(windows)\nprivate func helper(value x: Int): Int { return 2 }\n", 1) }, false};
    EXPECT_TRUE(select({input}, TargetPlatform::linux).empty());
    EXPECT_EQ(input.files[0]->items.size(), 1u);
    EXPECT_TRUE(input.files[1]->items.empty());
    ModuleInput split {"fixture", {
        parse("@platform(windows)\nprivate func f(): Int { return 1 }\n", 0),
        parse("@platform(posix)\nprivate func f(): Int { return 2 }\n", 1) }, false};
    EXPECT_TRUE(select({split}, TargetPlatform::windows).empty());
}

TEST_F(PlatformTest, PrivateVariantsInOneFileAreStillGrouped) {
    const std::vector<ModuleInput> modules { module(
            "@platform(posix)\nprivate func f(): Int { return 1 }\n"
            "@platform(linux)\nprivate func f(): Int { return 2 }\n") };
    EXPECT_TRUE(has(select(modules, TargetPlatform::windows), PlatformDiagnosticId::overlappingVariants));
}

TEST_F(PlatformTest, VariantNamesMayNotCollideWithOtherTopLevelDeclarations) {
    for (const char* other : {"struct Foo { var value: Int }", "enum Foo { A, B }", "let Foo: Int = 1"}) {
        for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
            const std::vector<ModuleInput> modules { module(
                    std::string("@platform(windows)\nfunc Foo(): Int { return 1 }\n") + other + "\n") };
            EXPECT_TRUE(has(select(modules, target), PlatformDiagnosticId::nameCollision)) << other;
        }
    }
}

TEST_F(PlatformTest, AMalformedVariantProducesOnlyItsOwnDiagnostic) {
    const std::vector<ModuleInput> modules { module(
            "@platform(freebsd)\nfunc f(): Int { return 1 }\n@platform(windows)\nfunc f(value x: Int): Int { return 2 }\n") };
    EXPECT_EQ(ids(select(modules, TargetPlatform::windows)),
              std::vector<PlatformDiagnosticId> { PlatformDiagnosticId::unknownName });
}

TEST_F(PlatformTest, OverlapIsReportedWithoutAnAdditionalMismatch) {
    const std::vector<ModuleInput> modules { module(
            "@platform(posix)\nfunc f(): Int { return 1 }\n@platform(linux)\nfunc f(value x: Int): Int { return 2 }\n") };
    EXPECT_EQ(ids(select(modules, TargetPlatform::linux)),
              std::vector<PlatformDiagnosticId> { PlatformDiagnosticId::overlappingVariants });
}

TEST_F(PlatformTest, DiagnosticsPointAtTheOffendingText) {
    const std::string overlap = "@platform(posix)\nfunc f(): Int { return 1 }\n@platform(linux)\nfunc f(): Int { return 2 }\n";
    const std::vector<ModuleInput> first { module(overlap) };
    auto diagnostics = select(first, TargetPlatform::linux);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].span.offset, overlap.rfind("@platform(linux)"));
    ASSERT_TRUE(diagnostics[0].related.has_value());
    EXPECT_EQ(diagnostics[0].related->offset, 0u);

    const std::string unknown = "@platform(windows, freebsd)\nfunc f(): Int { return 1 }\n";
    const std::vector<ModuleInput> second { module(unknown) };
    diagnostics = select(second, TargetPlatform::linux);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].span.offset, unknown.find("freebsd"));
    EXPECT_EQ(diagnostics[0].span.length, std::string("freebsd").size());
}

TEST_F(PlatformTest, DiagnosticsAreInSourceOrder) {
    const std::vector<ModuleInput> modules { module(
            "@platform(posix)\nfunc f(): Int { return 1 }\n@platform(linux)\nfunc f(): Int { return 2 }\n"
            "@platform(freebsd)\nfunc g(): Int { return 1 }\n") };
    EXPECT_EQ(ids(select(modules, TargetPlatform::linux)),
              (std::vector<PlatformDiagnosticId> {
                  PlatformDiagnosticId::overlappingVariants, PlatformDiagnosticId::unknownName }));
}

TEST_F(PlatformTest, AModuleWithDiagnosticsIsLeftUnmodified) {
    const std::vector<ModuleInput> modules { module(
            "@platform(windows)\nfunc f(): Int { return 1 }\n@platform(freebsd)\nfunc g(): Int { return 1 }\n") };
    const auto first = select(modules, TargetPlatform::linux);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(modules.front().files.front()->items.size(), 2u);
    EXPECT_EQ(functions(modules.front()).front()->attributes.size(), 1u);
    EXPECT_EQ(ids(select(modules, TargetPlatform::linux)), ids(first));
}

TEST_F(PlatformTest, AnnotationsBesideAPlatformAreCheckedEvenWhenTheTargetDropsTheFunction) {
    for (const char* text : {
            "@platform(windows)\n@bogus()\nfunc f(): Int { return 1 }\n",
            "@platform(windows)\n@c_export(symbol: \"x\")\nfunc f(): Int\n",
            "@bogus()\n@platform(windows)\nfunc f(): Int { return 1 }\n" }) {
        const std::vector<ModuleInput> modules { module(text, true) };
        EXPECT_TRUE(has(select(modules, TargetPlatform::linux), PlatformDiagnosticId::invalidAnnotation)) << text;
    }
    for (const char* text : {
            "@platform(windows)\n@c_extern(symbol: \"x\")\nfunc f(): Int\n",
            "@c_extern(symbol: \"x\")\n@platform(windows)\nfunc f(): Int\n" }) {
        for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
            const std::vector<ModuleInput> modules { module(text, true) };
            EXPECT_TRUE(select(modules, target).empty()) << text;
        }
    }
}

TEST_F(PlatformTest, MainMayHavePlatformVariants) {
    for (const auto target : {TargetPlatform::windows, TargetPlatform::macos}) {
        const std::vector<ModuleInput> modules { module(pair(
                "func main() { print(value: 1) }", "func main() { print(value: 2) }")) };
        ASSERT_TRUE(select(modules, target).empty());
        const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        EXPECT_TRUE(joyeer::typing::TypeChecker().check(resolution.model).succeeded());
    }
}

TEST_F(PlatformTest, DisjointVariantsMayExportTheSameCSymbol) {
    for (const char* text : {
            "@platform(windows)\n@c_export(symbol: \"shared_symbol\")\nfunc entry(input value: Int): Int { return value }\n"
            "@platform(posix)\n@c_export(symbol: \"shared_symbol\")\nfunc entry(input value: Int): Int { return value + 1 }\n",
            "@c_export(symbol: \"shared_symbol\")\n@platform(windows)\nfunc entry(input value: Int): Int { return value }\n"
            "@c_export(symbol: \"shared_symbol\")\n@platform(posix)\nfunc entry(input value: Int): Int { return value + 1 }\n" }) {
        for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
            const std::vector<ModuleInput> modules { module(text, true) };
            ASSERT_TRUE(select(modules, target).empty());
            const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
            ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
            const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
            EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        }
    }
    const std::vector<ModuleInput> overlapping { module(
            "@platform(posix)\n@c_export(symbol: \"shared_symbol\")\nfunc entry(input value: Int): Int { return value }\n"
            "@platform(linux)\n@c_export(symbol: \"shared_symbol\")\nfunc entry(input value: Int): Int { return value }\n", true) };
    EXPECT_TRUE(has(select(overlapping, TargetPlatform::linux), PlatformDiagnosticId::overlappingVariants));
}

TEST_F(PlatformTest, ASelectedFunctionStartsAtItsNameNotTheStrippedAnnotation) {
    const std::string text = "@platform(windows)\nfunc f(): Int { return 1 }\n";
    const std::vector<ModuleInput> modules { module(text) };
    ASSERT_TRUE(select(modules, TargetPlatform::windows).empty());
    const auto function = functions(modules.front()).front();
    EXPECT_EQ(function->span.offset, text.find("f():"));
    EXPECT_EQ(function->span.offset + function->span.length, text.rfind('}') + 1);
}

TEST_F(PlatformTest, ASelectedFunctionStartsAtItsNextRemainingAnnotation) {
    const std::string text = "@platform(windows)\n@c_extern(symbol: \"x\")\nfunc f(): Int\n";
    const std::vector<ModuleInput> modules { module(text, true) };
    ASSERT_TRUE(select(modules, TargetPlatform::windows).empty());
    EXPECT_EQ(functions(modules.front()).front()->span.offset, text.find("@c_extern"));
    const std::string reversed = "@c_extern(symbol: \"x\")\n@platform(windows)\nfunc f(): Int\n";
    const std::vector<ModuleInput> other { module(reversed, true) };
    ASSERT_TRUE(select(other, TargetPlatform::windows).empty());
    EXPECT_EQ(functions(other.front()).front()->span.offset, 0u);
}

TEST_F(PlatformTest, DiagnosticsCarryHelpText) {
    const std::vector<ModuleInput> modules { module(
            "@platform(posix)\nfunc f(): Int { return 1 }\n@platform(linux)\nfunc f(): Int { return 2 }\n") };
    const auto diagnostics = select(modules, TargetPlatform::linux);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].help.find("disjoint"), std::string::npos);
    const std::vector<ModuleInput> unknown { module("@platform(freebsd)\nfunc f(): Int { return 1 }\n") };
    EXPECT_NE(select(unknown, TargetPlatform::linux)[0].help.find("posix"), std::string::npos);
}

TEST_F(PlatformTest, TypeSpellingBoundaries) {
    for (const auto& text : {
            pair("func f(value x: Optional<Optional<Int>>) { }", "func f(value x: Optional<Int?>) { }"),
            pair("func f(value x: [[Int]]) { }", "func f(value x: Array<Array<Int>>) { }") }) {
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(select(modules, TargetPlatform::linux).empty()) << text;
    }
    for (const auto& text : {
            pair("func f(value x: [Int]) { }", "func f(value x: Array<Int, Int>) { }"),
            pair("func f(value x: File) { }", "func f(value x: io.File) { }") }) {
        const std::vector<ModuleInput> modules { module(text) };
        EXPECT_TRUE(has(select(modules, TargetPlatform::linux), PlatformDiagnosticId::signatureMismatch)) << text;
    }
}

TEST_F(PlatformTest, AVariableNamedLikeAVariantIsReportedAsAVariable) {
    const std::vector<ModuleInput> modules { module(
            "@platform(windows)\nfunc total(): Int { return 1 }\nvar total: Int = 0\n") };
    const auto diagnostics = select(modules, TargetPlatform::linux);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].message.find("variable"), std::string::npos);
}

std::vector<joyeer::parser::Diagnostic> parseDiagnostics(const std::string& text) {
    auto source = std::make_shared<SourceFile>(text);
    Diagnostics diagnostics;
    LexParser(&diagnostics).parse(source);
    return joyeer::parser::Parser(source->tokens).parse().diagnostics;
}

TEST_F(PlatformTest, AnnotationsOnMembersAndOtherDeclarationsAreRejectedOnce) {
    for (const char* text : {
            "struct S {\n    var v: Int\n    @platform(windows)\n    func get(): Int { return 1 }\n}\n",
            "struct S {\n    var v: Int\n}\nextension S {\n    @platform(windows)\n    func get(): Int { return 1 }\n}\n",
            "enum E {\n    A,\n    B\n    @platform(windows)\n    func get(): Int { return 1 }\n}\n",
            "@platform(windows)\nstruct S { var v: Int }\n",
            "@platform(windows)\nenum E { A, B }\n",
            "@platform(windows)\nlet answer: Int = 1\n",
            "struct S {\n    @platform(windows)\n    var v: Int\n}\n",
            "struct S { var v: Int }\n@platform(windows)\nextension S {\n    func get(): Int { return 1 }\n}\n" }) {
        const auto diagnostics = parseDiagnostics(text);
        ASSERT_EQ(diagnostics.size(), 1u) << text << "\n" << joyeer::parser::dump(diagnostics);
        EXPECT_EQ(diagnostics[0].id, joyeer::parser::DiagnosticId::unsupportedSyntax) << text;
        EXPECT_NE(diagnostics[0].message.find("only on top-level functions"), std::string::npos) << text;
    }
}

TEST_F(PlatformTest, AnAnnotatedEnumCaseIsReportedOnceAndTheCasesStillParse) {
    for (const char* text : {
            "enum E {\n    @platform(windows)\n    A,\n    B\n}\n",
            "enum E {\n    A,\n    @platform(windows)\n    B\n}\n",
            "enum E { A, @platform(windows) B }\n",
            "enum E {\n    A,\n    B\n    @platform(windows)\n    @platform(linux)\n    func get(): Int { return 1 }\n}\n" }) {
        const auto diagnostics = parseDiagnostics(text);
        ASSERT_EQ(diagnostics.size(), 1u) << text << "\n" << joyeer::parser::dump(diagnostics);
        EXPECT_NE(diagnostics[0].message.find("only on top-level functions"), std::string::npos) << text;
    }
}

TEST_F(PlatformTest, AnAnnotatedFunctionAfterAnUnclosedEnumKeepsItsAnnotation) {
    auto source = std::make_shared<SourceFile>(
            "enum E {\n    A,\n    B\n\n@platform(windows)\nfunc f(): Int { return 1 }\n");
    Diagnostics diagnostics;
    LexParser(&diagnostics).parse(source);
    const auto result = joyeer::parser::Parser(source->tokens).parse();
    for (const auto& diagnostic : result.diagnostics) {
        EXPECT_EQ(diagnostic.message.find("only on top-level functions"), std::string::npos)
                << joyeer::parser::dump(result.diagnostics);
    }
    ASSERT_NE(result.root, nullptr);
    bool annotated = false;
    for (const auto& item : result.root->items) {
        if (item->kind != joyeer::syntax::Kind::functionDecl) continue;
        annotated = !std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(item)->attributes.empty();
    }
    EXPECT_TRUE(annotated);
}

TEST_F(PlatformTest, ATopLevelAnnotationAfterAnUnclosedStructIsNotAMemberAnnotation) {
    const auto diagnostics = parseDiagnostics(
            "struct S {\n    var v: Int\n\n@platform(windows)\nfunc f(): Int { return 1 }\n");
    for (const auto& diagnostic : diagnostics) {
        EXPECT_EQ(diagnostic.message.find("only on top-level functions"), std::string::npos)
                << joyeer::parser::dump(diagnostics);
    }
}

TEST_F(PlatformTest, VariantsCollideWithAnyNamesakeInTheSameFile) {
    // Two declarations of a name in one file are an error whatever their
    // visibility; in another file a private declaration is unrelated.
    for (const auto target : {TargetPlatform::windows, TargetPlatform::linux}) {
        const std::vector<ModuleInput> sameFile { module(
                "@platform(windows)\nfunc Foo(): Int { return 1 }\nprivate let Foo: Int = 1\n") };
        EXPECT_TRUE(has(select(sameFile, target), PlatformDiagnosticId::nameCollision));
        ModuleInput otherFile {"fixture", {
            parse("@platform(windows)\nfunc Foo(): Int { return 1 }\n", 0),
            parse("private let Foo: Int = 1\n", 1) }, false};
        EXPECT_TRUE(select({otherFile}, target).empty());
    }
}

TEST_F(PlatformTest, ANonPrivateNamesakeInAnotherFileJoinsAPrivateVariantsGroup) {
    // Stricter than §12.2 shadowing, by design (§11.5): without @platform the
    // private function would shadow; with it, the two are variants of one name.
    ModuleInput input {"fixture", {
        parse("func helper(): Int { return 1 }\n", 0),
        parse("@platform(windows)\nprivate func helper(): Int { return 2 }\n", 1) }, false};
    EXPECT_FALSE(select({input}, TargetPlatform::linux).empty());
}

TEST_F(PlatformTest, HostPlatformMatchesTheBuild) {
#if defined(_WIN32)
    EXPECT_EQ(joyeer::semantic::hostPlatform(), TargetPlatform::windows);
#elif defined(__APPLE__)
    EXPECT_EQ(joyeer::semantic::hostPlatform(), TargetPlatform::macos);
#else
    EXPECT_EQ(joyeer::semantic::hostPlatform(), TargetPlatform::linux);
#endif
}

} // namespace
