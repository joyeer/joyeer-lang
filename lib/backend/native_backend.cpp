#include "joyeer/backend/native_backend.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#endif
#include "lld/Common/Driver.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#if defined(_WIN32)
#include "llvm/WindowsDriver/MSVCPaths.h"
#endif

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
LLD_HAS_DRIVER(coff)
#elif defined(__APPLE__)
LLD_HAS_DRIVER(macho)
#else
LLD_HAS_DRIVER(elf)
#endif

namespace {

#if defined(_WIN32)
constexpr lld::Flavor nativeLinkerFlavor = lld::WinLink;
lld::Driver nativeLinkerDriver = &lld::coff::link;
#elif defined(__APPLE__)
constexpr lld::Flavor nativeLinkerFlavor = lld::Darwin;
lld::Driver nativeLinkerDriver = &lld::macho::link;
#else
constexpr lld::Flavor nativeLinkerFlavor = lld::Gnu;
lld::Driver nativeLinkerDriver = &lld::elf::link;
#endif
std::mutex lldMutex;

void report(
        JoyeerNativeBackendDiagnosticCallback callback,
        void* context,
        JoyeerNativeBackendStatus status,
        std::string_view message) {
    if (callback == nullptr) return;
    callback(context, status, message.data(), message.size());
}

bool validHeader(uint32_t abiVersion, uint32_t structSize, size_t expectedSize) {
    return abiVersion == JOYEER_NATIVE_BACKEND_ABI_VERSION &&
            structSize >= expectedSize;
}

llvm::OptimizationLevel optimizationLevel(
        JoyeerNativeBackendOptimizationLevel level) {
    switch (level) {
        case JOYEER_NATIVE_BACKEND_O0: return llvm::OptimizationLevel::O0;
        case JOYEER_NATIVE_BACKEND_O1: return llvm::OptimizationLevel::O1;
        case JOYEER_NATIVE_BACKEND_O2: return llvm::OptimizationLevel::O2;
        case JOYEER_NATIVE_BACKEND_O3: return llvm::OptimizationLevel::O3;
    }
    return llvm::OptimizationLevel::O2;
}

llvm::CodeGenOptLevel codeGenerationLevel(
        JoyeerNativeBackendOptimizationLevel level) {
    switch (level) {
        case JOYEER_NATIVE_BACKEND_O0: return llvm::CodeGenOptLevel::None;
        case JOYEER_NATIVE_BACKEND_O1: return llvm::CodeGenOptLevel::Less;
        case JOYEER_NATIVE_BACKEND_O2: return llvm::CodeGenOptLevel::Default;
        case JOYEER_NATIVE_BACKEND_O3: return llvm::CodeGenOptLevel::Aggressive;
    }
    return llvm::CodeGenOptLevel::Default;
}

std::string sourceDiagnostic(const llvm::SMDiagnostic& diagnostic) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    diagnostic.print("joyeer", stream);
    return message;
}

