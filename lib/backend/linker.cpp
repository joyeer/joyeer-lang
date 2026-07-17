#include "joyeer/backend/linker.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace joyeer::native {

namespace {

std::atomic<uint64_t> nextTemporary { 0 };

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::filesystem::path pdbPathFor(const std::filesystem::path& output) {
    auto pdb = output;
    pdb.replace_extension(".pdb");
    return pdb;
}

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

bool pathIsWithin(
        const std::filesystem::path& child,
        const std::filesystem::path& parent) {
    if (child.empty() || parent.empty()) return false;
#if defined(_WIN32)
    const auto childKey = windowsPathKey(child);
    auto parentKey = windowsPathKey(parent);
    if (!parentKey.ends_with(L'\\')) parentKey.push_back(L'\\');
    return childKey.starts_with(parentKey);
#else
    const auto relative = absoluteNormalized(child).lexically_relative(
            absoluteNormalized(parent));
    if (relative.empty() || relative.is_absolute()) return false;
    const auto first = *relative.begin();
    return first != "..";
#endif
}

bool isNonemptyRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
            std::filesystem::file_size(path, error) > 0 && !error;
}

struct ProcessResult {
    int exitCode = -1;
    std::string launchError;
};

#if defined(_WIN32)

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

ProcessResult runProcess(
        const std::filesystem::path& executable,
        const std::vector<std::filesystem::path>& arguments,
        const std::filesystem::path& logFile) {
    SECURITY_ATTRIBUTES securityAttributes {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;
    const auto logHandle = CreateFileW(
            logFile.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        return ProcessResult {
            -1,
            "cannot create tool output log: " +
                    std::error_code(GetLastError(), std::system_category()).message(),
        };
    }
    const auto inputHandle = CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    if (inputHandle == INVALID_HANDLE_VALUE) {
        const auto error = std::error_code(GetLastError(), std::system_category()).message();
        CloseHandle(logHandle);
        return ProcessResult { -1, "cannot open null input: " + error };
    }

    std::wstring commandLine = quoteWindowsArgument(executable.native());
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument.native());
    }
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inputHandle;
    startup.hStdOutput = logHandle;
    startup.hStdError = logHandle;
    PROCESS_INFORMATION process {};
    const auto started = CreateProcessW(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process);
    CloseHandle(inputHandle);
    CloseHandle(logHandle);
    if (!started) {
        return ProcessResult {
            -1,
            "cannot launch Clang: " +
                    std::error_code(GetLastError(), std::system_category()).message(),
        };
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) exitCode = 1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ProcessResult { static_cast<int>(exitCode), {} };
}

#else

ProcessResult runProcess(
        const std::filesystem::path& executable,
        const std::vector<std::filesystem::path>& arguments,
        const std::filesystem::path& logFile) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable.string());
    for (const auto& argument : arguments) storage.push_back(argument.string());
    std::vector<char*> rawArguments;
    rawArguments.reserve(storage.size() + 1);
    for (auto& argument : storage) rawArguments.push_back(argument.data());
    rawArguments.push_back(nullptr);

    const auto logDescriptor = open(
            logFile.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR);
    if (logDescriptor < 0) {
        return ProcessResult {
            -1,
            "cannot create tool output log: " + std::string(std::strerror(errno)),
        };
    }
    const auto process = fork();
    if (process < 0) {
        const auto error = std::string(std::strerror(errno));
        close(logDescriptor);
        return ProcessResult { -1, "cannot launch Clang: " + error };
    }
    if (process == 0) {
        static_cast<void>(dup2(logDescriptor, STDOUT_FILENO));
        static_cast<void>(dup2(logDescriptor, STDERR_FILENO));
        close(logDescriptor);
        execv(executable.c_str(), rawArguments.data());
        const auto message = std::string("cannot launch Clang: ") + std::strerror(errno) + "\n";
        static_cast<void>(write(STDERR_FILENO, message.data(), message.size()));
        _exit(127);
    }
    close(logDescriptor);
    int status = 0;
    while (waitpid(process, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return ProcessResult { -1, "cannot wait for Clang: " + std::string(std::strerror(errno)) };
    }
    if (WIFEXITED(status)) return ProcessResult { WEXITSTATUS(status), {} };
    if (WIFSIGNALED(status)) return ProcessResult { 128 + WTERMSIG(status), {} };
    return ProcessResult { 1, {} };
}

