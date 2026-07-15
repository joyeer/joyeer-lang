#include "joyeer/backend/linker.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace joyeer::native {

namespace {

std::atomic<uint64_t> nextTemporary { 0 };

std::string quote(const std::filesystem::path& value) {
    auto text = value.string();
    if (text.find('"') != std::string::npos) return {};
    return '"' + text + '"';
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

} // namespace

LinkResult Linker::link(
        const std::string& llvmIR,
        bool hasEntryPoint,
        const LinkOptions& options) const {
    LinkResult result;
    auto report = [&result](LinkDiagnosticId id, std::string message) {
        result.diagnostics.push_back(LinkDiagnostic { id, std::move(message) });
    };

    if (!hasEntryPoint) {
        report(LinkDiagnosticId::missingEntryPoint, "native executable requires 'func main()'");
        return result;
    }
    if (options.clangExecutable.empty() ||
        !std::filesystem::is_regular_file(options.clangExecutable)) {
        report(
                LinkDiagnosticId::missingTool,
                "Clang executable was not found at '" +
                        options.clangExecutable.string() + "'");
        return result;
    }
    if (options.runtimeLibrary.empty() ||
        !std::filesystem::is_regular_file(options.runtimeLibrary)) {
        report(
                LinkDiagnosticId::missingTool,
                "Joyeer native runtime was not found at '" +
                        options.runtimeLibrary.string() + "'");
        return result;
    }
    if (options.outputFile.empty()) {
        report(LinkDiagnosticId::fileError, "native output path is empty");
        return result;
    }

    const auto parent = options.outputFile.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        report(
                LinkDiagnosticId::fileError,
                "native output directory does not exist: '" + parent.string() + "'");
        return result;
    }

    const auto nonce = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
            nextTemporary.fetch_add(1);
    const auto base = std::filesystem::temp_directory_path() /
            ("joyeer-native-" + std::to_string(nonce));
    const auto llvmFile = std::filesystem::path(base.string() + ".ll");
    const auto logFile = std::filesystem::path(base.string() + ".log");

    {
        std::ofstream output(llvmFile, std::ios::binary);
        output << llvmIR;
        if (!output.good()) {
            report(
                    LinkDiagnosticId::fileError,
                    "cannot write temporary LLVM IR file '" + llvmFile.string() + "'");
            return result;
        }
    }

    const auto clang = quote(options.clangExecutable);
    const auto runtime = quote(options.runtimeLibrary);
    const auto input = quote(llvmFile);
    const auto output = quote(options.outputFile);
    const auto log = quote(logFile);
    if (clang.empty() || runtime.empty() || input.empty() || output.empty() || log.empty()) {
        std::filesystem::remove(llvmFile);
        report(LinkDiagnosticId::fileError, "paths containing quotes are not supported");
        return result;
    }

        const auto commandBody = clang + " " + optimizationFlag(options.optimizationLevel) +
            " -Wno-override-module -x ir " + input +
            " -x none " + runtime + " -o " + output + " > " + log + " 2>&1";
    #if defined(_WIN32)
        const auto command = '"' + commandBody + '"';
    #else
        const auto& command = commandBody;
    #endif
    const auto status = std::system(command.c_str());
    const auto toolOutput = readText(logFile);
    std::filesystem::remove(llvmFile);
    std::filesystem::remove(logFile);
    if (status != 0 || !std::filesystem::is_regular_file(options.outputFile)) {
        std::filesystem::remove(options.outputFile);
        report(
                LinkDiagnosticId::toolFailure,
                "Clang failed to link the native executable" +
                        (toolOutput.empty() ? std::string() : ":\n" + toolOutput));
    }
    return result;
}

const char* diagnosticName(LinkDiagnosticId id) {
    switch (id) {
        case LinkDiagnosticId::missingEntryPoint: return "linker.missing-entry-point";
        case LinkDiagnosticId::missingTool: return "linker.missing-tool";
        case LinkDiagnosticId::fileError: return "linker.file-error";
        case LinkDiagnosticId::toolFailure: return "linker.tool-failure";
    }
    return "linker.unknown";
}

std::string dump(const std::vector<LinkDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::native
