#include "compiler/driver/compiler+service.h"
#include "compiler/driver/toolchain.h"
#include "compiler/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

class ModuleServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> sequence {0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
        const auto processId = _getpid();
#else
        const auto processId = getpid();
#endif
        directory = std::filesystem::current_path() /
                ("module-service-test-" + std::to_string(processId) + "-" +
                 std::to_string(stamp) + "-" + std::to_string(sequence++));
        ownsDirectory = std::filesystem::create_directory(directory);
        ASSERT_TRUE(ownsDirectory);
        options.workingDirectory = directory;
        options.moduleName = "project.app";
    }

    void TearDown() override {
        if (!ownsDirectory) return;
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        EXPECT_FALSE(error) << error.message();
    }

    std::filesystem::path write(const std::filesystem::path& relative, const std::string& text) {
        const auto path = directory / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        EXPECT_TRUE(stream.good());
        return path;
    }

    std::filesystem::path root(const std::filesystem::path& relative, const std::string& text) {
        const auto path = write(relative, text);
        options.sourceFiles.push_back(path);
        return path;
    }

    void dependency(const std::string& name, const std::filesystem::path& relative, const std::string& text) {
        const auto path = write(relative, text);
        const auto found = std::find_if(options.modules.begin(), options.modules.end(),
                [&](const auto& module) { return module.name == name; });
        if (found == options.modules.end()) options.modules.push_back({name, {path}});
        else found->files.push_back(path);
    }

    bool hasCode(const std::string& code) const {
        return std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(),
                [&](const auto& error) { return error.code == code; });
    }

    std::string errors() const {
        std::string text;
        for (const auto& error : diagnostics.errors) text += error.code + ": " + error.message + "\n";
        return text;
    }

    std::filesystem::path directory;
    bool ownsDirectory = false;
    joyeer::CompileOptions options;
    Diagnostics diagnostics;
};

