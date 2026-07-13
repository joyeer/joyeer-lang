#include "driver.h"
#include "joyeer/vm/isolate.h"

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
    if(!diagnostics->errors.empty()) {
        diagnostics->printErrors();
        return 1;
    } else if(arguments->languageMode == LanguageMode::legacy) {
        ((InterpretedIsolatedVM*)vm)->run(module);
    }
    return 0;
}
