#ifndef __joyeer_runtime_arguments_h__
#define __joyeer_runtime_arguments_h__

#include <filesystem>
#include <string>
#include <vector>
#include <memory>

#include "joyeer/compiler/options.h"
#include "joyeer/runtime/types.h"
#include "joyeer/diagnostic/diagnostic.h"

enum class LanguageMode {
    legacy,
    v0_1
};

enum class OutputMode {
    validate,
    llvmIR,
    executable,
};

struct CommandLineArguments {
    using Ptr = std::shared_ptr<CommandLineArguments>;
    
    // the executable of joyeer's locationInParent
    std::filesystem::path vmLocation;
    // main entry point of source code locationInParent
    std::filesystem::path inputfile;
    // working directory for source code
    std::filesystem::path workingDirectory;
    std::filesystem::path outputFile;
    
    bool vmDebug = true;
    bool accepted = false;
    LanguageMode languageMode = LanguageMode::legacy;
    OutputMode outputMode = OutputMode::validate;
    joyeer::OptimizationLevel optimizationLevel = joyeer::OptimizationLevel::O2;

    Diagnostics* diagnostics;

    CommandLineArguments(Diagnostics* diagnostics, int argc, char** argv);
    // print compiler usage
    static void printUsage();

    void parse(std::vector<std::string>& arguments);
    void parseInputFile(const std::string &inputpath);
};

struct Executor;

struct Arguments {
    explicit Arguments(Executor* executor);
    Value getArgument(Slot slot);

private:
    Executor* executor;
};


#endif
