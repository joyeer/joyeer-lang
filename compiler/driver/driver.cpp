#include "driver.h"
#include "compiler/codegen/linker.h"
#include "toolchain.h"
#include "compiler/ir/ir.h"

#include <filesystem>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

#ifndef JOYEER_RUNTIME_PATH
#define JOYEER_RUNTIME_PATH ""
#endif

#ifndef JOYEER_SDK_ROOT_PATH
#define JOYEER_SDK_ROOT_PATH ""
#endif

#ifndef JOYEER_SDK_VERSION
#define JOYEER_SDK_VERSION ""
#endif

#ifndef JOYEER_MACOS_DEPLOYMENT_TARGET
#define JOYEER_MACOS_DEPLOYMENT_TARGET ""
#endif

#ifndef JOYEER_DSYMUTIL_EXECUTABLE_PATH
#define JOYEER_DSYMUTIL_EXECUTABLE_PATH ""
#endif

namespace {

std::filesystem::path runtimePath() {
    const auto configuredPath = std::filesystem::u8path(JOYEER_RUNTIME_PATH);
    if (configuredPath.is_relative()) {
        const auto directory = joyeer::executableDirectory();
        if (!directory.empty()) return directory / configuredPath;
    }
    return configuredPath;
}

bool printInstantiationReport(
        const SourceFile& source,
        const std::filesystem::path& workingDirectory,
        Diagnostics& diagnostics) {
    if (source.typeCheckedModel == nullptr || source.joyeerIR == nullptr) {
        diagnostics.reportDiagnostic(ErrorLevel::failure, "driver.missing-source-result",
                                     "compiler produced no generic instantiation data");
        return false;
    }
    const auto& model = *source.typeCheckedModel;
    const auto& module = *source.joyeerIR;
    std::map<std::pair<joyeer::semantic::SymbolId, std::string>, size_t> sizes;
    for (const auto& function : module.functions) {
        if (!function.symbol.has_value() || model.genericParameters(*function.symbol) == nullptr) continue;
        size_t size = 0;
        for (const auto& block : function.blocks) size += block.instructions.size();
        if (!sizes.emplace(std::pair { *function.symbol, function.name }, size).second) {
            diagnostics.reportDiagnostic(ErrorLevel::failure, "driver.invalid-instantiation-report",
                                         "generic instances have indistinguishable display names");
            return false;
        }
    }
    struct Entry {
        const joyeer::typing::Instance* instance;
        std::string function;
        std::vector<std::string> arguments;
    };
    std::vector<Entry> entries;
    for (const auto& instance : model.instances()) {
        Entry entry { &instance, model.functionDisplayName(instance.function), {} };
        for (const auto argument : instance.arguments) {
            entry.arguments.push_back(model.types().displayName(argument, true));
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return std::tie(left.function, left.arguments) < std::tie(right.function, right.arguments);
    });
    std::ostringstream report;
    for (const auto& entry : entries) {
        const auto& instance = *entry.instance;
        const auto size = sizes.find({ instance.function, instance.name });
        const auto* file = module.sourceFile(instance.requiredAt.sourceId);
        if (size == sizes.end() || file == nullptr) {
            diagnostics.reportDiagnostic(ErrorLevel::failure, "driver.invalid-instantiation-report",
                                         "generic instance has no lowered function or requiring source file");
            return false;
        }
        const auto requirer = instance.requiredBy.has_value()
                ? model.instances()[*instance.requiredBy].name
                : instance.requiredByFunction.has_value()
                    ? model.functionDisplayName(*instance.requiredByFunction) : "<default argument>";
        auto path = std::filesystem::u8path(file->directory) / std::filesystem::u8path(file->fileName);
        const auto relative = path.lexically_relative(workingDirectory);
        if (!relative.empty()) path = relative;
        const auto encoded = path.generic_u8string();
        const auto line = std::upper_bound(file->lineStarts.begin(), file->lineStarts.end(),
                                          instance.requiredAt.offset) - file->lineStarts.begin();
        report << instance.name << " | required by " << requirer << " at "
               << std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size())
               << ':' << line << " | " << size->second << " IR instructions\n";
    }
    // Instances of generic structs and enums are listed after the functions (decision 0022).
    std::vector<std::string> typeLines;
    for (const auto& structure : module.structures) {
        if (model.genericParameters(structure.symbol) == nullptr) continue;
        typeLines.push_back(structure.name + " | struct | " + std::to_string(structure.fields.size()) + " fields\n");
    }
    for (const auto& enumeration : module.enumerations) {
        if (model.genericParameters(enumeration.symbol) == nullptr) continue;
        typeLines.push_back(enumeration.name + " | enum | " + std::to_string(enumeration.cases.size()) + " cases\n");
    }
    std::sort(typeLines.begin(), typeLines.end());
    for (const auto& line : typeLines) report << line;
    std::cout << report.str();
    if (!std::cout) {
        diagnostics.reportDiagnostic(ErrorLevel::failure, "driver.instantiation-report-write-failed",
                                     "cannot write the generic instantiation report to standard output");
        return false;
    }
    return true;
}

} // namespace