JoyeerNativeBackendStatus emitObject(
        const JoyeerNativeBackendObjectOptions& options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext) {
    static const bool targetInitializationFailed = [] {
        return llvm::InitializeNativeTarget() ||
                llvm::InitializeNativeTargetAsmPrinter();
    }();
    if (targetInitializationFailed) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INTERNAL_ERROR,
                "LLVM native target initialization failed");
        return JOYEER_NATIVE_BACKEND_INTERNAL_ERROR;
    }

    llvm::LLVMContext context;
    llvm::SMDiagnostic parseDiagnostic;
    auto module = llvm::parseAssemblyString(
            llvm::StringRef(options.llvmIR, options.llvmIRSize),
            parseDiagnostic,
            context);
    if (module == nullptr) {
        const auto message = sourceDiagnostic(parseDiagnostic);
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INVALID_IR,
                message);
        return JOYEER_NATIVE_BACKEND_INVALID_IR;
    }

    std::string verificationMessage;
    llvm::raw_string_ostream verificationStream(verificationMessage);
    if (llvm::verifyModule(*module, &verificationStream)) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INVALID_IR,
                verificationMessage);
        return JOYEER_NATIVE_BACKEND_INVALID_IR;
    }

    const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    std::string targetError;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, targetError);
    if (target == nullptr) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED,
                targetError);
        return JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED;
    }

    llvm::TargetOptions targetOptions;
    std::unique_ptr<llvm::TargetMachine> targetMachine(target->createTargetMachine(
            triple,
            "generic",
            "",
            targetOptions,
            std::nullopt,
            std::nullopt,
            codeGenerationLevel(options.optimizationLevel)));
    if (targetMachine == nullptr) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED,
                "LLVM could not create the native target machine");
        return JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED;
    }
    module->setTargetTriple(triple);
    module->setDataLayout(targetMachine->createDataLayout());

    llvm::LoopAnalysisManager loopAnalyses;
    llvm::FunctionAnalysisManager functionAnalyses;
    llvm::CGSCCAnalysisManager cgsccAnalyses;
    llvm::ModuleAnalysisManager moduleAnalyses;
    llvm::PassBuilder passBuilder(targetMachine.get());
    passBuilder.registerModuleAnalyses(moduleAnalyses);
    passBuilder.registerCGSCCAnalyses(cgsccAnalyses);
    passBuilder.registerFunctionAnalyses(functionAnalyses);
    passBuilder.registerLoopAnalyses(loopAnalyses);
    passBuilder.crossRegisterProxies(
            loopAnalyses,
            functionAnalyses,
            cgsccAnalyses,
            moduleAnalyses);
    const auto requestedOptimization = optimizationLevel(options.optimizationLevel);
    auto optimizationPipeline = requestedOptimization == llvm::OptimizationLevel::O0
            ? passBuilder.buildO0DefaultPipeline(requestedOptimization)
            : passBuilder.buildPerModuleDefaultPipeline(requestedOptimization);
    optimizationPipeline.run(*module, moduleAnalyses);

    std::error_code fileError;
    llvm::raw_fd_ostream objectStream(options.outputPath, fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_FILE_ERROR,
                fileError.message());
        return JOYEER_NATIVE_BACKEND_FILE_ERROR;
    }
    llvm::legacy::PassManager codeGenerationPasses;
    if (targetMachine->addPassesToEmitFile(
            codeGenerationPasses,
            objectStream,
            nullptr,
            llvm::CodeGenFileType::ObjectFile)) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED,
                "LLVM target does not support object emission");
        return JOYEER_NATIVE_BACKEND_CODE_GENERATION_FAILED;
    }
    codeGenerationPasses.run(*module);
    objectStream.flush();
    if (objectStream.has_error()) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_FILE_ERROR,
                "LLVM failed while writing the object file");
        return JOYEER_NATIVE_BACKEND_FILE_ERROR;
    }
    return JOYEER_NATIVE_BACKEND_SUCCESS;
}

