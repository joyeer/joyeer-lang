#ifndef __joyeer_driver_driver_h__
#define __joyeer_driver_driver_h__

#include "arguments.h"
#include "compiler+service.h"
#include "compiler/diagnostics/diagnostic.h"

class Driver {
public:
    Driver(Diagnostics* diagnostics, CommandLineArguments::Ptr arguments);
    int run();

private:
    CompilerService* compiler;
    CommandLineArguments::Ptr arguments;
    Diagnostics* diagnostics;
};

#endif
