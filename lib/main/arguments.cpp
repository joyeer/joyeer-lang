#include "joyeer/main/arguments.h"

#include <algorithm>
#include <cwctype>
#include <iostream>

using joyeer::OutputMode;

namespace {

std::filesystem::path absoluteNormalized(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (!error) return result.lexically_normal();
    error.clear();
    result = std::filesystem::absolute(path, error);
    if (error) result = path;
    return result.lexically_normal();
}

#if defined(_WIN32)
std::wstring windowsPathKey(const std::filesystem::path& path) {
    auto text = absoluteNormalized(path).native();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    std::wstring result;
    size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(L'\\', start);
        auto component = text.substr(
                start,
                end == std::wstring::npos ? text.size() - start : end - start);
        if (!component.empty() && component.back() != L':') {
            while (!component.empty() &&
                   (component.back() == L' ' || component.back() == L'.')) {
                component.pop_back();
            }
        }
        std::transform(
                component.begin(),
                component.end(),
                component.begin(),
                [](wchar_t character) {
                    return static_cast<wchar_t>(std::towlower(character));
                });
        if (start != 0) result.push_back(L'\\');
        result += component;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}
#endif

bool pathsAlias(
        const std::filesystem::path& left,
        const std::filesystem::path& right) {
    if (left.empty() || right.empty()) return false;
    std::error_code error;
    if (std::filesystem::exists(left, error) && !error) {
        error.clear();
        if (std::filesystem::exists(right, error) && !error) {
            error.clear();
            if (std::filesystem::equivalent(left, right, error) && !error) return true;
        }
    }
#if defined(_WIN32)
    return windowsPathKey(left) == windowsPathKey(right);
#else
    return absoluteNormalized(left) == absoluteNormalized(right);
#endif
}

std::filesystem::path pdbPathFor(const std::filesystem::path& output) {
    auto pdb = output;
    pdb.replace_extension(".pdb");
    return pdb;
}

} // namespace

CommandLineArguments::CommandLineArguments(
        Diagnostics* diagnostics,
        int argc,
        char** argv):
        diagnostics(diagnostics) {
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    parse(arguments);
}

