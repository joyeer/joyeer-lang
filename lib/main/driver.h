#ifndef __joyeer_driver_driver_h__
#define __joyeer_driver_driver_h__

#include "joyeer/runtime/arguments.h"
#include "joyeer/compiler/compiler+service.h"
#include "joyeer/vm/isolate.h"
#include "joyeer/diagnostic/diagnostic.h"

class Driver {
public:
    Driver(Diagnostics* diagnostics, CommandLineArguments::Ptr arguments);
    int run();

private:
    CompilerService* compiler;
    IsolateVM* vm = nullptr;
    CommandLineArguments::Ptr arguments;
    Diagnostics* diagnostics;
};

#endif
