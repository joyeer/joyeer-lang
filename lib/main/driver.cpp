#include "driver.h"
#include "joyeer/backend/linker.h"

#include <filesystem>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#ifndef JOYEER_CLANG_EXECUTABLE_PATH
#define JOYEER_CLANG_EXECUTABLE_PATH ""
#endif

#ifndef JOYEER_NATIVE_RUNTIME_PATH
#define JOYEER_NATIVE_RUNTIME_PATH ""
#endif

#ifndef JOYEER_SDK_ROOT_PATH
#define JOYEER_SDK_ROOT_PATH ""
#endif

namespace {

std::filesystem::path nativeRuntimePath() {
    const std::filesystem::path configuredPath = JOYEER_NATIVE_RUNTIME_PATH;
#if defined(_WIN32)
    if (configuredPath.is_relative()) {
        std::wstring executablePath(32768, L'\0');
        const auto length = GetModuleFileNameW(
                nullptr,
                executablePath.data(),
                static_cast<DWORD>(executablePath.size()));
        if (length > 0 && length < executablePath.size()) {
            executablePath.resize(length);
            return std::filesystem::path(executablePath).parent_path() /
                    configuredPath;
        }
    }
#endif
    return configuredPath;
}

} // namespace

Driver::Driver(Diagnostics* diagnostics, CommandLineArguments::Ptr arguments):arguments(arguments) {
    this->diagnostics = diagnostics;
    compiler = new CompilerService(
            diagnostics,
            joyeer::CompileOptions {
                arguments->workingDirectory,
                arguments->outputFile,
                arguments->outputMode,
                arguments->optimizationLevel,
                arguments->debugInfo,
            });
}

int Driver::run() {
    compiler->compile(arguments->inputfile);
    if(diagnostics->hasFailure()) {
        diagnostics->printErrors();
        return 1;
    }
    if(arguments->outputMode == joyeer::OutputMode::executable) {
        const auto& source = compiler->getLastCompiledSourceFile();
        if (source == nullptr) {
            diagnostics->reportDiagnostic(
                    ErrorLevel::failure,
                    "driver.missing-source-result",
                    "compiler produced no source result");
        } else {
            const auto linking = joyeer::native::Linker().link(
                    source->llvmIR,
                    source->llvmHasEntryPoint,
                    joyeer::native::LinkOptions {
                        JOYEER_CLANG_EXECUTABLE_PATH,
                        nativeRuntimePath(),
                        JOYEER_SDK_ROOT_PATH,
                        arguments->outputFile,
                        arguments->optimizationLevel,
                        arguments->debugInfo,
                        arguments->inputfile,
                    });
            for (const auto& diagnostic : linking.diagnostics) {
                        diagnostics->reportDiagnostic(
                        ErrorLevel::failure,
                        joyeer::native::diagnosticName(diagnostic.id),
                            diagnostic.message);
            }
        }
        if (diagnostics->hasFailure()) {
            diagnostics->printErrors();
            return 1;
        }
    }
    if (!diagnostics->errors.empty()) diagnostics->printErrors();
    return 0;
}
