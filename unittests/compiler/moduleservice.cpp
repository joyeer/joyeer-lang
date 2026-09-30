#include "joyeer/compiler/compiler+service.h"
#include "joyeer/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace {

class ModuleServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> sequence {0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::current_path() /
                ("module-service-test-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        options.workingDirectory = directory;
        options.moduleName = "project.app";
    }

    void TearDown() override {
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

} // namespace
