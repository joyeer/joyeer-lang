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

namespace {

class ModuleServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> sequence {0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::current_path() /
                ("module-service-test-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        std::filesystem::create_directory(directory / "root");
        options.workingDirectory = directory;
        options.moduleRoot = directory / "root";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path write(const std::filesystem::path& relative, const std::string& text) {
        const auto path = directory / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        EXPECT_TRUE(stream.good());
        return path;
    }

    void map(const std::string& name, const std::filesystem::path& relative) {
        std::filesystem::create_directories(directory / relative);
        options.modules.push_back({name, directory / relative});
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

TEST_F(ModuleServiceTest, CollectsSortedDirectFilesBeforeCheckingBodies) {
    write("root/z.joyeer", "func answer(): Int { return 42 }\n");
    const auto firstText = std::string("func main() { print(value: answer()) }\n");
    write("root/a.joyeer", firstText);
    write("root/nested/ignored.joyeer", "not valid source\n");
    write("root/ignored.txt", "not valid source\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    const auto& result = compiler.getLastCompiledSourceFile();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->content, firstText);
    EXPECT_EQ(result->getAbstractPath().filename(), "a.joyeer");
    ASSERT_NE(result->joyeerIR, nullptr);
    ASSERT_EQ(result->joyeerIR->sourceFiles.size(), 2u);
    EXPECT_EQ(result->joyeerIR->sourceFiles[0].fileName, "a.joyeer");
    EXPECT_EQ(result->joyeerIR->sourceFiles[1].fileName, "z.joyeer");
    EXPECT_TRUE(result->llvmHasEntryPoint);
}

TEST_F(ModuleServiceTest, CompilesReachableGraphWithoutParsingUnusedMappings) {
    write("root/main.joyeer", "import project.config\nfunc main() { print(value: project.config.answer()) }\n");
    write("config/config.joyeer", "import project.values\npublic func answer(): Int { return project.values.answer() }\n");
    write("values/value.joyeer", "public func answer(): Int { return 42 }\n");
    write("unused/broken.joyeer", "this should never be parsed\n");
    map("project.config", "config");
    map("project.values", "values");
    map("unused", "unused");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    ASSERT_NE(compiler.getLastCompiledSourceFile()->joyeerIR, nullptr);
    EXPECT_EQ(compiler.getLastCompiledSourceFile()->joyeerIR->sourceFiles.size(), 3u);
}

TEST_F(ModuleServiceTest, ReportsUnknownImportAtItsOwnFile) {
    write("root/a.joyeer", "func main() {}\n");
    write("root/z.joyeer", "import missing.module\nfunc helper() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(hasCode("module.unknown-import")) << errors();
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.back().path).filename(), "z.joyeer");
    EXPECT_EQ(diagnostics.errors.back().lineAt, 0);
    EXPECT_EQ(diagnostics.errors.back().sourceLine, "import missing.module");
}

TEST_F(ModuleServiceTest, ReportsEntireDependencyCycleAtClosingImport) {
    write("root/main.joyeer", "import first\nfunc main() {}\n");
    write("first/first.joyeer", "import second\npublic func first() {}\n");
    write("second/second.joyeer", "import first\npublic func second() {}\n");
    map("first", "first");
    map("second", "second");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(hasCode("module.import-cycle")) << errors();
    EXPECT_NE(diagnostics.errors.back().message.find("first -> second -> first"), std::string::npos);
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.back().path).filename(), "second.joyeer");
}

TEST_F(ModuleServiceTest, ReportsParserErrorsFromLaterFiles) {
    write("root/a.joyeer", "func main() {}\n");
    write("root/z.joyeer", "func broken( {\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path).filename(), "z.joyeer");
}

TEST_F(ModuleServiceTest, ReportsLexerErrorsFromDependencies) {
    write("root/main.joyeer", "import dependency\nfunc main() {}\n");
    write("dependency/broken.joyeer", "public func broken() { let text = \"unterminated\n");
    map("dependency", "dependency");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path).filename(), "broken.joyeer");
    EXPECT_EQ(diagnostics.errors.front().code, "lexer.unterminated-string");
}

TEST_F(ModuleServiceTest, DoesNotShareImportsBetweenSiblingFiles) {
    write("root/a.joyeer", "import dependency\nfunc helper() { dependency.answer() }\n");
    write("root/z.joyeer", "func main() { dependency.answer() }\n");
    write("dependency/answer.joyeer", "public func answer(): Int { return 42 }\n");
    map("dependency", "dependency");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_TRUE(std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(), [](const auto& error) {
        return std::filesystem::path(error.path).filename() == "z.joyeer";
    })) << errors();
}