TEST_F(ModuleServiceTest, CompilesExplicitSourcesAcrossDirectoriesWithoutDiscoveringSiblings) {
    root("elsewhere/deep/z.joyeer", "func answer(): Int { return 42 }\n");
    const auto firstText = std::string("func main() { print(value: answer()) }\n");
    root("app/a.joyeer", firstText);
    write("app/unlisted.joyeer", "not valid source\n");
    write("elsewhere/deep/nested/unlisted.joyeer", "not valid source\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    const auto& result = compiler.getLastCompiledSourceFile();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->content, firstText);
    ASSERT_NE(result->joyeerIR, nullptr);
    ASSERT_EQ(result->joyeerIR->sourceFiles.size(), 2u);
    EXPECT_EQ(result->joyeerIR->sourceFiles[0].fileName, "a.joyeer");
    EXPECT_EQ(result->joyeerIR->sourceFiles[1].fileName, "z.joyeer");
    EXPECT_TRUE(result->llvmHasEntryPoint);
    const auto ir = result->llvmIR;

    std::reverse(options.sourceFiles.begin(), options.sourceFiles.end());
    CompilerService reordered(&diagnostics, options);
    reordered.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_EQ(reordered.getLastCompiledSourceFile()->llvmIR, ir);
}

TEST_F(ModuleServiceTest, DistinctModulesMayUseFilesFromTheSameDirectory) {
    root("shared/main.joyeer", "import project.config\nfunc main() { print(value: project.config.answer()) }\n");
    dependency("project.config", "shared/config.joyeer",
               "public func answer(): Int { return helper() }\n");
    dependency("project.config", "elsewhere/helper.joyeer", "func helper(): Int { return 42 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_EQ(compiler.getLastCompiledSourceFile()->joyeerIR->sourceFiles.size(), 3u);
}

TEST_F(ModuleServiceTest, ModuleNamesDoNotImplyParentModulesOrDirectories) {
    root("main.joyeer", "import alpha.beta.gamma\nfunc main() { print(value: alpha.beta.gamma.answer()) }\n");
    dependency("alpha.beta.gamma", "unrelated/source.joyeer", "public func answer(): Int { return 42 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_EQ(compiler.getLastCompiledSourceFile()->joyeerIR->sourceFiles.size(), 2u);
}

TEST_F(ModuleServiceTest, CompilesPublicShortNamesWithoutMergingModuleIdentities) {
    root("main.joyeer",
         "import project.model\nfunc main() {\n"
         "let item: Record = Record(value: answer())\nprint(value: item.value)\n}\n");
    dependency("project.model", "model.joyeer",
               "public struct Record {\npublic let value: Int\n}\n"
               "public func answer(): Int { return 42 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    const auto& compiled = compiler.getLastCompiledSourceFile();
    ASSERT_NE(compiled->joyeerIR, nullptr);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*compiled->joyeerIR).succeeded());
    EXPECT_EQ(compiled->joyeerIR->sourceFiles.size(), 2u);
    EXPECT_TRUE(compiled->llvmHasEntryPoint);
}

TEST_F(ModuleServiceTest, ReportsShortNameAmbiguityAtTheImportingUse) {
    root("root/main.joyeer",
         "import alpha\nimport beta\nfunc main() { answer() }\n");
    dependency("alpha", "alpha.joyeer", "public func answer(): Int { return 1 }\n");
    dependency("beta", "beta.joyeer", "public func answer(): Int { return 2 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(hasCode("name-resolution.ambiguous-import")) << errors();
    ASSERT_EQ(diagnostics.errors.size(), 1u) << errors();
    const auto& error = diagnostics.errors.front();
    EXPECT_EQ(std::filesystem::path(error.path), std::filesystem::path("root/main.joyeer"));
    EXPECT_EQ(error.lineAt, 2);
    EXPECT_EQ(error.sourceLine, "func main() { answer() }");
    EXPECT_NE(error.message.find("'alpha.answer', 'beta.answer'"), std::string::npos);
}

TEST_F(ModuleServiceTest, CompilesReachableGraphWithoutParsingUnusedSources) {
    root("root/main.joyeer", "import project.config\nfunc main() { print(value: project.config.answer()) }\n");
    dependency("project.config", "config/config.joyeer",
               "import project.values\npublic func answer(): Int { return project.values.answer() }\n");
    dependency("project.values", "values/value.joyeer", "public func answer(): Int { return 42 }\n");
    dependency("unused", "unused/broken.joyeer", "this should never be parsed\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_EQ(compiler.getLastCompiledSourceFile()->joyeerIR->sourceFiles.size(), 3u);
}

TEST_F(ModuleServiceTest, ResolvesRelativeFilePathsAgainstExplicitWorkingDirectory) {
    write("src/main.joyeer", "import configuration\nfunc main() { print(value: configuration.value()) }\n");
    write("other/config.joyeer", "public func value(): Int { return 42 }\n");
    options.sourceFiles = {"src/main.joyeer"};
    options.modules = {{"configuration", {"other/config.joyeer"}}};
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_EQ(compiler.getLastCompiledSourceFile()->getAbstractPath(),
              std::filesystem::canonical(directory / "src/main.joyeer"));
}

TEST_F(ModuleServiceTest, ReportsUnknownImportAtItsOwnFile) {
    root("root/a.joyeer", "func main() {}\n");
    root("root/nested/z.joyeer", "import missing.module\nfunc helper() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(hasCode("module.unknown-import")) << errors();
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.back().path), std::filesystem::path("root/nested/z.joyeer"));
    EXPECT_EQ(diagnostics.errors.back().lineAt, 0);
    EXPECT_EQ(diagnostics.errors.back().sourceLine, "import missing.module");
}

TEST_F(ModuleServiceTest, ReportsEntireDependencyCycleAtClosingImport) {
    root("root/main.joyeer", "import first\nfunc main() {}\n");
    dependency("first", "first.joyeer", "import second\npublic func first() {}\n");
    dependency("second", "second.joyeer", "import first\npublic func second() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(hasCode("module.import-cycle")) << errors();
    EXPECT_NE(diagnostics.errors.back().message.find("first -> second -> first"), std::string::npos);
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.back().path).filename(), "second.joyeer");
}

TEST_F(ModuleServiceTest, DetectsCyclesBackToTheNamedRoot) {
    root("main.joyeer", "import dependency\nfunc main() {}\n");
    dependency("dependency", "dependency.joyeer", "import project.app\npublic func helper() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(hasCode("module.import-cycle")) << errors();
    EXPECT_NE(diagnostics.errors.back().message.find("project.app -> dependency -> project.app"), std::string::npos);
}

TEST_F(ModuleServiceTest, ReportsParserErrorsFromLaterFiles) {
    root("root/a.joyeer", "func main() {}\n");
    root("elsewhere/z.joyeer", "func broken( {\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path).filename(), "z.joyeer");
}

TEST_F(ModuleServiceTest, PreservesPerFileDiagnosticsInEveryFrontendStage) {
    root("root/main.joyeer", "import dependency\nfunc main() {}\n");
    for (const auto& [text, prefix] : {
            std::pair {"public func broken() { let text = \"unterminated\n", "lexer."},
            std::pair {"public func broken(): Int { return missing }\n", "name-resolution."},
            std::pair {"public func broken(): Int { return true }\n", "type-checking."},
            std::pair {"public func broken(): Int { let unused = 1 }\n", "semantic-analysis."},
        }) {
        SCOPED_TRACE(prefix);
        diagnostics.errors.clear();
        options.modules = {{"dependency", {write("dependency/nested/broken.joyeer", text)}}};
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        ASSERT_TRUE(diagnostics.hasFailure()) << errors();
        EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path),
                  std::filesystem::path("dependency/nested/broken.joyeer"));
        EXPECT_TRUE(diagnostics.errors.front().code.starts_with(prefix)) << errors();
    }
}

TEST_F(ModuleServiceTest, DoesNotShareImportsBetweenFilesInOneModule) {
    root("a.joyeer", "import dependency\nfunc helper() { dependency.answer() }\n");
    root("nested/z.joyeer", "func main() { dependency.answer() }\n");
    dependency("dependency", "dependency.joyeer", "public func answer(): Int { return 42 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(diagnostics.hasFailure());
    EXPECT_TRUE(std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(), [](const auto& error) {
        return std::filesystem::path(error.path).filename() == "z.joyeer";
    })) << errors();
}

TEST_F(ModuleServiceTest, DirectoriesDoNotRelaxPrivateVisibility) {
    root("one/a.joyeer", "private func hidden(): Int { return 42 }\n");
    root("two/main.joyeer", "func main() { print(value: hidden()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(diagnostics.hasFailure());
}

TEST_F(ModuleServiceTest, SameDirectoryDoesNotRelaxModuleVisibility) {
    root("main.joyeer", "import dependency\nfunc main() { dependency.hidden() }\n");
    dependency("dependency", "library.joyeer", "func hidden() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(diagnostics.hasFailure());
}

TEST_F(ModuleServiceTest, RejectsDuplicateDeclarationsAcrossDirectories) {
    root("one/first.joyeer", "func duplicated() {}\n");
    root("two/second.joyeer", "func duplicated() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("name-resolution.duplicate-declaration")) << errors();
}

TEST_F(ModuleServiceTest, RejectsRepeatedSourceWithinAUnit) {
    const auto file = root("main.joyeer", "func main() {}\n");
    options.sourceFiles.push_back(file.parent_path() / "." / file.filename());
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.duplicate-source")) << errors();
}

TEST_F(ModuleServiceTest, RejectsSourceSharedByDifferentModulesEvenIfUnreachable) {
    const auto file = root("main.joyeer", "func main() {}\n");
    options.modules.push_back({"unused", {file}});
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.duplicate-source")) << errors();
}

TEST_F(ModuleServiceTest, RejectsDuplicateLogicalNamesAndRootNameCollisions) {
    root("main.joyeer", "func main() {}\n");
    for (const bool rootCollision : {false, true}) {
        diagnostics.errors.clear();
        options.modules = {{"duplicate", {write("one.joyeer", "func one() {}\n")}},
                           {rootCollision ? "project.app" : "duplicate", {write("two.joyeer", "func two() {}\n")}}};
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        EXPECT_TRUE(hasCode("module.duplicate-name")) << errors();
    }
}

TEST_F(ModuleServiceTest, RejectsInvalidRootAndDependencyNames) {
    root("main.joyeer", "func main() {}\n");
    options.moduleName = "";
    CompilerService unnamed(&diagnostics, options);
    unnamed.compile();
    EXPECT_TRUE(hasCode("module.invalid-name")) << errors();
    diagnostics.errors.clear();
    options.moduleName = "project.app";
    dependency("bad..name", "dependency.joyeer", "func helper() {}\n");
    CompilerService invalid(&diagnostics, options);
    invalid.compile();
    EXPECT_TRUE(hasCode("module.invalid-name")) << errors();
}

TEST_F(ModuleServiceTest, RejectsHardLinkedAndSymlinkedDuplicateSources) {
    const auto source = root("main.joyeer", "func main() {}\n");
    for (const bool symbolic : {false, true}) {
        diagnostics.errors.clear();
        const auto alias = directory / (symbolic ? "symbolic.joyeer" : "hard.joyeer");
        std::error_code error;
        if (symbolic) std::filesystem::create_symlink(source, alias, error);
        else std::filesystem::create_hard_link(source, alias, error);
        if (error) {
            if (symbolic) GTEST_SKIP() << "symbolic links unavailable: " << error.message();
            FAIL() << "hard link creation failed: " << error.message();
        }
        options.modules = {{"dependency", {alias}}};
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        EXPECT_TRUE(hasCode("module.duplicate-source")) << errors();
    }
}

TEST_F(ModuleServiceTest, RejectsMissingNonregularAndEmptySourcePaths) {
    root("main.joyeer", "func main() {}\n");
    for (const auto& path : {directory / "missing.joyeer", directory, std::filesystem::path()}) {
        diagnostics.errors.clear();
        options.modules = {{"unused", {path}}};
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        EXPECT_TRUE(hasCode("module.read-source")) << errors();
    }
}

TEST_F(ModuleServiceTest, RejectsEmptyRootAndDependencySets) {
    write("nested/main.joyeer", "func main() {}\n");
    CompilerService emptyRoot(&diagnostics, options);
    emptyRoot.compile();
    EXPECT_TRUE(hasCode("module.empty")) << errors();
    diagnostics.errors.clear();
    root("main.joyeer", "func main() {}\n");
    options.modules = {{"unused", {}}};
    CompilerService emptyDependency(&diagnostics, options);
    emptyDependency.compile();
    EXPECT_TRUE(hasCode("module.empty")) << errors();
}

TEST_F(ModuleServiceTest, ProtectsUnreachableModuleInputsFromOutput) {
    root("main.joyeer", "func main() {}\n");
    const std::string text = "public func value(): Int { return 42 }\n";
    dependency("unused", "value.joyeer", text);
    options.outputMode = joyeer::OutputMode::llvmIR;
    options.outputFile = directory / "value.joyeer";
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("driver.output-file-error")) << errors();
    std::ifstream source(options.outputFile, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(source), {}), text);
}

TEST_F(ModuleServiceTest, ProtectsHardLinkedSourceFromOutput) {
    const auto source = root("main.joyeer", "func main() {}\n");
    options.outputMode = joyeer::OutputMode::llvmIR;
    options.outputFile = directory / "output.ll";
    std::error_code error;
    std::filesystem::create_hard_link(source, options.outputFile, error);
    ASSERT_FALSE(error) << error.message();
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("driver.output-file-error")) << errors();
}

#if defined(_WIN32)
TEST_F(ModuleServiceTest, ProtectsInputAliasedBySiblingPDBCleanup) {
    const auto source = root("main.joyeer", "func main() {}\n");
    options.outputMode = joyeer::OutputMode::executable;
    options.outputFile = directory / "program.exe";
    std::error_code error;
    std::filesystem::create_hard_link(source, directory / "program.pdb", error);
    ASSERT_FALSE(error) << error.message();
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("driver.output-file-error")) << errors();
}
#endif

TEST_F(ModuleServiceTest, EmitsWholeGraphIRWithOnlyTheRootEntry) {
    root("main.joyeer", "import dependency\nfunc main() { print(value: dependency.answer()) }\n");
    dependency("dependency", "nested/answer.joyeer",
               "public func answer(): Int { return 42 }\npublic func main(value: Int): Bool { return true }\n");
    options.outputMode = joyeer::OutputMode::llvmIR;
    options.outputFile = directory / "output.ll";
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    const auto& result = compiler.getLastCompiledSourceFile();
    std::ifstream output(options.outputFile, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(output), {}), result->llvmIR);
    EXPECT_TRUE(result->llvmHasEntryPoint);
    const auto& functions = result->joyeerIR->functions;
    EXPECT_EQ(std::count_if(functions.begin(), functions.end(), [](const auto& function) {
        return function.name == "main" && function.isRootModule;
    }), 1);
    EXPECT_EQ(std::count_if(functions.begin(), functions.end(), [](const auto& function) {
        return function.name == "main" && !function.isRootModule;
    }), 1);
}

