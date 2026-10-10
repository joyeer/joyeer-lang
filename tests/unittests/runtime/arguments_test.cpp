#include "joyeer/compiler/options.h"
#include "joyeer/main/arguments.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

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

#if defined(_WIN32)
std::unique_ptr<CommandLineArguments> parseWideArguments(
        Diagnostics& diagnostics,
        std::vector<std::wstring> values) {
    std::vector<wchar_t*> raw;
    raw.reserve(values.size());
    for (auto& value : values) raw.push_back(value.data());
    return std::make_unique<CommandLineArguments>(
            &diagnostics, static_cast<int>(raw.size()), raw.data());
}
#endif

class UnicodeCommandLineArgumentsTest : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic<uint64_t> next { 0 };
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
        const auto processId = _getpid();
#else
        const auto processId = getpid();
#endif
        directory = std::filesystem::temp_directory_path() /
            std::filesystem::u8path("joyeer-cli-\xf0\x9f\x98\x80 space-" +
                std::to_string(processId) + "-" + std::to_string(nonce) + "-" +
                std::to_string(next.fetch_add(1)));
        ownsDirectory = std::filesystem::create_directory(directory);
        ASSERT_TRUE(ownsDirectory);
        input = directory / std::filesystem::u8path("source-\xf0\x9f\x98\x80.joyeer");
        dependency = directory / std::filesystem::u8path("dependency-\xf0\x9f\x98\x80.joyeer");
        output = directory / std::filesystem::u8path("output-\xf0\x9f\x98\x80.ll");
        for (const auto& file : { input, dependency }) {
            std::ofstream stream(file, std::ios::binary);
            ASSERT_TRUE(stream.is_open());
            stream << "func main() {}\n";
            ASSERT_TRUE(stream.good());
        }
    }

    void TearDown() override {
        if (!ownsDirectory) return;
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        EXPECT_FALSE(error) << error.message();
    }

    std::filesystem::path directory;
    std::filesystem::path input;
    std::filesystem::path dependency;
    std::filesystem::path output;
    bool ownsDirectory = false;
};

std::vector<std::string> nativeArguments(const std::string& optimization = {}) {
    std::vector<std::string> result {
        "joyeer",
    };
    if (!optimization.empty()) result.push_back(optimization);
    result.push_back("-o");
    result.push_back(
            pathUtf8(std::filesystem::temp_directory_path() / "joyeer-options-test"));
    result.push_back(pathUtf8(std::filesystem::u8path(__FILE__)));
    return result;
}

std::vector<std::string> nativeArguments(const std::vector<std::string>& options) {
    std::vector<std::string> result {
        "joyeer",
    };
    result.insert(result.end(), options.begin(), options.end());
    result.push_back("-o");
    result.push_back(
            pathUtf8(std::filesystem::temp_directory_path() / "joyeer-options-test"));
    result.push_back(pathUtf8(std::filesystem::u8path(__FILE__)));
    return result;
}

TEST_F(UnicodeCommandLineArgumentsTest, DecodesUtf8InputAndBothOutputModes) {
    for (const auto& option : { "-o", "--emit-llvm" }) {
        SCOPED_TRACE(option);
        Diagnostics diagnostics;
        const auto executable = directory / std::filesystem::u8path("joyeer-\xf0\x9f\x98\x80.exe");
        const auto arguments = parseArguments(
                diagnostics, { pathUtf8(executable), option, pathUtf8(output), pathUtf8(input) });
        EXPECT_FALSE(diagnostics.hasFailure());
        EXPECT_TRUE(arguments->accepted);
        EXPECT_EQ(arguments->executableLocation, executable);
        EXPECT_EQ(arguments->inputfile, std::filesystem::weakly_canonical(input));
        EXPECT_EQ(arguments->outputFile, output);
    }
}

TEST_F(UnicodeCommandLineArgumentsTest, DecodesUtf8RootAndDependencySources) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module-name", "unicode.app", pathUtf8(input),
        "--module-source", "unicode.lib=" + pathUtf8(dependency),
        "--emit-llvm", pathUtf8(output),
    });
    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->accepted);
    ASSERT_EQ(arguments->sourceFiles.size(), 1u);
    EXPECT_EQ(arguments->sourceFiles[0], std::filesystem::weakly_canonical(input));
    ASSERT_EQ(arguments->modules.size(), 1u);
    EXPECT_EQ(arguments->modules[0].name, "unicode.lib");
    ASSERT_EQ(arguments->modules[0].files.size(), 1u);
    EXPECT_EQ(arguments->modules[0].files[0], std::filesystem::weakly_canonical(dependency));
    EXPECT_EQ(arguments->outputFile, output);
}

