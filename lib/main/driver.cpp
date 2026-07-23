#include "driver.h"
#include "joyeer/backend/linker.h"

#ifndef JOYEER_CLANG_EXECUTABLE_PATH
#define JOYEER_CLANG_EXECUTABLE_PATH ""
#endif

#ifndef JOYEER_NATIVE_RUNTIME_PATH
#define JOYEER_NATIVE_RUNTIME_PATH ""
#endif

#ifndef JOYEER_SDK_ROOT_PATH
#define JOYEER_SDK_ROOT_PATH ""
#endif

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
                        JOYEER_NATIVE_RUNTIME_PATH,
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