TEST_F(ModuleServiceTest, DependencyMainDoesNotProvideANativeEntryPoint) {
    root("helper.joyeer", "import dependency\nfunc helper() {}\n");
    dependency("dependency", "other/main.joyeer", "public func main() {}\n");
    options.outputMode = joyeer::OutputMode::executable;
    options.outputFile = directory / "program";
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_FALSE(compiler.getLastCompiledSourceFile()->llvmHasEntryPoint);
}

TEST_F(ModuleServiceTest, PreservesSingleFileCompilation) {
    const auto file = write("single.joyeer", "func main() {}\n");
    options.moduleName.clear();
    CompilerService compiler(&diagnostics, options);
    compiler.compile(file);
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_TRUE(compiler.getLastCompiledSourceFile()->llvmHasEntryPoint);
}

TEST_F(ModuleServiceTest, RejectsConflictingServiceInputs) {
    const auto file = root("main.joyeer", "func main() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile(file);
    EXPECT_TRUE(hasCode("module.conflicting-input")) << errors();
}

TEST_F(ModuleServiceTest, RejectsDependenciesWithoutARootCompilationUnit) {
    options.moduleName.clear();
    dependency("dependency", "library.joyeer", "func helper() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.missing-root")) << errors();
}

constexpr auto platformSource =
        "@platform(windows)\nfunc code(): Int { return 1111 }\n"
        "@platform(posix)\nfunc code(): Int { return 2222 }\n"
        "func main() { print(value: code()) }\n";