#endif

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
    std::error_code filesystemError;
    if (options.clangExecutable.empty() ||
        !std::filesystem::is_regular_file(options.clangExecutable, filesystemError) ||
        filesystemError) {
        report(
                LinkDiagnosticId::missingTool,
                "Clang executable was not found at '" +
                        options.clangExecutable.string() + "'");
        return result;
    }
    filesystemError.clear();
    if (options.runtimeLibrary.empty() ||
        !std::filesystem::is_regular_file(options.runtimeLibrary, filesystemError) ||
        filesystemError) {
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

#if !defined(_WIN32)
    if (options.debugInfo.emitLineTables &&
        options.debugInfo.format == DebugInfoFormat::codeView) {
        report(
                LinkDiagnosticId::toolFailure,
                "CodeView debug artifacts are supported only on Windows");
        return result;
    }
#else
    if (options.debugInfo.emitLineTables &&
        options.debugInfo.format == DebugInfoFormat::dwarf) {
        const auto bundledLld = options.clangExecutable.parent_path() / "lld-link.exe";
        filesystemError.clear();
        if (!std::filesystem::is_regular_file(bundledLld, filesystemError) ||
            filesystemError) {
        report(
                LinkDiagnosticId::missingTool,
                    "LLD is required for Windows DWARF executable output and was not found at '" +
                            bundledLld.string() + "'");
            return result;
        }
    }
#endif

    const auto parent = options.outputFile.parent_path();
    filesystemError.clear();
    if (!parent.empty() &&
        (!std::filesystem::is_directory(parent, filesystemError) || filesystemError)) {
        report(
                LinkDiagnosticId::fileError,
                "native output directory does not exist or is not a directory: '" +
                        parent.string() + "'");
        return result;
    }

    const auto pdbFile = pdbPathFor(options.outputFile);
    const auto dsymDirectory = std::filesystem::path(options.outputFile.string() + ".dSYM");
    const std::vector<std::filesystem::path> protectedFiles {
        options.clangExecutable,
        options.runtimeLibrary,
        options.sourceFile,
    };
    auto collidesWithProtectedFile = [&](const std::filesystem::path& path) {
        return std::any_of(
                protectedFiles.begin(),
                protectedFiles.end(),
                [&path](const auto& protectedFile) {
                    return pathsAlias(path, protectedFile);
                });
    };
    if (collidesWithProtectedFile(options.outputFile)) {
        report(
                LinkDiagnosticId::fileError,
                "native output path collides with a compiler input");
        return result;
    }
#if defined(_WIN32)
    if (options.outputFile.filename().native().find(L':') != std::wstring::npos) {
        report(
                LinkDiagnosticId::fileError,
                "native output paths must not use Windows alternate data streams");
        return result;
    }
    if (collidesWithProtectedFile(pdbFile)) {
        report(
                LinkDiagnosticId::fileError,
                "sibling PDB cleanup path collides with a compiler input");
        return result;
    }
#elif defined(__APPLE__)
    if (std::any_of(
                protectedFiles.begin(),
                protectedFiles.end(),
                [&dsymDirectory](const auto& protectedFile) {
                    return pathIsWithin(protectedFile, dsymDirectory);
                })) {
        report(
                LinkDiagnosticId::fileError,
                "dSYM output path contains a compiler input");
        return result;
    }
#endif
#if defined(_WIN32)
    if (options.debugInfo.emitLineTables &&
        options.debugInfo.format == DebugInfoFormat::codeView) {
        auto normalizedName = options.outputFile.filename().wstring();
        while (!normalizedName.empty() &&
               (normalizedName.back() == L' ' || normalizedName.back() == L'.')) {
            normalizedName.pop_back();
        }
        auto extension = std::filesystem::path(normalizedName).extension().wstring();
        std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
        if (extension == L".pdb") {
            report(
                    LinkDiagnosticId::fileError,
                    "CodeView executable output cannot use the '.pdb' extension because it collides with the sibling PDB artifact");
            return result;
        }
    }
#endif
    auto removeOutputFile = [&](const std::filesystem::path& path, const char* description) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            report(
                    LinkDiagnosticId::fileError,
                    "cannot inspect " + std::string(description) + " '" + path.string() +
                            "': " + error.message());
            return false;
        }
        if (!std::filesystem::exists(status)) return true;
        if (std::filesystem::is_directory(status)) {
            report(
                    LinkDiagnosticId::fileError,
                    "refusing to overwrite directory at " + std::string(description) +
                            " path '" + path.string() + "'");
            return false;
        }
        if (!std::filesystem::remove(path, error) || error) {
            report(
                    LinkDiagnosticId::fileError,
                    "cannot remove stale " + std::string(description) + " '" +
                            path.string() + "': " + error.message());
            return false;
        }
        return true;
    };
    if (!removeOutputFile(options.outputFile, "native output")) return result;
#if defined(_WIN32)
    if (!removeOutputFile(pdbFile, "PDB output")) {
        return result;
    }
#elif defined(__APPLE__)
    {
        filesystemError.clear();
        const auto status = std::filesystem::symlink_status(dsymDirectory, filesystemError);
        if (filesystemError && filesystemError != std::errc::no_such_file_or_directory) {
            report(
                    LinkDiagnosticId::fileError,
                    "cannot inspect dSYM output '" + dsymDirectory.string() + "': " +
                            filesystemError.message());
            return result;
        }
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status)) {
                report(
                        LinkDiagnosticId::fileError,
                        "refusing to overwrite non-directory dSYM path '" +
                                dsymDirectory.string() + "'");
                return result;
            }
            std::filesystem::remove_all(dsymDirectory, filesystemError);
            if (filesystemError) {
                report(
                        LinkDiagnosticId::fileError,
                        "cannot remove stale dSYM bundle '" + dsymDirectory.string() +
                                "': " + filesystemError.message());
                return result;
            }
        }
    }
