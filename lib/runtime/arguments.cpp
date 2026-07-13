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
        } else {
            // input file
            parseInputFile(*iterator);
        }
    }

    if(!std::filesystem::exists(inputfile)) {
        diagnostics->reportError(ErrorLevel::failure, Diagnostics::errorNoSuchFileOrDirectory);
    }

    if(!inputfile.empty()) {
        accepted = true;
    }
}

void CommandLineArguments::printUsage() {
    std::cout << "Usage: joyeer [--lang=v0.1|--lang=v0.1-legacy] <inputfile>" << std::endl;
    std::cout << "  --lang=v0.1         validate with the new lexer and Parser MVP (no codegen yet)" << std::endl;
    std::cout << "  --lang=v0.1-legacy  compile and run with the legacy VM pipeline (default)" << std::endl;
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