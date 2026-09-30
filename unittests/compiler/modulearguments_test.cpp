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
    raw.reserve(values.size());
    for (auto& value : values) raw.push_back(value.data());
    return std::make_unique<CommandLineArguments>(
            &diagnostics,
            static_cast<int>(raw.size()),
            raw.data());
}

TEST(ModuleArgumentsTest, AcceptsModuleRootAndRepeatedLogicalMappings) {
    Diagnostics diagnostics;
    const auto root = std::filesystem::path(__FILE__).parent_path();
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module", "project.config=" + root.string(),
        "--module-root", root.string(), "--module", "project.io=" + root.string(),
        "-O0", "--emit-llvm", "module-output.ll",
    });

    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->accepted);
    EXPECT_TRUE(arguments->inputfile.empty());
    EXPECT_EQ(arguments->moduleRoot, std::filesystem::weakly_canonical(root));
    EXPECT_EQ(arguments->workingDirectory, arguments->moduleRoot);
    ASSERT_EQ(arguments->modules.size(), 2u);
    EXPECT_EQ(arguments->modules[0].name, "project.config");
    EXPECT_EQ(arguments->modules[1].name, "project.io");
    EXPECT_EQ(arguments->outputMode, joyeer::OutputMode::llvmIR);
    EXPECT_EQ(arguments->optimizationLevel, joyeer::OptimizationLevel::O0);
}

TEST(ModuleArgumentsTest, RejectsModuleRootWithPositionalSourceInEitherOrder) {
    const auto file = std::filesystem::path(__FILE__);
    for (const auto& values : std::vector<std::vector<std::string>> {
            {"joyeer", "--module-root", file.parent_path().string(), file.string()},
            {"joyeer", file.string(), "--module-root", file.parent_path().string()}}) {
        Diagnostics diagnostics;
        static_cast<void>(parseArguments(diagnostics, values));
        EXPECT_TRUE(diagnostics.hasFailure());
        EXPECT_TRUE(std::any_of(diagnostics.errors.begin(), diagnostics.errors.end(),
                [](const auto& error) { return error.message.find("mutually exclusive") != std::string::npos; }));
    }
}

TEST(ModuleArgumentsTest, RejectsMalformedOrDuplicateModuleMappings) {
    const auto root = std::filesystem::path(__FILE__).parent_path().string();
    for (const auto& mapping : {
            "", "=directory", "name=", "name", ".name=directory", "name.=directory",
            "name..other=directory", "1name=directory", "name-with-dash=directory"}) {
        Diagnostics diagnostics;
        static_cast<void>(parseArguments(diagnostics, {
            "joyeer", "--module-root", root, "--module", mapping,
        }));
        EXPECT_TRUE(diagnostics.hasFailure()) << mapping;
    }
    Diagnostics diagnostics;
    static_cast<void>(parseArguments(diagnostics, {
        "joyeer", "--module-root", root, "--module", "project=" + root,
        "--module", "project=" + root,
    }));
    EXPECT_TRUE(diagnostics.hasFailure());
}

TEST(ModuleArgumentsTest, RejectsModuleMappingWithoutRoot) {
    Diagnostics diagnostics;
    static_cast<void>(parseArguments(diagnostics, {
        "joyeer", "--module", "project=directory", std::filesystem::path(__FILE__).string(),
    }));
    EXPECT_TRUE(diagnostics.hasFailure());
}

TEST(ModuleArgumentsTest, RejectsMissingModuleOptionValuesAndRepeatedRoot) {
    const auto root = std::filesystem::path(__FILE__).parent_path().string();
    for (const auto& values : std::vector<std::vector<std::string>> {
            {"joyeer", "--module-root"},
            {"joyeer", "--module"},
            {"joyeer", "--module-root", "--module", "project=directory"},
            {"joyeer", "--module-root", root, "--module-root", root},
            {"joyeer", "--module-root", std::filesystem::path(__FILE__).string()}}) {
        Diagnostics diagnostics;
        static_cast<void>(parseArguments(diagnostics, values));
        EXPECT_TRUE(diagnostics.hasFailure());
    }
}

} // namespace
