#include "joyeer/main/arguments.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<CommandLineArguments> parseArguments(
        Diagnostics& diagnostics,
        std::vector<std::string> values) {
    std::vector<char*> raw;
    for (auto& value : values) raw.push_back(value.data());
    return std::make_unique<CommandLineArguments>(
            &diagnostics, static_cast<int>(raw.size()), raw.data());
}

std::filesystem::path source(const char* name = "modulearguments_test.cpp") {
    return std::filesystem::weakly_canonical(std::filesystem::path(__FILE__).parent_path() / name);
}

TEST(ModuleArgumentsTest, CollectsNamedRootAndRepeatedDependencySources) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module-source", "project.config=" + source("module_test.cpp").string(),
        source().string(), "--module-name", "project.app",
        "--module-source", "project.io=" + source("ir_test.cpp").string(),
        source("moduleservice.cpp").string(),
        "--module-source", "project.config=" + source("llvm_test.cpp").string(),
        "-O0", "--emit-llvm", "module-output.ll",
    });

    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->accepted);
    EXPECT_TRUE(arguments->inputfile.empty());
    EXPECT_EQ(arguments->moduleName, "project.app");
    EXPECT_EQ(arguments->workingDirectory, std::filesystem::current_path());
    EXPECT_EQ(arguments->sourceFiles, (std::vector {source(), source("moduleservice.cpp")}));
    ASSERT_EQ(arguments->modules.size(), 2u);
    EXPECT_EQ(arguments->modules[0].name, "project.config");
    EXPECT_EQ(arguments->modules[0].files,
              (std::vector {source("module_test.cpp"), source("llvm_test.cpp")}));
    EXPECT_EQ(arguments->modules[1].name, "project.io");
    EXPECT_EQ(arguments->modules[1].files, (std::vector {source("ir_test.cpp")}));
    EXPECT_EQ(arguments->outputMode, joyeer::OutputMode::llvmIR);
    EXPECT_EQ(arguments->optimizationLevel, joyeer::OptimizationLevel::O0);
}

TEST(ModuleArgumentsTest, ResolvesSourcePathsAgainstCompilerWorkingDirectory) {
    Diagnostics diagnostics;
    const auto relative = std::filesystem::relative(source(), std::filesystem::current_path());
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module-name", "project.app", relative.string(),
    });
    ASSERT_FALSE(diagnostics.hasFailure());
    ASSERT_EQ(arguments->sourceFiles.size(), 1u);
    EXPECT_EQ(arguments->sourceFiles[0], source());
}

TEST(ModuleArgumentsTest, PreservesLegacySingleFileInput) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, {"joyeer", source().string()});
    ASSERT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->accepted);
    EXPECT_EQ(arguments->inputfile, source());
    EXPECT_TRUE(arguments->moduleName.empty());
    EXPECT_TRUE(arguments->sourceFiles.empty());
}

TEST(ModuleArgumentsTest, RejectsEmptyPositionalSourcePaths) {
    for (const auto& values : std::vector<std::vector<std::string>> {
            {"joyeer", ""},
            {"joyeer", "--module-name", "app", ""},
        }) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, values);
        EXPECT_TRUE(diagnostics.hasFailure());
        EXPECT_FALSE(arguments->accepted);
    }
}

TEST(ModuleArgumentsTest, RequiresNameForMultipleSourcesAndDependencies) {
    for (const auto& values : std::vector<std::vector<std::string>> {
            {"joyeer", source().string(), source("module_test.cpp").string()},
            {"joyeer", source().string(), "--module-source", "project=" + source("module_test.cpp").string()},
        }) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, values);
        EXPECT_TRUE(diagnostics.hasFailure());
        EXPECT_FALSE(arguments->accepted);
    }
}