Driver::Driver(Diagnostics* diagnostics, CommandLineArguments::Ptr arguments):arguments(arguments) {
    this->diagnostics = diagnostics;
    compiler = new CompilerService(
            diagnostics,
            joyeer::CompileOptions {
                arguments->workingDirectory,
                arguments->outputFile,
                arguments->outputMode,
                arguments->optimizationLevel,
                arguments->debugInfo,
                arguments->moduleName,
                arguments->sourceFiles,
                arguments->modules,
                arguments->runtimeModules,
                arguments->coreModules,
                arguments->nativeLibraries,
                joyeer::StandardLibrary::libraryDirectoryFor(arguments->sysroot),
                arguments->standardLibrary,
            });
}

int Driver::run() {
    compiler->compile(arguments->inputfile);
    if(diagnostics->hasFailure()) {
        diagnostics->printErrors();
        return 1;
    }
    if(arguments->outputMode == joyeer::OutputMode::executable ||
       arguments->outputMode == joyeer::OutputMode::runtimeObject) {
        const auto& source = compiler->getLastCompiledSourceFile();
        if (source == nullptr) {
            diagnostics->reportDiagnostic(
                    ErrorLevel::failure,
                    "driver.missing-source-result",
                    "compiler produced no source result");
        } else {
            // Standard modules that were actually imported bring their native archives.
            auto linkLibraries = arguments->nativeLibraries;
            for (const auto& library : compiler->implicitNativeLibraries()) {
                if (std::find(linkLibraries.begin(), linkLibraries.end(), library) == linkLibraries.end()) {
                    linkLibraries.push_back(library);
                }
            }
            const joyeer::native::LinkOptions options {
                        runtimePath(),
                        std::filesystem::u8path(JOYEER_SDK_ROOT_PATH),
                        JOYEER_SDK_VERSION,
                        JOYEER_MACOS_DEPLOYMENT_TARGET,
                        std::filesystem::u8path(JOYEER_DSYMUTIL_EXECUTABLE_PATH),
                        arguments->outputFile,
                        arguments->optimizationLevel,
                        arguments->debugInfo,
                        source->getAbstractPath(),
                        source->llvmModule,
                        linkLibraries,
                        source->llvmUsesThreads,
                    };
            const auto linking = arguments->outputMode == joyeer::OutputMode::runtimeObject
                    ? joyeer::native::Linker().emitObject(source->llvmIR, options)
                    : joyeer::native::Linker().link(source->llvmIR, source->llvmHasEntryPoint, options);
            for (const auto& diagnostic : linking.diagnostics) {
                        diagnostics->reportDiagnostic(
                        ErrorLevel::failure,
                        joyeer::native::diagnosticName(diagnostic.id),
                            diagnostic.message);
            }
        }
        if (diagnostics->hasFailure()) {
            diagnostics->printErrors();
            return 1;
        }
    }
    if (arguments->reportInstantiations) {
        const auto& source = compiler->getLastCompiledSourceFile();
        if (source == nullptr ||
            !printInstantiationReport(*source, arguments->workingDirectory, *diagnostics)) {
            if (source == nullptr) {
                diagnostics->reportDiagnostic(ErrorLevel::failure, "driver.missing-source-result",
                                             "compiler produced no source result");
            }
            diagnostics->printErrors();
            return 1;
        }
    }
    if (!diagnostics->errors.empty()) diagnostics->printErrors();
    return 0;
}