TEST_F(UnicodeCommandLineArgumentsTest, UsesUtf8InMissingSourceDiagnostics) {
    const auto missing = directory / std::filesystem::u8path("missing-\xf0\x9f\x98\x80.joyeer");
    for (const bool named : { false, true }) {
        SCOPED_TRACE(named);
        Diagnostics diagnostics;
        std::vector<std::string> values { "joyeer" };
        if (named) values.insert(values.end(), { "--module-name", "unicode.app" });
        values.push_back(pathUtf8(missing));
        const auto arguments = parseArguments(diagnostics, std::move(values));
        EXPECT_FALSE(arguments->accepted);
        ASSERT_EQ(diagnostics.errors.size(), 1u);
        EXPECT_EQ(diagnostics.errors[0].code, named ? "module.read-source" : "source-file.read-failed");
        EXPECT_NE(diagnostics.errors[0].message.find("missing-\xf0\x9f\x98\x80.joyeer"),
                  std::string::npos);
    }
}

TEST_F(UnicodeCommandLineArgumentsTest, UsesUtf8InDuplicateSourceDiagnostics) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, {
        "joyeer", "--module-name", "unicode.app", pathUtf8(input), pathUtf8(input),
    });
    EXPECT_FALSE(arguments->accepted);
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].code, "module.duplicate-source");
    EXPECT_NE(diagnostics.errors[0].message.find("source-\xf0\x9f\x98\x80.joyeer"),
              std::string::npos);
}

#if defined(_WIN32)
TEST_F(UnicodeCommandLineArgumentsTest, ConvertsWideSingleFileAndOutputPaths) {
    for (const auto& option : { L"-o", L"--emit-llvm" }) {
        Diagnostics diagnostics;
        const auto executable = directory / std::filesystem::u8path("joyeer-\xf0\x9f\x98\x80.exe");
        const auto arguments = parseWideArguments(diagnostics, {
            executable.native(), option, output.native(), input.native(),
        });
        EXPECT_FALSE(diagnostics.hasFailure());
        EXPECT_TRUE(arguments->accepted);
        EXPECT_EQ(arguments->executableLocation, executable);
        EXPECT_EQ(arguments->inputfile, std::filesystem::weakly_canonical(input));
        EXPECT_EQ(arguments->outputFile, output);
    }
}

TEST_F(UnicodeCommandLineArgumentsTest, ConvertsWideRootAndDependencyPaths) {
    Diagnostics diagnostics;
    const auto arguments = parseWideArguments(diagnostics, {
        L"joyeer", L"--module-name", L"unicode.app", input.native(),
        L"--module-source", std::wstring(L"unicode.lib=") + dependency.native(),
        L"--emit-llvm", output.native(),
    });
    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->accepted);
    ASSERT_EQ(arguments->sourceFiles.size(), 1u);
    EXPECT_EQ(arguments->sourceFiles[0], std::filesystem::weakly_canonical(input));
    ASSERT_EQ(arguments->modules.size(), 1u);
    ASSERT_EQ(arguments->modules[0].files.size(), 1u);
    EXPECT_EQ(arguments->modules[0].files[0], std::filesystem::weakly_canonical(dependency));
    EXPECT_EQ(arguments->outputFile, output);
}

TEST(CommandLineArgumentsTest, PreservesSupplementaryUnicodeInWideArgumentDiagnostics) {
    Diagnostics diagnostics;
    const auto arguments = parseWideArguments(diagnostics, {
        L"joyeer", L"--unknown-\U0001f600",
    });
    EXPECT_FALSE(arguments->accepted);
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_NE(diagnostics.errors[0].message.find("--unknown-\xf0\x9f\x98\x80"),
              std::string::npos);
}

TEST(CommandLineArgumentsTest, PreservesEmptyWideArgumentsForParserValidation) {
    Diagnostics diagnostics;
    const auto arguments = parseWideArguments(diagnostics, { L"joyeer", L"" });
    EXPECT_FALSE(arguments->accepted);
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_NE(diagnostics.errors[0].message.find("source file path must not be empty"),
              std::string::npos);
}