void CommandLineArguments::parse(std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        diagnostics->reportError(ErrorLevel::failure, "missing compiler arguments");
        return;
    }
    auto iterator = arguments.begin();
    executableLocation = *iterator;
    bool parseOptions = true;
    bool moduleNameProvided = false;

    iterator++;
    for (; iterator != arguments.end(); ++iterator) {
        if (parseOptions && *iterator == "--") {
            parseOptions = false;
        } else if (parseOptions && *iterator == "-O0") {
            optimizationLevel = joyeer::OptimizationLevel::O0;
        } else if (parseOptions && *iterator == "-O1") {
            optimizationLevel = joyeer::OptimizationLevel::O1;
        } else if (parseOptions && *iterator == "-O2") {
            optimizationLevel = joyeer::OptimizationLevel::O2;
        } else if (parseOptions && *iterator == "-O3") {
            optimizationLevel = joyeer::OptimizationLevel::O3;
        } else if (parseOptions && iterator->starts_with("-O")) {
            diagnostics->reportError(
                    ErrorLevel::failure,
                    "unsupported optimization level '%s'; expected -O0, -O1, -O2, or -O3",
                    iterator->c_str());
        } else if (parseOptions && *iterator == "-g0") {
            debugInfo.emitLineTables = false;
            debugInfo.emitVariables = false;
        } else if (parseOptions && (*iterator == "-g" || *iterator == "-gline-tables-only")) {
            debugInfo.emitLineTables = true;
            debugInfo.emitVariables = false;
        } else if (parseOptions && *iterator == "-gfull") {
            debugInfo.emitLineTables = true;
            debugInfo.emitVariables = true;
        } else if (parseOptions && *iterator == "-gdwarf") {
            debugInfo.emitLineTables = true;
            debugInfo.format = joyeer::DebugInfoFormat::dwarf;
        } else if (parseOptions && *iterator == "-gcodeview") {
            debugInfo.emitLineTables = true;
            debugInfo.format = joyeer::DebugInfoFormat::codeView;
        } else if (parseOptions && iterator->starts_with("-g")) {
            diagnostics->reportError(
                    ErrorLevel::failure,
                    "unsupported debug option '%s'; expected -g0, -g, -gline-tables-only, -gfull, -gdwarf, or -gcodeview",
                    iterator->c_str());
        } else if (parseOptions && (*iterator == "--module-root" || *iterator == "--module")) {
            diagnostics->reportError(
                    ErrorLevel::failure,
                    "%s directory inputs are no longer supported; use --module-name with explicit source files and --module-source name=file",
                    iterator->c_str());
            if (iterator + 1 != arguments.end() && !(iterator + 1)->starts_with('-')) ++iterator;
        } else if (parseOptions && (*iterator == "--module-name" || *iterator == "--module-source")) {
            const auto option = *iterator;
            ++iterator;
            if (iterator == arguments.end()) {
                diagnostics->reportError(ErrorLevel::failure, "%s requires a value", option.c_str());
                break;
            }
            if (iterator->empty() || iterator->starts_with('-')) {
                diagnostics->reportError(ErrorLevel::failure, "%s requires a value", option.c_str());
                if (!iterator->empty()) --iterator;
                continue;
            }
            if (option == "--module-name") {
                if (moduleNameProvided) {
                    diagnostics->reportError(ErrorLevel::failure, "--module-name may only be specified once");
                } else if (!joyeer::isModuleName(*iterator)) {
                    diagnostics->reportError(ErrorLevel::failure, "invalid logical module name '%s'", iterator->c_str());
                } else {
                    moduleName = *iterator;
                }
                moduleNameProvided = true;
            } else {
                const auto separator = iterator->find('=');
                const auto name = iterator->substr(0, separator);
                if (separator == std::string::npos || separator + 1 == iterator->size() ||
                    !joyeer::isModuleName(name)) {
                    diagnostics->reportError(
                            ErrorLevel::failure,
                            "--module-source requires a logical.dotted.name=file mapping");
                } else {
                    const auto file = absoluteNormalized(
                            std::filesystem::path(iterator->substr(separator + 1)));
                    const auto found = std::find_if(modules.begin(), modules.end(),
                            [&](const auto& module) { return module.name == name; });
                    if (found == modules.end()) {
                        modules.push_back({name, {file}});
                    } else {
                        found->files.push_back(file);
                    }
                }
            }
        } else if (parseOptions && (*iterator == "--emit-llvm" || *iterator == "-o")) {
            const auto option = *iterator;
            ++iterator;
            if (iterator == arguments.end()) {
                diagnostics->reportError(
                        ErrorLevel::failure,
                        "%s requires an output path",
                        option.c_str());
                break;
            }
            if (iterator->starts_with('-')) {
                diagnostics->reportError(
                        ErrorLevel::failure,
                        "%s requires an output path, found option '%s'",
                        option.c_str(),
                        iterator->c_str());
                --iterator;
                continue;
            }
            outputMode = option == "--emit-llvm"
                    ? OutputMode::llvmIR
                    : OutputMode::executable;
            outputFile = std::filesystem::path(*iterator);
        } else if (parseOptions && iterator->starts_with('-')) {
            diagnostics->reportError(
                    ErrorLevel::failure,
                    "unknown option '%s'",
                    iterator->c_str());
        } else {
            parseInputFile(*iterator);
        }
    }

    if (moduleName.empty()) {
        if (!modules.empty()) {
            diagnostics->reportError(ErrorLevel::failure, "--module-source requires --module-name");
        }
        if (sourceFiles.size() > 1) {
            diagnostics->reportError(ErrorLevel::failure, "multiple input files require --module-name");
        }
        if (!sourceFiles.empty()) {
            inputfile = sourceFiles.front();
            workingDirectory = inputfile.parent_path();
        }
    } else {
        std::error_code error;
        workingDirectory = std::filesystem::current_path(error);
        if (error) {
            diagnostics->reportError(
                    ErrorLevel::failure, "cannot determine compiler working directory: %s",
                    error.message().c_str());
        }
        if (sourceFiles.empty()) {
            diagnostics->reportError(ErrorLevel::failure, "--module-name requires at least one source file");
        }
    }
    if (sourceFiles.empty() && moduleName.empty() && !diagnostics->hasFailure()) {
        diagnostics->reportError(ErrorLevel::failure, Diagnostics::errorNoSuchFileOrDirectory);
    }
    auto inputs = sourceFiles;
    for (const auto& module : modules) {
        if (module.name == moduleName) {
            diagnostics->reportError(
                    ErrorLevel::failure, "dependency module '%s' conflicts with the root module", module.name.c_str());
        }
        inputs.insert(inputs.end(), module.files.begin(), module.files.end());
    }
    for (size_t index = 0; index < inputs.size(); ++index) {
        std::error_code error;
        if (!moduleName.empty() && !std::filesystem::is_regular_file(inputs[index], error)) {
            diagnostics->reportDiagnostic(
                    ErrorLevel::failure, "module.read-source",
                    "module input must be a regular source file: '" + inputs[index].string() + "'" +
                    (error ? ": " + error.message() : ""));
        } else if (moduleName.empty() && !std::filesystem::exists(inputs[index], error)) {
            diagnostics->reportError(ErrorLevel::failure, Diagnostics::errorNoSuchFileOrDirectory);
        }
        if (std::any_of(inputs.begin(), inputs.begin() + index,
                [&](const auto& earlier) { return pathsAlias(earlier, inputs[index]); })) {
            diagnostics->reportError(
                    ErrorLevel::failure, "duplicate source file: '%s'", inputs[index].string().c_str());
        }
    }

