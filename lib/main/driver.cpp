#include "driver.h"
#include "joyeer/backend/linker.h"

#include <filesystem>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <vector>

#ifndef JOYEER_NATIVE_RUNTIME_PATH
#define JOYEER_NATIVE_RUNTIME_PATH ""
#endif

#ifndef JOYEER_SDK_ROOT_PATH
#define JOYEER_SDK_ROOT_PATH ""
#endif

#ifndef JOYEER_SDK_VERSION
#define JOYEER_SDK_VERSION ""
#endif

#ifndef JOYEER_MACOS_DEPLOYMENT_TARGET
#define JOYEER_MACOS_DEPLOYMENT_TARGET ""
#endif

#ifndef JOYEER_DSYMUTIL_EXECUTABLE_PATH
#define JOYEER_DSYMUTIL_EXECUTABLE_PATH ""
#endif

namespace {

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::wstring executablePath(32768, L'\0');
    const auto length = GetModuleFileNameW(
            nullptr,
            executablePath.data(),
            static_cast<DWORD>(executablePath.size()));
    if (length > 0 && length < executablePath.size()) {
        executablePath.resize(length);
        return std::filesystem::path(executablePath).parent_path();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    std::vector<char> executablePath(size);
    if (size != 0 && _NSGetExecutablePath(executablePath.data(), &size) == 0) {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(
                executablePath.data(),
                error);
        return (error ? std::filesystem::path(executablePath.data()) : resolved)
                .parent_path();
    }
#elif defined(__linux__)
    std::vector<char> executablePath(4096);
    while (true) {
    const auto length = readlink(
                "/proc/self/exe",
                executablePath.data(),
                executablePath.size());
        if (length < 0) break;
        if (static_cast<size_t>(length) < executablePath.size()) {
            return std::filesystem::path(
                    std::string(executablePath.data(), static_cast<size_t>(length)))
                    .parent_path();
        }
        executablePath.resize(executablePath.size() * 2);
    }
#endif
    return {};
}

std::filesystem::path nativeRuntimePath() {
    const std::filesystem::path configuredPath = JOYEER_NATIVE_RUNTIME_PATH;
    if (configuredPath.is_relative()) {
        const auto directory = executableDirectory();
        if (!directory.empty()) return directory / configuredPath;
    }
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
                        nativeRuntimePath(),
                        JOYEER_SDK_ROOT_PATH,
                        JOYEER_SDK_VERSION,
                        JOYEER_MACOS_DEPLOYMENT_TARGET,
                        JOYEER_DSYMUTIL_EXECUTABLE_PATH,
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