#if defined(_WIN32)
bool appendWindowsLibraryPaths(
        std::vector<std::string>& arguments,
        std::string& error) {
    auto fileSystem = llvm::vfs::getRealFileSystem();
    std::string toolchainPath;
    llvm::ToolsetLayout toolsetLayout;
    if (!llvm::findVCToolChainViaEnvironment(
                *fileSystem,
                toolchainPath,
                toolsetLayout) &&
        !llvm::findVCToolChainViaSetupConfig(
                *fileSystem,
                std::nullopt,
                toolchainPath,
                toolsetLayout) &&
        !llvm::findVCToolChainViaRegistry(toolchainPath, toolsetLayout)) {
        error = "Microsoft Visual C++ build tools were not found";
        return false;
    }
    arguments.emplace_back(
            "/LIBPATH:" + llvm::getSubDirectoryPath(
                    llvm::SubDirectoryType::Lib,
                    toolsetLayout,
                    toolchainPath,
                    llvm::Triple::x86_64));

    if (llvm::useUniversalCRT(
                toolsetLayout,
                toolchainPath,
                llvm::Triple::x86_64,
                *fileSystem)) {
        std::string universalCRTRoot;
        std::string universalCRTVersion;
        if (!llvm::getUniversalCRTSdkDir(
                    *fileSystem,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    universalCRTRoot,
                    universalCRTVersion)) {
            error = "Universal CRT SDK was not found";
            return false;
        }
        llvm::SmallString<128> universalCRTPath(universalCRTRoot);
        llvm::sys::path::append(
                universalCRTPath,
                "Lib",
                universalCRTVersion,
                "ucrt",
                llvm::archToWindowsSDKArch(llvm::Triple::x86_64));
        arguments.emplace_back("/LIBPATH:" + std::string(universalCRTPath));
    }

    std::string windowsSDKRoot;
    std::string windowsSDKIncludeVersion;
    std::string windowsSDKLibraryVersion;
    int windowsSDKMajor = 0;
    if (!llvm::getWindowsSDKDir(
                *fileSystem,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                windowsSDKRoot,
                windowsSDKMajor,
                windowsSDKIncludeVersion,
                windowsSDKLibraryVersion)) {
        error = "Windows SDK libraries were not found";
        return false;
    }
    llvm::SmallString<128> windowsSDKPath(windowsSDKRoot);
    llvm::sys::path::append(windowsSDKPath, "Lib");
    if (windowsSDKMajor >= 8) {
        llvm::sys::path::append(
                windowsSDKPath,
                windowsSDKLibraryVersion,
                "um");
    }
    std::string windowsSDKArchitecturePath;
    if (!llvm::appendArchToWindowsSDKLibPath(
                windowsSDKMajor,
                windowsSDKPath,
                llvm::Triple::x86_64,
                windowsSDKArchitecturePath)) {
        error = "Windows SDK does not support the x64 target";
        return false;
    }
    arguments.emplace_back("/LIBPATH:" + windowsSDKArchitecturePath);
    return true;
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
std::optional<std::vector<std::string>> buildElfLinkerArguments(
    const JoyeerNativeBackendLinkOptions& options,
    std::string& error) {
    std::string diagnosticsText;
    llvm::raw_string_ostream diagnosticsStream(diagnosticsText);
    clang::DiagnosticOptions diagnosticOptions;
    clang::TextDiagnosticPrinter diagnosticPrinter(
        diagnosticsStream,
        diagnosticOptions);
    auto diagnosticIds = llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(
        new clang::DiagnosticIDs());
    clang::DiagnosticsEngine diagnostics(
        diagnosticIds,
        diagnosticOptions,
        &diagnosticPrinter,
        false);
    clang::driver::Driver driver(
            "/usr/bin/clang",
        llvm::sys::getDefaultTargetTriple(),
        diagnostics,
        "Joyeer native linker");

    const llvm::ArrayRef<const char*> driverArguments(
        options.arguments,
        options.argumentCount);
    std::unique_ptr<clang::driver::Compilation> compilation(
        driver.BuildCompilation(driverArguments));
    diagnosticsStream.flush();
    if (compilation == nullptr || diagnostics.hasErrorOccurred()) {
    error = diagnosticsText.empty()
        ? "Clang Driver could not construct the ELF link command"
        : diagnosticsText;
    return std::nullopt;
    }
    if (compilation->getJobs().size() != 1) {
    error = "Clang Driver did not produce exactly one ELF link command";
    return std::nullopt;
    }

    const auto& command = *compilation->getJobs().begin();
    std::vector<std::string> arguments;
    arguments.reserve(command.getArguments().size() + 1);
    arguments.emplace_back("ld.lld");
    for (const auto* argument : command.getArguments()) {
    arguments.emplace_back(argument);
    }
    return arguments;
}
#endif

JoyeerNativeBackendStatus linkNative(
        const JoyeerNativeBackendLinkOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext) {
    if (options == nullptr ||
        !validHeader(options->abiVersion, options->structSize, sizeof(*options)) ||
        options->arguments == nullptr || options->argumentCount == 0) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
                "invalid native backend linker options");
        return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < options->argumentCount; ++index) {
        if (options->arguments[index] == nullptr) {
            report(
                    diagnosticCallback,
                    diagnosticContext,
                    JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
                    "native backend linker arguments must not contain null entries");
            return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
        }
    }

    try {
        std::lock_guard lock(lldMutex);
        std::vector<std::string> argumentStorage;
        argumentStorage.reserve(options->argumentCount + 3);
        for (size_t index = 0; index < options->argumentCount; ++index) {
            argumentStorage.emplace_back(options->arguments[index]);
        }
#if defined(_WIN32)
        std::string discoveryError;
        if (!appendWindowsLibraryPaths(argumentStorage, discoveryError)) {
            report(
                    diagnosticCallback,
                    diagnosticContext,
                    JOYEER_NATIVE_BACKEND_LINK_FAILED,
                    discoveryError);
            return JOYEER_NATIVE_BACKEND_LINK_FAILED;
        }
#endif
        std::vector<const char*> arguments;
        arguments.reserve(argumentStorage.size());
        for (const auto& argument : argumentStorage) {
            arguments.push_back(argument.c_str());
        }
        std::string standardOutput;
        std::string standardError;
        llvm::raw_string_ostream outputStream(standardOutput);
        llvm::raw_string_ostream errorStream(standardError);
        const lld::DriverDef drivers[] {
            { nativeLinkerFlavor, nativeLinkerDriver },
        };
        const auto linking = lld::lldMain(
                arguments,
                outputStream,
                errorStream,
                drivers);
        if (!linking.canRunAgain) {
            lld::exitLld(linking.retCode == 0 ? 1 : linking.retCode);
        }
        if (linking.retCode == 0) return JOYEER_NATIVE_BACKEND_SUCCESS;
        auto message = standardError;
        if (!standardOutput.empty()) {
            if (!message.empty()) message.push_back('\n');
            message += standardOutput;
        }
        if (message.empty()) message = "LLD failed without a diagnostic";
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_LINK_FAILED,
                message);
        return JOYEER_NATIVE_BACKEND_LINK_FAILED;
    } catch (const std::exception& error) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INTERNAL_ERROR,
                error.what());
    } catch (...) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INTERNAL_ERROR,
                "unknown LLD failure");
    }
    return JOYEER_NATIVE_BACKEND_INTERNAL_ERROR;
}

} // namespace