TEST_F(ModuleServiceTest, RejectsImportsAfterDeclarations) {
    write("root/main.joyeer", "func main() {}\nimport dependency\n");
    write("dependency/answer.joyeer", "public func answer(): Int { return 42 }\n");
    map("dependency", "dependency");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path).filename(), "main.joyeer");
}

TEST_F(ModuleServiceTest, ReportsNameResolutionErrorsFromDependencies) {
    write("root/main.joyeer", "import dependency\nfunc main() {}\n");
    write("dependency/broken.joyeer", "public func broken(): Int { return missing }\n");
    map("dependency", "dependency");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path).filename(), "broken.joyeer");
    EXPECT_NE(diagnostics.errors.front().sourceLine.find("return missing"), std::string::npos);
}

TEST_F(ModuleServiceTest, ReportsTypeErrorsFromDependencies) {
    write("root/main.joyeer", "import dependency\nfunc main() {}\n");
    write("dependency/broken.joyeer", "public func broken(): Int { return true }\n");
    map("dependency", "dependency");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_EQ(std::filesystem::path(diagnostics.errors.front().path).filename(), "broken.joyeer");
    EXPECT_NE(diagnostics.errors.front().sourceLine.find("return true"), std::string::npos);
}

TEST_F(ModuleServiceTest, RejectsCanonicalDuplicateDirectories) {
    write("root/main.joyeer", "func main() {}\n");
    map("alias", std::filesystem::path("root") / ".");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.duplicate-directory")) << errors();
}

TEST_F(ModuleServiceTest, RejectsDuplicateLogicalNamesInServiceOptions) {
    write("root/main.joyeer", "func main() {}\n");
    map("duplicate", "one");
    map("duplicate", "two");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.duplicate-name")) << errors();
}

TEST_F(ModuleServiceTest, RejectsInvalidLogicalNamesInServiceOptions) {
    write("root/main.joyeer", "func main() {}\n");
    map("bad..name", "dependency");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.invalid-name")) << errors();
}

TEST_F(ModuleServiceTest, RejectsHardLinkedDuplicateSources) {
    const auto source = write("root/main.joyeer", "func main() {}\n");
    map("dependency", "dependency");
    std::error_code error;
    std::filesystem::create_hard_link(source, directory / "dependency" / "alias.joyeer", error);
    if (error) GTEST_SKIP() << "hard links unavailable: " << error.message();
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.duplicate-source")) << errors();
}

TEST_F(ModuleServiceTest, RejectsMissingModuleDirectory) {
    options.modules.push_back({"missing", directory / "does-not-exist"});
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.invalid-directory")) << errors();
}

TEST_F(ModuleServiceTest, RejectsEmptyRootWithoutRecursivelyDiscoveringSources) {
    write("root/nested/main.joyeer", "func main() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("module.empty")) << errors();
}

TEST_F(ModuleServiceTest, ProtectsEvenUnreachableModuleInputsFromIROutput) {
    write("root/main.joyeer", "func main() {}\n");
    const auto text = std::string("public func value(): Int { return 42 }\n");
    const auto protectedFile = write("unused/value.joyeer", text);
    map("unused", "unused");
    options.outputMode = joyeer::OutputMode::llvmIR;
    options.outputFile = protectedFile;
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    EXPECT_TRUE(hasCode("driver.output-file-error")) << errors();
    std::ifstream source(protectedFile, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(source), {}), text);
}

