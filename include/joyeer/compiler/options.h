#ifndef __joyeer_compiler_options_h__
#define __joyeer_compiler_options_h__

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

[[nodiscard]] constexpr const char* optimizationFlag(OptimizationLevel level) {
    switch (level) {
        case OptimizationLevel::O0: return "-O0";
        case OptimizationLevel::O1: return "-O1";
        case OptimizationLevel::O2: return "-O2";
        case OptimizationLevel::O3: return "-O3";
    }
    return "-O2";
}

} // namespace joyeer

#endif