extern "C" uint32_t joyeer_native_backend_abi_version(void) {
    return JOYEER_NATIVE_BACKEND_ABI_VERSION;
}

extern "C" const char* joyeer_native_backend_llvm_version(void) {
    return LLVM_VERSION_STRING;
}

extern "C" int joyeer_native_backend_has_coff_linker(void) {
#if defined(_WIN32)
    return nativeLinkerDriver != nullptr;
#else
    return 0;
#endif
}

extern "C" int joyeer_native_backend_has_macho_linker(void) {
#if defined(__APPLE__)
    return nativeLinkerDriver != nullptr;
#else
    return 0;
#endif
}

extern "C" JoyeerNativeBackendStatus joyeer_native_backend_emit_object(
        const JoyeerNativeBackendObjectOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext) {
    if (options == nullptr ||
        !validHeader(options->abiVersion, options->structSize, sizeof(*options)) ||
        options->llvmIR == nullptr || options->outputPath == nullptr ||
        options->llvmIRSize == 0 || options->outputPath[0] == '\0') {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
                "invalid native backend object options");
        return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
    }
    try {
        return emitObject(*options, diagnosticCallback, diagnosticContext);
    } catch (const std::exception& error) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INTERNAL_ERROR,
                error.what());
    } catch (...) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INTERNAL_ERROR,
                "unknown native backend failure");
    }
    return JOYEER_NATIVE_BACKEND_INTERNAL_ERROR;
}

extern "C" JoyeerNativeBackendStatus joyeer_native_backend_link_coff(
        const JoyeerNativeBackendLinkOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext) {
#if defined(_WIN32)
    return linkNative(options, diagnosticCallback, diagnosticContext);
#else
    report(
            diagnosticCallback,
            diagnosticContext,
            JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
            "COFF linking is unavailable in this native backend");
    return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
#endif
}

extern "C" JoyeerNativeBackendStatus joyeer_native_backend_link_macho(
        const JoyeerNativeBackendLinkOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext) {
#if defined(__APPLE__)
    return linkNative(options, diagnosticCallback, diagnosticContext);
#else
    report(
            diagnosticCallback,
            diagnosticContext,
            JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
            "Mach-O linking is unavailable in this native backend");
    return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
#endif
}

extern "C" int joyeer_native_backend_has_elf_linker(void) {
#if !defined(_WIN32) && !defined(__APPLE__)
    return nativeLinkerDriver != nullptr;
#else
    return 0;
#endif
}

extern "C" JoyeerNativeBackendStatus joyeer_native_backend_link_elf(
        const JoyeerNativeBackendLinkOptions* options,
        JoyeerNativeBackendDiagnosticCallback diagnosticCallback,
        void* diagnosticContext) {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (options == nullptr ||
        !validHeader(options->abiVersion, options->structSize, sizeof(*options)) ||
        options->arguments == nullptr || options->argumentCount == 0) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
                "invalid native backend ELF linker options");
        return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < options->argumentCount; ++index) {
        if (options->arguments[index] == nullptr) {
            report(
                    diagnosticCallback,
                    diagnosticContext,
                    JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
                    "native backend ELF linker arguments must not contain null entries");
            return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
        }
    }

    std::string driverError;
    const auto linkerArguments = buildElfLinkerArguments(*options, driverError);
    if (!linkerArguments.has_value()) {
        report(
                diagnosticCallback,
                diagnosticContext,
                JOYEER_NATIVE_BACKEND_LINK_FAILED,
                driverError);
        return JOYEER_NATIVE_BACKEND_LINK_FAILED;
    }
    std::vector<const char*> rawArguments;
    rawArguments.reserve(linkerArguments->size());
    for (const auto& argument : *linkerArguments) {
        rawArguments.push_back(argument.c_str());
    }
    const JoyeerNativeBackendLinkOptions linkerOptions {
        JOYEER_NATIVE_BACKEND_ABI_VERSION,
        sizeof(JoyeerNativeBackendLinkOptions),
        rawArguments.data(),
        rawArguments.size(),
    };
    return linkNative(&linkerOptions, diagnosticCallback, diagnosticContext);
#else
    report(
            diagnosticCallback,
            diagnosticContext,
            JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT,
            "ELF linking is unavailable in this native backend");
    return JOYEER_NATIVE_BACKEND_INVALID_ARGUMENT;
#endif
}