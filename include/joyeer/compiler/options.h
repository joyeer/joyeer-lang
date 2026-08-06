#ifndef __joyeer_compiler_options_h__
#define __joyeer_compiler_options_h__

#include <filesystem>

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

struct CompileOptions {
    std::filesystem::path workingDirectory;
    std::filesystem::path outputFile;
    OutputMode outputMode = OutputMode::validate;
    OptimizationLevel optimizationLevel = OptimizationLevel::O2;
    DebugInfoOptions debugInfo;
};

} // namespace joyeer

#endif
