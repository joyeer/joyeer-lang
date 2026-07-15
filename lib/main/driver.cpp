#include "driver.h"
#include "joyeer/backend/linker.h"
#include "joyeer/vm/isolate.h"

#ifndef JOYEER_CLANG_EXECUTABLE_PATH
#define JOYEER_CLANG_EXECUTABLE_PATH ""
#endif

#ifndef JOYEER_NATIVE_RUNTIME_PATH
#define JOYEER_NATIVE_RUNTIME_PATH ""
#endif

Driver::Driver(Diagnostics* diagnostics, CommandLineArguments::Ptr arguments):arguments(arguments) {
    this->diagnostics = diagnostics;
    compiler = new CompilerService(diagnostics, arguments);

    if(arguments->languageMode == LanguageMode::legacy) {
        vm = new InterpretedIsolatedVM();
        compiler->strings = vm->strings;
        compiler->types = vm->types;
        compiler->bootstrap();
        vm->bootstrap();
    }
}

int Driver::run() {
    auto module = compiler->compile(arguments->inputfile.string());
    if(diagnostics->hasFailure()) {
        diagnostics->printErrors();
        return 1;
    }
    if (!diagnostics->errors.empty()) diagnostics->printErrors();
    if(arguments->languageMode == LanguageMode::v0_1 &&
              arguments->outputMode == OutputMode::executable) {
        const auto& source = compiler->getLastCompiledSourceFile();
        if (source == nullptr) {
            diagnostics->reportError(ErrorLevel::failure, "compiler produced no source result");
        } else {
            const auto linking = joyeer::native::Linker().link(
                    source->llvmIR,
                    source->llvmHasEntryPoint,
                    joyeer::native::LinkOptions {
                        JOYEER_CLANG_EXECUTABLE_PATH,
                        JOYEER_NATIVE_RUNTIME_PATH,
                        arguments->outputFile,
                        arguments->optimizationLevel,
                    });
            for (const auto& diagnostic : linking.diagnostics) {
                diagnostics->reportError(
                        ErrorLevel::failure,
                        "%s: %s",
                        joyeer::native::diagnosticName(diagnostic.id),
                        diagnostic.message.c_str());
            }
        }
        if (diagnostics->hasFailure()) {
            diagnostics->printErrors();
            return 1;
        }
    } else if(arguments->languageMode == LanguageMode::legacy) {
        ((InterpretedIsolatedVM*)vm)->run(module);
    }
    return 0;
}