TEST(CommandLineArgumentsTest, RejectsInvalidUtf16BeforeParsingWithExistingDiagnosticIds) {
    const std::vector<std::wstring> invalid {
        std::wstring(1, static_cast<wchar_t>(0xd800)),
        std::wstring(1, static_cast<wchar_t>(0xdc00)),
        std::wstring { static_cast<wchar_t>(0xd800), L'x' },
    };
    for (const auto& text : invalid) {
        const std::vector<std::pair<std::vector<std::wstring>, std::string>> cases {
            { { text }, "source-file.read-failed" },
            { { L"joyeer", text }, "source-file.read-failed" },
            { { L"joyeer", L"-o", text }, "driver.output-file-error" },
            { { L"joyeer", L"--emit-llvm", text }, "driver.output-file-error" },
            { { L"joyeer", L"--module-source", std::wstring(L"unicode.lib=") + text },
              "module.read-source" },
        };
        for (const auto& [values, code] : cases) {
            SCOPED_TRACE(code);
            Diagnostics diagnostics;
            const auto arguments = parseWideArguments(diagnostics, values);
            EXPECT_FALSE(arguments->accepted);
            EXPECT_TRUE(arguments->inputfile.empty());
            EXPECT_TRUE(arguments->outputFile.empty());
            EXPECT_TRUE(arguments->modules.empty());
            ASSERT_EQ(diagnostics.errors.size(), 1u);
            EXPECT_EQ(diagnostics.errors[0].code, code);
            EXPECT_NE(diagnostics.errors[0].message.find("invalid Windows command line argument encoding"),
                      std::string::npos);
            EXPECT_EQ(diagnostics.errors[0].message.find("\xef\xbf\xbd"), std::string::npos);
        }
    }
}
#endif

TEST(CommandLineArgumentsTest, DefaultsNativeCompilationToO2) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(diagnostics, nativeArguments());

    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_EQ(arguments->optimizationLevel, joyeer::OptimizationLevel::O2);
    EXPECT_FALSE(arguments->debugInfo.emitLineTables);
    EXPECT_FALSE(arguments->debugInfo.emitVariables);
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
        EXPECT_FALSE(arguments->debugInfo.emitVariables) << option;
        EXPECT_EQ(arguments->debugInfo.format, expectedFormat) << option;
    }
}

TEST(CommandLineArgumentsTest, ParsesFullDebugAndPreservesItAcrossFormatSelection) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(
            diagnostics,
            nativeArguments(std::vector<std::string> {
                "-gfull", "-gdwarf",
            }));

    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->debugInfo.emitLineTables);
    EXPECT_TRUE(arguments->debugInfo.emitVariables);
    EXPECT_EQ(arguments->debugInfo.format, joyeer::DebugInfoFormat::dwarf);
}

TEST(CommandLineArgumentsTest, LineTableOptionDowngradesFullDebugInOrder) {
    Diagnostics diagnostics;
    const auto arguments = parseArguments(
            diagnostics,
            nativeArguments(std::vector<std::string> {
                "-gfull", "-gline-tables-only",
            }));

    EXPECT_FALSE(diagnostics.hasFailure());
    EXPECT_TRUE(arguments->debugInfo.emitLineTables);
    EXPECT_FALSE(arguments->debugInfo.emitVariables);
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
        EXPECT_FALSE(arguments->debugInfo.emitVariables);
        EXPECT_EQ(arguments->debugInfo.format, joyeer::DebugInfoFormat::dwarf);
    }
    {
        Diagnostics diagnostics;
        const auto arguments = parseArguments(
                diagnostics,
                nativeArguments(std::vector<std::string> { "-g", "-g0" }));
        EXPECT_FALSE(diagnostics.hasFailure());
        EXPECT_FALSE(arguments->debugInfo.emitLineTables);
        EXPECT_FALSE(arguments->debugInfo.emitVariables);
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
            values.push_back(pathUtf8(std::filesystem::u8path(__FILE__)));
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
                { "joyeer", "-g3" });

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

TEST(CommandLineArgumentsTest, AcceptsDebugInfoInDefaultMode) {
    Diagnostics diagnostics;
    std::vector<std::string> values {
        "joyeer",
        "-g",
        pathUtf8(std::filesystem::u8path(__FILE__)),
    };
    static_cast<void>(parseArguments(diagnostics, std::move(values)));

    EXPECT_FALSE(diagnostics.hasFailure());
}

TEST(CommandLineArgumentsTest, RejectsRemovedLanguageModes) {
    for (const auto& option : { "--lang=v0.1", "--lang=v0.1-legacy" }) {
        Diagnostics diagnostics;
        auto values = nativeArguments();
        values.insert(values.begin() + 1, option);
        static_cast<void>(parseArguments(diagnostics, std::move(values)));

        ASSERT_TRUE(diagnostics.hasFailure()) << option;
        EXPECT_NE(
                diagnostics.errors[0].message.find("unknown option '" +
                                                   std::string(option) + "'"),
                std::string::npos) << option;
    }
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

} // namespace
