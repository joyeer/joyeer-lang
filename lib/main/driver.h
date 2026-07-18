#ifndef __joyeer_driver_driver_h__
#define __joyeer_driver_driver_h__

#include "joyeer/main/arguments.h"
#include "joyeer/compiler/compiler+service.h"
#include "joyeer/diagnostic/diagnostic.h"

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
