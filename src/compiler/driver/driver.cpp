#include "driver.h"
#include "compiler/codegen/linker.h"
#include "toolchain.h"

#include <filesystem>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <vector>

#ifndef JOYEER_RUNTIME_PATH
#define JOYEER_RUNTIME_PATH ""
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

std::filesystem::path runtimePath() {
    const auto configuredPath = std::filesystem::u8path(JOYEER_RUNTIME_PATH);
    if (configuredPath.is_relative()) {
        const auto directory = joyeer::executableDirectory();
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
                arguments->moduleName,
                arguments->sourceFiles,
                arguments->modules,
                arguments->runtimeModules,
                arguments->coreModules,
                arguments->nativeLibraries,
                joyeer::StandardLibrary::libraryDirectoryFor(arguments->sysroot),
                arguments->standardLibrary,
            });
}

int Driver::run() {
    compiler->compile(arguments->inputfile);
    if(diagnostics->hasFailure()) {
        diagnostics->printErrors();
        return 1;
    }
    if(arguments->outputMode == joyeer::OutputMode::executable ||
       arguments->outputMode == joyeer::OutputMode::runtimeObject) {
        const auto& source = compiler->getLastCompiledSourceFile();
        if (source == nullptr) {
            diagnostics->reportDiagnostic(
                    ErrorLevel::failure,
                    "driver.missing-source-result",
                    "compiler produced no source result");
        } else {
            // Standard modules that were actually imported bring their native archives.
            auto linkLibraries = arguments->nativeLibraries;
            for (const auto& library : compiler->implicitNativeLibraries()) {
                if (std::find(linkLibraries.begin(), linkLibraries.end(), library) == linkLibraries.end()) {
                    linkLibraries.push_back(library);
                }
            }
            const joyeer::native::LinkOptions options {
                        runtimePath(),
                        std::filesystem::u8path(JOYEER_SDK_ROOT_PATH),
                        JOYEER_SDK_VERSION,
                        JOYEER_MACOS_DEPLOYMENT_TARGET,
                        std::filesystem::u8path(JOYEER_DSYMUTIL_EXECUTABLE_PATH),
                        arguments->outputFile,
                        arguments->optimizationLevel,
                        arguments->debugInfo,
                        source->getAbstractPath(),
                        source->llvmModule,
                        linkLibraries,
                    };
            const auto linking = arguments->outputMode == joyeer::OutputMode::runtimeObject
                    ? joyeer::native::Linker().emitObject(source->llvmIR, options)
                    : joyeer::native::Linker().link(source->llvmIR, source->llvmHasEntryPoint, options);
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
