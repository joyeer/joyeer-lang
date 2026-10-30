#ifndef JOYEER_COMPILER_NATIVE_BINDING_H
#define JOYEER_COMPILER_NATIVE_BINDING_H

#include "token.h"

#include <string>
#include <string_view>

namespace joyeer {

enum class NativeBindingKind {
    cImport,
    cExport,
};

// The source declaration owns the signature; this adds only native linkage.
struct NativeBinding {
    NativeBindingKind kind = NativeBindingKind::cImport;
    std::string symbol;
    SourceSpan span;
};

[[nodiscard]] inline bool isCIdentifier(std::string_view name) {
    if (name.empty()) return false;
    for (size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            character == '_';
        if (!letter && (index == 0 || character < '0' || character > '9')) return false;
    }
    return true;
}

[[nodiscard]] inline bool isCEntrySymbol(std::string_view name) {
    return name == "main" || name == "wmain" || name == "WinMain" ||
           name == "wWinMain" || name == "_start" ||
           name == "mainCRTStartup" || name == "wmainCRTStartup" ||
           name == "WinMainCRTStartup" || name == "wWinMainCRTStartup" ||
           name == "DllMain" || name == "DllMainCRTStartup" ||
           name == "_DllMainCRTStartup" ||
           name == "joyeer_main" || name == "joyeer_main_uses_arguments";
}

} // namespace joyeer

#endif
