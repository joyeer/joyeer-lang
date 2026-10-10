#include "driver.h"

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif

    auto diagnostics = new Diagnostics();
    // command line arguments
    auto options = std::make_shared<CommandLineArguments>(diagnostics, argc, argv);

    if (diagnostics->hasFailure()) {
        diagnostics->printErrors();
        return 1;
    }
    if(!options->accepted) {
        options->printUsage();
        return 0;
    }

    Driver driver(diagnostics, options);
    return driver.run();
}
