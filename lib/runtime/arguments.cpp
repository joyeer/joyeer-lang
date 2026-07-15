#include "joyeer/runtime/arguments.h"
#include "joyeer/diagnostic/diagnostic.h"
#include "joyeer/runtime/executor.h"
#include <iostream>

CommandLineArguments::CommandLineArguments(Diagnostics* diagnostics, int argc, char** argv) {
    this->diagnostics = diagnostics;
    std::vector<std::string> arguments;
    for(int i = 0 ; i < argc; i ++) {
        arguments.emplace_back(argv[i]);
    }

    parse(arguments);
}

void CommandLineArguments::parse(std::vector<std::string>& arguments) {
    auto iterator = arguments.begin();
    vmLocation = *iterator;

    iterator ++;
    for(; iterator != arguments.end(); iterator ++) {
        if(*iterator == "--debug-vm") {
            vmDebug = true;
        } else if(*iterator == "--lang=v0.1") {
            languageMode = LanguageMode::v0_1;
        } else if(*iterator == "--lang=v0.1-legacy") {
            languageMode = LanguageMode::legacy;
        } else if(*iterator == "-O0") {
            optimizationLevel = joyeer::OptimizationLevel::O0;
        } else if(*iterator == "-O1") {
            optimizationLevel = joyeer::OptimizationLevel::O1;
        } else if(*iterator == "-O2") {
            optimizationLevel = joyeer::OptimizationLevel::O2;
        } else if(*iterator == "-O3") {
            optimizationLevel = joyeer::OptimizationLevel::O3;
        } else if(iterator->starts_with("-O")) {
            diagnostics->reportError(
                    ErrorLevel::failure,
                    "unsupported optimization level '%s'; expected -O0, -O1, -O2, or -O3",
                    iterator->c_str());
        } else if(*iterator == "--emit-llvm" || *iterator == "-o") {
            const auto option = *iterator;
            iterator++;
            if(iterator == arguments.end()) {
                diagnostics->reportError(
                        ErrorLevel::failure,
                        "%s requires an output path",
                        option.c_str());
                break;
            }
            outputMode = option == "--emit-llvm"
                    ? OutputMode::llvmIR
                    : OutputMode::executable;
            outputFile = std::filesystem::path(*iterator);
        } else {
            // input file
            parseInputFile(*iterator);
        }
    }

    if(inputfile.empty() || !std::filesystem::exists(inputfile)) {
        diagnostics->reportError(ErrorLevel::failure, Diagnostics::errorNoSuchFileOrDirectory);
    }

    if(outputMode != OutputMode::validate && languageMode != LanguageMode::v0_1) {
        diagnostics->reportError(
                ErrorLevel::failure,
                "native output options require --lang=v0.1");
    }

    if(!inputfile.empty()) {
        accepted = true;
    }
}

void CommandLineArguments::printUsage() {
    std::cout << "Usage: joyeer [--lang=v0.1|--lang=v0.1-legacy] [-O0|-O1|-O2|-O3] [--emit-llvm <file>|-o <file>] <inputfile>" << std::endl;
    std::cout << "  --lang=v0.1         compile with the typed native pipeline" << std::endl;
    std::cout << "  --lang=v0.1-legacy  compile and run with the legacy VM pipeline (default)" << std::endl;
    std::cout << "  --emit-llvm <file>  write textual LLVM IR" << std::endl;
    std::cout << "  -o <file>           write a native executable" << std::endl;
    std::cout << "  -O0|-O1|-O2|-O3    native Clang optimization level (default: -O2)" << std::endl;
}

void CommandLineArguments::parseInputFile(const std::string &inputpath) {
    inputfile = std::filesystem::path(inputpath);
    if(inputfile.is_relative()) {
        inputfile = std::filesystem::absolute(inputfile).lexically_normal();
        workingDirectory = inputfile.parent_path();
    } else {
        // using input file as working directory
        workingDirectory = inputfile.parent_path();
    }
}


Arguments::Arguments(Executor *executor):
        executor(executor) {
}

Value Arguments::getArgument(Slot slot) {
    auto pValue = (Value*)(executor->stack + executor->fp - kValueSize - slot * kValueSize);
    return *pValue;
}