TEST_F(ModuleServiceTest, SelectsPlatformFunctionsForTheTargetPlatform) {
    root("main.joyeer", platformSource);
    std::string windowsIR;
    std::string linuxIR;
    for (const auto target : {joyeer::semantic::TargetPlatform::windows,
                              joyeer::semantic::TargetPlatform::linux}) {
        options.targetPlatform = target;
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        ASSERT_FALSE(diagnostics.hasFailure()) << errors();
        const auto& ir = compiler.getLastCompiledSourceFile()->llvmIR;
        (target == joyeer::semantic::TargetPlatform::windows ? windowsIR : linuxIR) = ir;
    }
    EXPECT_NE(windowsIR.find("1111"), std::string::npos);
    EXPECT_EQ(windowsIR.find("2222"), std::string::npos);
    EXPECT_NE(linuxIR.find("2222"), std::string::npos);
    EXPECT_EQ(linuxIR.find("1111"), std::string::npos);
}

TEST_F(ModuleServiceTest, DefaultsTheTargetPlatformToTheHost) {
    EXPECT_EQ(options.targetPlatform, joyeer::semantic::hostPlatform());
    root("main.joyeer", platformSource);
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
}

TEST_F(ModuleServiceTest, ReportsPlatformDiagnosticsWithTheirStableCodes) {
    root("main.joyeer",
         "@platform(posix)\nfunc code(): Int { return 1 }\n"
         "@platform(linux)\nfunc code(): Int { return 2 }\n"
         "func main() { print(value: code()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("platform.overlapping-variants")) << errors();
}

TEST_F(ModuleServiceTest, RejectsANonHostTargetUnlessValidating) {
    root("main.joyeer", platformSource);
    const auto other = joyeer::semantic::hostPlatform() == joyeer::semantic::TargetPlatform::windows
            ? joyeer::semantic::TargetPlatform::linux
            : joyeer::semantic::TargetPlatform::windows;
    options.targetPlatform = other;
    options.outputFile = directory / "out-program";
    for (const auto mode : {joyeer::OutputMode::executable, joyeer::OutputMode::llvmIR,
                            joyeer::OutputMode::runtimeObject}) {
        diagnostics = Diagnostics();
        options.outputMode = mode;
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        EXPECT_TRUE(hasCode("driver.unsupported-target-platform")) << errors();
        EXPECT_FALSE(std::filesystem::exists(options.outputFile));
    }
    diagnostics = Diagnostics();
    options.outputMode = joyeer::OutputMode::validate;
    CompilerService validating(&diagnostics, options);
    validating.compile();
    EXPECT_FALSE(diagnostics.hasFailure()) << errors();
}

TEST_F(ModuleServiceTest, PlatformErrorsStopTheCompilationBeforeNameResolution) {
    root("a.joyeer",
         "@platform(posix)\nfunc code(): Int { return 1 }\n@platform(linux)\nfunc code(): Int { return 2 }\n"
         "func main() { print(value: code()) }\n");
    root("b.joyeer", "func other(): Int { return missing() }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("platform.overlapping-variants")) << errors();
    EXPECT_FALSE(hasCode("name-resolution.undefined-name")) << errors();
}

TEST_F(ModuleServiceTest, PlatformDiagnosticsNameTheOtherFileWhenTheRelatedDeclarationIsElsewhere) {
    root("a.joyeer", "@platform(posix)\nfunc code(): Int { return 1 }\nfunc main() { print(value: code()) }\n");
    root("b.joyeer", "@platform(linux)\nfunc code(): Int { return 2 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(hasCode("platform.overlapping-variants")) << errors();
    const auto found = std::find_if(diagnostics.errors.begin(), diagnostics.errors.end(),
            [](const auto& error) { return error.code == "platform.overlapping-variants"; });
    EXPECT_NE(found->message.find("a.joyeer"), std::string::npos) << found->message;
    EXPECT_TRUE(found->help.has_value());
}

TEST_F(ModuleServiceTest, MissingReturnInASelectedFunctionPointsAtTheFunctionNotTheAnnotation) {
    const auto missingReturnLine = [&](const std::string& source) {
        options.sourceFiles.clear();
        diagnostics = Diagnostics();
        root("main.joyeer", source);
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        const auto found = std::find_if(diagnostics.errors.begin(), diagnostics.errors.end(),
                [](const auto& error) { return error.code == "semantic-analysis.missing-return"; });
        EXPECT_NE(found, diagnostics.errors.end()) << errors();
        return found == diagnostics.errors.end() ? -1 : found->lineAt;
    };
    // The annotated function's error lands on the line the unannotated one
    // gets when it sits where the annotated function's name is.
    const auto annotated = missingReturnLine(
            "@platform(windows, linux, macos)\nfunc broken(): Int { }\nfunc main() { }\n");
    const auto plain = missingReturnLine("\nfunc broken(): Int { }\nfunc main() { }\n");
    EXPECT_EQ(annotated, plain);
}

TEST_F(ModuleServiceTest, PlatformVariantsInADependencyAreSelectedForTheImporter) {
    root("main.joyeer",
         "import project.dep\nfunc main() { print(value: project.dep.code()) }\n");
    dependency("project.dep", "dep.joyeer",
               "@platform(windows)\npublic func code(): Int { return 1111 }\n"
               "@platform(posix)\npublic func code(): Int { return 2222 }\n");
    for (const auto target : {joyeer::semantic::TargetPlatform::windows,
                              joyeer::semantic::TargetPlatform::macos}) {
        diagnostics = Diagnostics();
        options.targetPlatform = target;
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        ASSERT_FALSE(diagnostics.hasFailure()) << errors();
        const auto& ir = compiler.getLastCompiledSourceFile()->llvmIR;
        const auto expected = target == joyeer::semantic::TargetPlatform::windows ? "1111" : "2222";
        EXPECT_NE(ir.find(expected), std::string::npos);
    }
}

TEST_F(ModuleServiceTest, ReferenceToAFunctionWithoutAVariantForTheTargetFails) {
    root("main.joyeer",
         "@platform(windows)\nfunc code(): Int { return 1 }\n"
         "func main() { print(value: code()) }\n");
    options.targetPlatform = joyeer::semantic::TargetPlatform::linux;
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("name-resolution.undefined-name")) << errors();
}

// Writes <directory>/toolchain/lib with an index stamped for this compiler.
class StandardLibraryTest : public ModuleServiceTest {
protected:
    void SetUp() override {
        ModuleServiceTest::SetUp();
        library = directory / "toolchain" / "lib";
        options.libraryDirectory = library;
        options.standardLibrary = true;
    }

    void index(const std::string& modules, const std::string& version = std::string()) {
        const auto stamp = joyeer::expectedToolchainStamp();
        write("toolchain/lib/index.toml",
              "schema-version = 1\n"
              "compiler-version = \"" + (version.empty() ? stamp.version : version) + "\"\n"
              "target = \"" + stamp.target + "\"\n"
              "configuration = \"" + stamp.configuration + "\"\n"
              "modules = [" + modules + "]\n");
    }

    void module(const std::string& name, const std::string& source, const std::string& extra = std::string()) {
        write("toolchain/lib/" + name + "/project.toml",
              "schema-version = 1\n[module]\nname = \"" + name + "\"\nkind = \"source\"\n"
              "sources = [\"" + name + ".joyeer\"]\ndependencies = []\n" + extra);
        write("toolchain/lib/" + name + "/" + name + ".joyeer", source);
    }

    std::filesystem::path library;
};

TEST_F(StandardLibraryTest, ResolvesAnImportedStandardModuleWithoutSuppliedSources) {
    index("\"Answer\"");
    module("Answer", "public func answer(): Int { return 42 }\n");
    root("main.joyeer", "import Answer\nfunc main() { print(value: Answer.answer()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_TRUE(compiler.getLastCompiledSourceFile()->llvmHasEntryPoint);
}

TEST_F(StandardLibraryTest, ResolvesStandardModulesForASingleFile) {
    index("\"Answer\"");
    module("Answer", "public func answer(): Int { return 42 }\n");
    options.moduleName.clear();
    const auto file = write("single.joyeer", "import Answer\nfunc main() { print(value: Answer.answer()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile(file);
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_TRUE(compiler.getLastCompiledSourceFile()->llvmHasEntryPoint);
}

TEST_F(StandardLibraryTest, LoadsOnlyTheImportedStandardModules) {
    index("\"Answer\", \"Broken\"");
    module("Answer", "public func answer(): Int { return 42 }\n");
    module("Broken", "this is not valid source\n");
    root("main.joyeer", "import Answer\nfunc main() { print(value: Answer.answer()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_FALSE(diagnostics.hasFailure()) << errors();
}

TEST_F(StandardLibraryTest, ResolvesStandardDependenciesOfStandardModules) {
    index("\"Outer\", \"Inner\"");
    module("Inner", "public func inner(): Int { return 7 }\n");
    module("Outer", "import Inner\npublic func outer(): Int { return Inner.inner() }\n");
    root("main.joyeer", "import Outer\nfunc main() { print(value: Outer.outer()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_FALSE(diagnostics.hasFailure()) << errors();
}

TEST_F(StandardLibraryTest, SuppliedModulesAreSearchedBeforeTheToolchain) {
    index("\"Answer\"");
    module("Answer", "public func answer(): Int { return 42 }\n");
    root("main.joyeer", "import Other\nfunc main() { print(value: Other.other()) }\n");
    dependency("Other", "other.joyeer", "public func other(): Int { return 1 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_FALSE(diagnostics.hasFailure()) << errors();
}

TEST_F(StandardLibraryTest, ASuppliedModuleOverridesAStandardModuleOfTheSameName) {
    index("\"Answer\"");
    // The toolchain copy is invalid; only the supplied module may be loaded.
    module("Answer", "this is not valid source\n");
    root("main.joyeer", "import Answer\nfunc main() { print(value: Answer.answer()) }\n");
    dependency("Answer", "mine.joyeer", "public func answer(): Int { return 1 }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_FALSE(diagnostics.hasFailure()) << errors();
}

TEST_F(StandardLibraryTest, RejectsALibraryStampedByAnotherCompiler) {
    index("\"Answer\"", "9.9.9");
    module("Answer", "public func answer(): Int { return 42 }\n");
    root("main.joyeer", "import Answer\nfunc main() { print(value: Answer.answer()) }\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("toolchain.library-mismatch")) << errors();
}

TEST_F(StandardLibraryTest, ReportsUnknownImportsAndDisabledLibraries) {
    index("\"Answer\"");
    module("Answer", "public func answer(): Int { return 42 }\n");
    root("main.joyeer", "import Answer\nimport Missing\nfunc main() {}\n");
    {
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        EXPECT_TRUE(hasCode("module.unknown-import")) << errors();
    }
    diagnostics.errors.clear();
    options.standardLibrary = false;
    CompilerService disabled(&diagnostics, options);
    disabled.compile();
    EXPECT_TRUE(hasCode("module.unknown-import")) << errors();
}

TEST_F(StandardLibraryTest, ToolchainAuthorizesNativeArchivesOnlyForImportedModules) {
    index("\"Native\", \"Plain\"");
    module("Native", "public func value(): Int { return 3 }\n",
           "native-bindings = false\nnative-libraries = [\"native/archive.a\"]\n");
    module("Plain", "public func plain(): Int { return 4 }\n");
    write("toolchain/lib/Native/native/archive.a", "archive");
    {
        root("main.joyeer", "import Native\nfunc main() { print(value: Native.value()) }\n");
        CompilerService compiler(&diagnostics, options);
        compiler.compile();
        ASSERT_FALSE(diagnostics.hasFailure()) << errors();
        ASSERT_EQ(compiler.implicitNativeLibraries().size(), 1u);
        EXPECT_EQ(compiler.implicitNativeLibraries()[0].filename(), "archive.a");
    }
    options.sourceFiles.clear();
    root("other.joyeer", "import Plain\nfunc main() { print(value: Plain.plain()) }\n");
    CompilerService plain(&diagnostics, options);
    plain.compile();
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_TRUE(plain.implicitNativeLibraries().empty());
}

TEST_F(StandardLibraryTest, RejectsUnsafeStandardModulePaths) {
    index("\"Answer\"");
    write("toolchain/lib/Answer/project.toml",
          "schema-version = 1\n[module]\nname = \"Answer\"\nkind = \"source\"\n"
          "sources = [\"../escape.joyeer\"]\ndependencies = []\n");
    root("main.joyeer", "import Answer\nfunc main() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("toolchain.library-invalid")) << errors();
}


} // namespace
