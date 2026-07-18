#ifndef __joyeer_runtime_arguments_h__
#define __joyeer_runtime_arguments_h__

#include "joyeer/runtime/types.h"

struct Executor;

struct Arguments {
    explicit Arguments(Executor* executor);
    Value getArgument(Slot slot);

private:
    Executor* executor;
};

#endif