TEST(ModuleArgumentsTest, RejectsMissingAndMalformedModuleOptionValues) {
    for (const auto& values : std::vector<std::vector<std::string>> {
            {"joyeer", "--module-name"},
            {"joyeer", "--module-source"},
            {"joyeer", "--module-name", ""},
            {"joyeer", "--module-name", "app"},
            {"joyeer", "--module-name", "app", "--module-name", "app", source().string()},
            {"joyeer", "--module-name", "--module-source", "project=missing"},
        }) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, values);
        EXPECT_TRUE(diagnostics.hasFailure());
        EXPECT_FALSE(arguments->accepted);
    }
    for (const auto* name : {"", ".name", "name.", "name..other", "1name", "name-with-dash"}) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, {
            "joyeer", "--module-name", name, source().string(),
        });
        EXPECT_TRUE(diagnostics.hasFailure()) << name;
        EXPECT_FALSE(arguments->accepted);
    }
    for (const auto* mapping : {"", "=file", "name=", "name", ".name=file", "name..other=file"}) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, {
            "joyeer", "--module-name", "app", source().string(), "--module-source", mapping,
        });
        EXPECT_TRUE(diagnostics.hasFailure()) << mapping;
        EXPECT_FALSE(arguments->accepted);
    }
}

TEST(ModuleArgumentsTest, RejectsDuplicateFilesWithinAndAcrossCompilationUnits) {
    for (const auto& values : std::vector<std::vector<std::string>> {
            {"joyeer", "--module-name", "app", source().string(), source().string()},
            {"joyeer", "--module-name", "app", source().string(), "--module-source", "dep=" + source().string()},
            {"joyeer", "--module-name", "app", source().string(),
             "--module-source", "dep=" + source("module_test.cpp").string(),
             "--module-source", "dep=" + source("module_test.cpp").string()},
        }) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, values);
        EXPECT_FALSE(arguments->accepted);
        EXPECT_TRUE(std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(),
                [](const auto& error) {
                    return error.message.find("duplicate source file") != std::string::npos;
                }));
    }
}

TEST(ModuleArgumentsTest, RejectsDependencyUsingRootNameInEitherOptionOrder) {
    for (const bool rootFirst : {true, false}) {
        std::vector<std::string> values {"joyeer", source().string()};
        const std::vector<std::string> root {"--module-name", "app"};
        const std::vector<std::string> dependency {"--module-source", "app=" + source("module_test.cpp").string()};
        const auto& first = rootFirst ? root : dependency;
        const auto& second = rootFirst ? dependency : root;
        values.insert(values.end(), first.begin(), first.end());
        values.insert(values.end(), second.begin(), second.end());
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, values);
        EXPECT_TRUE(diagnostics.hasFailure());
        EXPECT_FALSE(arguments->accepted);
    }
}

TEST(ModuleArgumentsTest, RejectsDirectoryAndMissingFileInputs) {
    for (const auto& input : {source().parent_path(), source("absent-unit-test.joyeer")}) {
        for (const bool dependency : {true, false}) {
            Diagnostics diagnostics;
            std::vector<std::string> values {"joyeer", "--module-name", "app"};
            if (dependency) {
                values.insert(values.end(), {source().string(), "--module-source", "dep=" + input.string()});
            } else {
                values.push_back(input.string());
            }
            const auto arguments = parseArguments(diagnostics, values);
            EXPECT_TRUE(diagnostics.hasFailure());
            EXPECT_FALSE(arguments->accepted);
        }
    }
}

TEST(ModuleArgumentsTest, RejectsRemovedDirectoryOptionsWithMigrationHelp) {
    for (const auto* option : {"--module-root", "--module"}) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, {"joyeer", option, "directory"});
        EXPECT_FALSE(arguments->accepted);
        EXPECT_TRUE(std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(),
                [](const auto& error) {
                    return error.message.find("use --module-name with explicit source files") != std::string::npos;
                }));
    }
}

TEST(ModuleArgumentsTest, ProtectsEverySuppliedSourceFromOutput) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module-name", "app", source().string(),
        "--module-source", "unused=" + source("module_test.cpp").string(),
        "--emit-llvm", source("module_test.cpp").string(),
    });
    EXPECT_FALSE(arguments->accepted);
    EXPECT_TRUE(std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(),
            [](const auto& error) {
                return error.message.find("must not overwrite") != std::string::npos;
            }));
}

TEST(ModuleArgumentsTest, TerminatesOptionParsingBeforeNamedSourceList) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module-name", "app", "--", source().string(), source("module_test.cpp").string(),
    });
    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->accepted);
    EXPECT_EQ(arguments->sourceFiles.size(), 2u);
}

} // namespace
