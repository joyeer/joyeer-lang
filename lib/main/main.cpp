#include "driver.h"

int main(int argc, char** argv) {

    auto diagnostics = new Diagnostics();
    // command line arguments
    auto options = std::make_shared<CommandLineArguments>(diagnostics, argc, argv);
    
    if(!options->accepted) {
        options->printUsage();
        return 0;
    }

    Driver driver(diagnostics, options);
    driver.run();
    
    return 0;
}