#if !defined(_WIN32)
    if (debugInfo.emitLineTables && debugInfo.format == joyeer::DebugInfoFormat::codeView) {
        diagnostics->reportError(
                ErrorLevel::failure,
                "-gcodeview is supported only for Windows native output");
    }
#endif

    if (outputMode != OutputMode::validate) {
        for (const auto& input : inputs) {
            if (pathsAlias(input, outputFile)) {
                diagnostics->reportError(
                        ErrorLevel::failure,
                        "output path must not overwrite the input file");
            }
#if defined(_WIN32)
            if (outputMode == OutputMode::executable && pathsAlias(input, pdbPathFor(outputFile))) {
                diagnostics->reportError(
                        ErrorLevel::failure,
                        "sibling PDB cleanup path must not overwrite the input file");
            }
#endif
        }
    }
#if defined(_WIN32)
    if (outputMode != OutputMode::validate &&
        outputFile.filename().native().find(L':') != std::wstring::npos) {
        diagnostics->reportError(
                ErrorLevel::failure,
                "output paths must not use Windows alternate data streams");
    }
#endif

    if (moduleName.empty()) sourceFiles.clear();
    accepted = (!inputfile.empty() || !sourceFiles.empty()) && !diagnostics->hasFailure();
}

void CommandLineArguments::printUsage() {
    std::cout << "Module usage: joyeer --module-name <name> <source>... [--module-source <name=file>]... [output options]" << std::endl;
    std::cout << "Usage: joyeer [-O0|-O1|-O2|-O3] [-g0|-g|-gline-tables-only|-gfull|-gdwarf|-gcodeview] [--emit-llvm <file>|-o <file>] <inputfile>" << std::endl;
    std::cout << "  --emit-llvm <file>  write textual LLVM IR" << std::endl;
    std::cout << "  -o <file>           write a native executable" << std::endl;
    std::cout << "  --module-name <name> name the root compilation unit of explicit source files" << std::endl;
    std::cout << "  --module-source <name=file> add a file to a dependency module (repeatable)" << std::endl;
    std::cout << "  -O0|-O1|-O2|-O3    native optimization level (default: -O2)" << std::endl;
    std::cout << "  -g0                  disable debug line tables (default)" << std::endl;
    std::cout << "  -g|-gline-tables-only emit source line tables using the platform format" << std::endl;
    std::cout << "  -gfull               emit line tables plus source variables and physical types" << std::endl;
    std::cout << "  -gdwarf              emit DWARF 4 line tables" << std::endl;
    std::cout << "  -gcodeview           emit Windows CodeView line tables" << std::endl;
}

void CommandLineArguments::parseInputFile(const std::string& inputpath) {
    if (inputpath.empty()) {
        diagnostics->reportError(ErrorLevel::failure, "source file path must not be empty");
        return;
    }
    sourceFiles.push_back(absoluteNormalized(std::filesystem::path(inputpath)));
}