#endif

    const auto nonce = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
            nextTemporary.fetch_add(1);
    filesystemError.clear();
    const auto temporaryDirectory = std::filesystem::temp_directory_path(filesystemError);
    if (filesystemError) {
        report(
                LinkDiagnosticId::fileError,
                "cannot locate the temporary directory: " + filesystemError.message());
        return result;
    }
    const auto base = temporaryDirectory /
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

    std::vector<std::filesystem::path> clangArguments {
        optimizationFlag(options.optimizationLevel),
        "-Wno-override-module",
        "-x",
        "ir",
        llvmFile,
        "-x",
        "none",
        options.runtimeLibrary,
        "-o",
        options.outputFile,
    };
    if (options.debugInfo.emitLineTables) {
#if defined(_WIN32)
        const auto optimizeReferences = options.optimizationLevel == OptimizationLevel::O0
                ? "/OPT:NOREF"
                : "/OPT:REF";
        const auto foldIdenticalCode = options.optimizationLevel == OptimizationLevel::O0
                ? "/OPT:NOICF"
                : "/OPT:ICF";
        if (options.debugInfo.format == DebugInfoFormat::codeView) {
            clangArguments.insert(
                    clangArguments.end(),
                    {
                        "-Xlinker", "/DEBUG:FULL",
                        "-Xlinker", "/INCREMENTAL:NO",
                        "-Xlinker", optimizeReferences,
                        "-Xlinker", foldIdenticalCode,
                        "-Xlinker",
                        std::filesystem::path(L"/PDB:" + pdbFile.native()),
                    });
        } else {
            clangArguments.insert(
                    clangArguments.end(),
                    {
                        "-B", options.clangExecutable.parent_path(),
                        "-fuse-ld=lld",
                        "-Xlinker", "/DEBUG:DWARF",
                        "-Xlinker", "/INCREMENTAL:NO",
                        "-Xlinker", optimizeReferences,
                        "-Xlinker", foldIdenticalCode,
                    });
        }
#elif defined(__APPLE__)
    clangArguments.emplace_back(
        options.debugInfo.emitVariables ? "-g" : "-gline-tables-only");
#endif
    }

    const auto process = runProcess(options.clangExecutable, clangArguments, logFile);
    const auto toolOutput = readText(logFile);
    filesystemError.clear();
    std::filesystem::remove(llvmFile, filesystemError);
    filesystemError.clear();
    std::filesystem::remove(logFile, filesystemError);
    auto cleanupFailedOutput = [&]() {
        std::error_code error;
        std::filesystem::remove(options.outputFile, error);
#if defined(_WIN32)
        error.clear();
        std::filesystem::remove(pdbFile, error);
#elif defined(__APPLE__)
        error.clear();
        std::filesystem::remove_all(dsymDirectory, error);
#endif
    };
    if (process.exitCode != 0 || !process.launchError.empty() ||
        !isNonemptyRegularFile(options.outputFile)) {
        cleanupFailedOutput();
        report(
                LinkDiagnosticId::toolFailure,
                "Clang failed to link the native executable" +
                        (process.launchError.empty()
                                ? std::string()
                                : ":\n" + process.launchError) +
                        (toolOutput.empty() ? std::string() : ":\n" + toolOutput));
        return result;
    }

#if defined(_WIN32)
    if (options.debugInfo.emitLineTables &&
        options.debugInfo.format == DebugInfoFormat::codeView) {
        if (!isNonemptyRegularFile(pdbFile)) {
            cleanupFailedOutput();
            report(
                    LinkDiagnosticId::toolFailure,
                    "native linker did not produce the expected CodeView PDB '" +
                            pdbFile.string() + "'");
            return result;
        }
        result.debugArtifact = pdbFile;
    } else if (options.debugInfo.emitLineTables) {
        result.debugArtifact = options.outputFile;
    }
#elif defined(__APPLE__)
    if (options.debugInfo.emitLineTables) {
        const auto dsymBinary = dsymDirectory / "Contents" / "Resources" / "DWARF" /
                options.outputFile.filename();
    if (!isNonemptyRegularFile(dsymBinary)) {
            cleanupFailedOutput();
            report(
                    LinkDiagnosticId::toolFailure,
            "Clang did not produce the expected dSYM bundle '" +
                dsymDirectory.string() + "'");
            return result;
        }
        result.debugArtifact = dsymDirectory;
    }
#else
    if (options.debugInfo.emitLineTables) {
        result.debugArtifact = options.outputFile;
    }
#endif
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
