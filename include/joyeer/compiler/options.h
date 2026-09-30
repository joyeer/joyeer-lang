#ifndef __joyeer_compiler_options_h__
#define __joyeer_compiler_options_h__

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace joyeer {

enum class OptimizationLevel {
    O0,
    O1,
    O2,
    O3,
};

enum class DebugInfoFormat {
    dwarf,
    codeView,
};

[[nodiscard]] constexpr DebugInfoFormat defaultDebugInfoFormat() {
#if defined(_WIN32)
    return DebugInfoFormat::codeView;
#else
    return DebugInfoFormat::dwarf;
#endif
}

struct DebugInfoOptions {
    bool emitLineTables = false;
    DebugInfoFormat format = defaultDebugInfoFormat();
    bool emitVariables = false;
};

enum class OutputMode {
    validate,
    llvmIR,
    executable,
};

struct ModuleMapping {
    std::string name;
    std::filesystem::path directory;
};

[[nodiscard]] inline bool isModuleName(std::string_view name) {
    bool start = true;
    for (const char character : name) {
        if (character == '.') {
            if (start) return false;
            start = true;
            continue;
        }
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            character == '_';
        if (!letter && (start || character < '0' || character > '9')) return false;
        start = false;
    }
    return !start;
}

struct CompileOptions {
    std::filesystem::path workingDirectory;
    std::filesystem::path outputFile;
    OutputMode outputMode = OutputMode::validate;
    OptimizationLevel optimizationLevel = OptimizationLevel::O2;
    DebugInfoOptions debugInfo;
    std::filesystem::path moduleRoot;
    std::vector<ModuleMapping> modules;
};

} // namespace joyeer

#endif
