#ifndef __joyeer_main_arguments_h__
#define __joyeer_main_arguments_h__

#include "joyeer/compiler/options.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct CommandLineArguments {
    using Ptr = std::shared_ptr<CommandLineArguments>;

    std::filesystem::path executableLocation;
    std::filesystem::path inputfile;
    std::filesystem::path workingDirectory;
    std::filesystem::path outputFile;

    bool accepted = false;
    joyeer::OutputMode outputMode = joyeer::OutputMode::validate;
    joyeer::OptimizationLevel optimizationLevel = joyeer::OptimizationLevel::O2;
    joyeer::DebugInfoOptions debugInfo;

    Diagnostics* diagnostics;

    CommandLineArguments(Diagnostics* diagnostics, int argc, char** argv);
    static void printUsage();

    void parse(std::vector<std::string>& arguments);
    void parseInputFile(const std::string& inputpath);
};

#endif