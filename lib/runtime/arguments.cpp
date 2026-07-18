#include "joyeer/runtime/arguments.h"
#include "joyeer/runtime/executor.h"

Arguments::Arguments(Executor* executor):
        executor(executor) {
}

Value Arguments::getArgument(Slot slot) {
    auto value = reinterpret_cast<Value*>(
            executor->stack + executor->fp - kValueSize - slot * kValueSize);
    return *value;
}