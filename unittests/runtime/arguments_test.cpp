#include "joyeer/compiler/options.h"
#include "joyeer/runtime/arguments.h"

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

std::vector<std::string> nativeArguments(const std::vector<std::string>& options) {
    std::vector<std::string> result {
        "joyeer",
        "--lang=v0.1",
    };
    result.insert(result.end(), options.begin(), options.end());
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
    EXPECT_FALSE(arguments->debugInfo.emitLineTables);
    EXPECT_EQ(arguments->debugInfo.format, joyeer::defaultDebugInfoFormat());
}

TEST(CommandLineArgumentsTest, ParsesDebugLineTableOptions) {
    const std::vector<std::pair<std::string, joyeer::DebugInfoFormat>> cases {
        { "-g", joyeer::defaultDebugInfoFormat() },
        { "-gline-tables-only", joyeer::defaultDebugInfoFormat() },
        { "-gdwarf", joyeer::DebugInfoFormat::dwarf },
#if defined(_WIN32)
        { "-gcodeview", joyeer::DebugInfoFormat::codeView },
#endif
    };
    for (const auto& [option, expectedFormat] : cases) {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(
                diagnostics,
                nativeArguments(std::vector<std::string> { option }));
        EXPECT_FALSE(diagnostics.hasFailure()) << option;
        EXPECT_TRUE(arguments->debugInfo.emitLineTables) << option;
        EXPECT_EQ(arguments->debugInfo.format, expectedFormat) << option;
    }
}

TEST(CommandLineArgumentsTest, AppliesDebugLevelAndFormatOptionsInOrder) {
    {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(
                diagnostics,
                nativeArguments(std::vector<std::string> {
                    "-gdwarf", "-g0", "-gline-tables-only",
                }));
        EXPECT_FALSE(diagnostics.hasFailure());
        EXPECT_TRUE(arguments->debugInfo.emitLineTables);
        EXPECT_EQ(arguments->debugInfo.format, joyeer::DebugInfoFormat::dwarf);
    }
    {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(
                diagnostics,
                nativeArguments(std::vector<std::string> { "-g", "-g0" }));
        EXPECT_FALSE(diagnostics.hasFailure());
        EXPECT_FALSE(arguments->debugInfo.emitLineTables);
    }
}

TEST(CommandLineArgumentsTest, RejectsUnknownDebugOptions) {
    Diagnostics diagnostics;
    static_cast<void>(parseArguments(
            diagnostics,
            nativeArguments(std::vector<std::string> { "-g3" })));

    ASSERT_TRUE(diagnostics.hasFailure());
    ASSERT_FALSE(diagnostics.errors.empty());
    EXPECT_NE(
            diagnostics.errors[0].message.find("unsupported debug option"),
            std::string::npos);
}

        TEST(CommandLineArgumentsTest, RejectsUnknownOptionsAndMultipleInputs) {
            Diagnostics diagnostics;
            auto values = nativeArguments(std::vector<std::string> { "--gdwarf" });
            values.push_back(std::filesystem::path(__FILE__).string());
            static_cast<void>(parseArguments(diagnostics, std::move(values)));

            ASSERT_TRUE(diagnostics.hasFailure());
            EXPECT_TRUE(std::any_of(
                diagnostics.errors.begin(),
                diagnostics.errors.end(),
                [](const auto& error) {
                return error.message.find("unknown option '--gdwarf'") !=
                    std::string::npos;
                }));
            EXPECT_TRUE(std::any_of(
                diagnostics.errors.begin(),
                diagnostics.errors.end(),
                [](const auto& error) {
                return error.message.find("multiple input files") !=
                    std::string::npos;
                }));
        }

        TEST(CommandLineArgumentsTest, KeepsFailuresWhenNoInputWasProvided) {
            Diagnostics diagnostics;
            const auto arguments = parseArguments(
                diagnostics,
                { "joyeer", "--lang=v0.1", "-g3" });

            EXPECT_TRUE(diagnostics.hasFailure());
            EXPECT_FALSE(arguments->accepted);
            ASSERT_EQ(diagnostics.errors.size(), 1u);
            EXPECT_NE(
                    diagnostics.errors[0].message.find("unsupported debug option"),
                    std::string::npos);
        }

#if !defined(_WIN32)
TEST(CommandLineArgumentsTest, RejectsEnabledCodeViewOutsideWindows) {
    Diagnostics diagnostics;
    static_cast<void>(parseArguments(
            diagnostics,
            nativeArguments(std::vector<std::string> { "-gcodeview" })));

    ASSERT_TRUE(diagnostics.hasFailure());
}
#endif

TEST(CommandLineArgumentsTest, RejectsEnabledDebugInfoForLegacyMode) {
    Diagnostics diagnostics;
    std::vector<std::string> values {
        "joyeer",
        "-g",
        std::filesystem::path(__FILE__).string(),
    };
    static_cast<void>(parseArguments(diagnostics, std::move(values)));

    ASSERT_TRUE(diagnostics.hasFailure());
    EXPECT_TRUE(std::any_of(
            diagnostics.errors.begin(),
            diagnostics.errors.end(),
            [](const auto& error) {
                return error.message.find("debug information requires --lang=v0.1") !=
                        std::string::npos;
            }));
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
