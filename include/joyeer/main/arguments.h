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
    std::string moduleName;
    std::vector<std::filesystem::path> sourceFiles;
    std::vector<joyeer::ModuleSources> modules;

    bool accepted = false;
    joyeer::OutputMode outputMode = joyeer::OutputMode::validate;
    joyeer::OptimizationLevel optimizationLevel = joyeer::OptimizationLevel::O2;
    joyeer::DebugInfoOptions debugInfo;

    Diagnostics* diagnostics;

    // Narrow arguments use UTF-8 paths, not the Windows active code page.
    CommandLineArguments(Diagnostics* diagnostics, int argc, char** argv);
#if defined(_WIN32)
    // The compiler entry converts each UTF-16 argument strictly once before
    // parsing; malformed surrogate sequences are diagnostics, not replacements.
    CommandLineArguments(Diagnostics* diagnostics, int argc, wchar_t** argv);
#endif
    static void printUsage();

    void parse(std::vector<std::string>& arguments);
    void parseInputFile(const std::string& inputpath);
};

#endif