#include "joyeer/compiler/options.h"
#include "joyeer/runtime/arguments.h"

#include <gtest/gtest.h>

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

std::vector<std::string> nativeArguments(const std::string& optimization = {}) {
    std::vector<std::string> result {
        "joyeer",
        "--lang=v0.1",
    };
    if (!optimization.empty()) result.push_back(optimization);
    result.push_back("-o");
    result.push_back(
            (std::filesystem::temp_directory_path() / "joyeer-options-test").string());
    result.push_back(std::filesystem::path(__FILE__).string());
    return result;
}

TEST(CommandLineArgumentsTest, DefaultsNativeCompilationToO2) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, nativeArguments());

    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_EQ(arguments->optimizationLevel, joyeer::OptimizationLevel::O2);
}

TEST(CommandLineArgumentsTest, ParsesEverySupportedOptimizationLevel) {
    const std::vector<std::pair<std::string, joyeer::OptimizationLevel>> cases {
        { "-O0", joyeer::OptimizationLevel::O0 },
        { "-O1", joyeer::OptimizationLevel::O1 },
        { "-O2", joyeer::OptimizationLevel::O2 },
        { "-O3", joyeer::OptimizationLevel::O3 },
    };
    for (const auto& [option, expected] : cases) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(diagnostics, nativeArguments(option));
        EXPECT_FALSE(diagnostics.hasFailure()) << option;
        EXPECT_EQ(arguments->optimizationLevel, expected) << option;
    }
}

TEST(CommandLineArgumentsTest, RejectsUnknownOptimizationLevels) {
    Diagnostics diagnostics;
    static_cast<void>(parseArguments(diagnostics, nativeArguments("-O4")));

    ASSERT_TRUE(diagnostics.hasFailure());
    ASSERT_FALSE(diagnostics.errors.empty());
    EXPECT_NE(
            diagnostics.errors[0].message.find("unsupported optimization level"),
            std::string::npos);
}

TEST(OptimizationOptionsTest, MapsEveryLevelToAClangFlag) {
    EXPECT_STREQ(joyeer::optimizationFlag(joyeer::OptimizationLevel::O0), "-O0");
    EXPECT_STREQ(joyeer::optimizationFlag(joyeer::OptimizationLevel::O1), "-O1");
    EXPECT_STREQ(joyeer::optimizationFlag(joyeer::OptimizationLevel::O2), "-O2");
    EXPECT_STREQ(joyeer::optimizationFlag(joyeer::OptimizationLevel::O3), "-O3");
}

} // namespace