TEST_F(ModuleServiceTest, ProtectsHardLinkedInputFromIROutput) {
    const auto source = write("root/main.joyeer", "func main() {}\n");
    options.outputMode = joyeer::OutputMode::llvmIR;
    options.outputFile = directory / "output.ll";
    std::error_code error;
    std::filesystem::create_hard_link(source, options.outputFile, error);
    if (error) GTEST_SKIP() << "hard links unavailable: " << error.message();
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("driver.output-file-error")) << errors();
}

#if defined(_WIN32)
TEST_F(ModuleServiceTest, ProtectsInputAliasedBySiblingPDBCleanup) {
    const auto source = write("root/main.joyeer", "func main() {}\n");
    options.outputMode = joyeer::OutputMode::executable;
    options.outputFile = directory / "program.exe";
    std::error_code error;
    std::filesystem::create_hard_link(source, directory / "program.pdb", error);
    if (error) GTEST_SKIP() << "hard links unavailable: " << error.message();
    CompilerService compiler(&diagnostics, options);
    compiler.compile();
    EXPECT_TRUE(hasCode("driver.output-file-error")) << errors();
}
#endif

TEST_F(ModuleServiceTest, EmitsWholeGraphIRToTheRequestedOutput) {
    write("root/main.joyeer", "import dependency\nfunc main() { print(value: dependency.answer()) }\n");
    write("dependency/answer.joyeer", "public func answer(): Int { return 42 }\n");
    map("dependency", "dependency");
    options.outputMode = joyeer::OutputMode::llvmIR;
    options.outputFile = directory / "output.ll";
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    std::ifstream output(options.outputFile, std::ios::binary);
    const std::string ir(std::istreambuf_iterator<char>(output), {});
    EXPECT_EQ(ir, compiler.getLastCompiledSourceFile()->llvmIR);
    EXPECT_FALSE(ir.empty());
}

TEST_F(ModuleServiceTest, NativeOutputSelectsOnlyRootModuleMain) {
    write("root/main.joyeer", "import dependency\nfunc main() { print(value: dependency.answer()) }\n");
    write("dependency/answer.joyeer",
            "public func answer(): Int { return 42 }\n"
            "public func main(value: Int): Bool { return true }\n");
    map("dependency", "dependency");
    options.outputMode = joyeer::OutputMode::executable;
    options.outputFile = directory / "program";
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    const auto& result = compiler.getLastCompiledSourceFile();
    ASSERT_NE(result->joyeerIR, nullptr);
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
    write("root/helper.joyeer", "import dependency\nfunc helper() {}\n");
    write("dependency/main.joyeer", "public func main() {}\n");
    map("dependency", "dependency");
    options.outputMode = joyeer::OutputMode::executable;
    options.outputFile = directory / "program";
    CompilerService compiler(&diagnostics, options);
    compiler.compile();

    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    EXPECT_FALSE(compiler.getLastCompiledSourceFile()->llvmHasEntryPoint);
}

TEST_F(ModuleServiceTest, PreservesSingleFileCompilation) {
    const auto file = write("single.joyeer", "func main() {}\n");
    options.moduleRoot.clear();
    CompilerService compiler(&diagnostics, options);
    compiler.compile(file);
    ASSERT_FALSE(diagnostics.hasFailure()) << errors();
    ASSERT_NE(compiler.getLastCompiledSourceFile(), nullptr);
    EXPECT_TRUE(compiler.getLastCompiledSourceFile()->llvmHasEntryPoint);
}

TEST_F(ModuleServiceTest, RejectsConflictingServiceInputs) {
    const auto file = write("root/main.joyeer", "func main() {}\n");
    CompilerService compiler(&diagnostics, options);
    compiler.compile(file);
    EXPECT_TRUE(hasCode("module.conflicting-input")) << errors();
}

} // namespace
