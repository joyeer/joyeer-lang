#include "driver.h"
#include "joyeer/vm/isolate.h"

Driver::Driver(Diagnostics* diagnostics, CommandLineArguments::Ptr arguments):arguments(arguments) {
    this->diagnostics = diagnostics;

    vm = new InterpretedIsolatedVM();

    compiler = new CompilerService(diagnostics, arguments);
    compiler->strings = vm->strings;
    compiler->types = vm->types;

    compiler->bootstrap();
    vm->bootstrap();
}

void Driver::run() {
    auto module = compiler->compile(arguments->inputfile.string());
    if(diagnostics->errors.empty()) {
        ((InterpretedIsolatedVM*)vm)->run(module);
    } else {
        diagnostics->printErrors();
    }

}
