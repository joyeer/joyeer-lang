#include "joyeer/compiler/compiler+service.h"

#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/semanticanalysis.h"
#include "joyeer/backend/llvm.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace {

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(
            reinterpret_cast<const char*>(encoded.data()),
            encoded.size());
}

joyeer::ir::SourceInfo sourceInfoFor(const SourceFile::Ptr& sourcefile) {
    std::error_code error;
    auto sourcePath = std::filesystem::absolute(
            sourcefile->getAbstractPath(),
            error);
    if (error) {
        sourcePath = sourcefile->getAbstractPath();
    }
    sourcePath = sourcePath.lexically_normal();
    return joyeer::ir::SourceInfo {
        pathUtf8(sourcePath.filename()),
        pathUtf8(sourcePath.parent_path()),
        static_cast<uint64_t>(sourcefile->content.size()),
        sourcefile->lineStarts,
    };
}

void reportSpannedDiagnostic(
    Diagnostics* diagnostics,
    const SourceFile::Ptr& sourcefile,
    SourceSpan span,
    ErrorLevel level,
    std::string code,
    const std::string& message,
    std::optional<std::string> help = std::nullopt,
    std::optional<DiagnosticFixIt> fixIt = std::nullopt,
    std::vector<DiagnosticSourceNote> notes = {}) {
    diagnostics->reportSourceDiagnostic(
        level,
        std::move(code),
        sourcefile->getLocation(),
        sourcefile->content,
        sourcefile->lineStarts,
        span.offset,
        span.length,
        message,
        std::move(help),
        std::move(fixIt),
        std::move(notes));
}

} // namespace

#define CHECK_ERROR_RETURN \
    if(diagnostics->hasFailure()) { \
        return; \
    }

CompilerService::CompilerService(Diagnostics* diagnostics, joyeer::CompileOptions opts):
        options(std::move(opts)) {
    this->diagnostics = diagnostics;
}

void CompilerService::compile(const std::filesystem::path& inputFile) {
    auto sourcefile = findSourceFile(inputFile);
    lastCompiledSourceFile = sourcefile;
    compile(sourcefile);
}

SourceFile::Ptr CompilerService::findSourceFile(
        const std::filesystem::path& path,
        const std::filesystem::path& relativeFolder) {
    auto sourcefile = path;
    if(sourcefile.is_relative()) {
        // check the relative folder
        auto target = std::filesystem::path(relativeFolder) / path;
        if(std::filesystem::exists(target)) {
            sourcefile = target;
        }
    }

    std::error_code error;
    auto sourcefilePath = std::filesystem::absolute(sourcefile, error);
    if (error) sourcefilePath = sourcefile;
    sourcefilePath = sourcefilePath.lexically_normal();
    if(sourceFiles.find(sourcefilePath) == sourceFiles.end()) {
        auto sf = std::make_shared<SourceFile>(options.workingDirectory, sourcefilePath);
        sourceFiles.insert({sourcefilePath, sf});
    }

    return sourceFiles.find(sourcefilePath)->second;
}


void CompilerService::compile(const SourceFile::Ptr& sourcefile) {
    if (!sourcefile->loaded()) {
        const auto tooLarge = sourcefile->loadingError() == SourceFile::LoadError::sourceTooLarge;
        diagnostics->reportDiagnostic(
                ErrorLevel::failure,
                tooLarge ? "lexer.source-too-large" : "source-file.read-failed",
                tooLarge
                        ? Diagnostics::errorSourceTooLarge
                        : "cannot read source file '" + sourcefile->getLocation() + "'");
        return;
    }

    LexParser lexParser(diagnostics);
    lexParser.parse(sourcefile);
    CHECK_ERROR_RETURN

    sourcefile->semanticModel.reset();
    sourcefile->typeCheckedModel.reset();
    sourcefile->joyeerIR.reset();
    sourcefile->llvmIR.clear();
    sourcefile->llvmHasEntryPoint = false;
    joyeer::parser::Parser parser(sourcefile->tokens);
    auto result = parser.parse();
    for(const auto& diagnostic : result.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                sourcefile,
                diagnostic.span,
                ErrorLevel::failure,
                joyeer::parser::diagnosticName(diagnostic.id),
                diagnostic.message,
                diagnostic.help,
                diagnostic.fixIt);
    }
    if (!result.succeeded()) {
        return;
    }

    const auto resolution = joyeer::semantic::NameResolver().resolve(result.root);
    sourcefile->semanticModel = resolution.model;
    for (const auto& diagnostic : resolution.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                sourcefile,
                diagnostic.span,
                ErrorLevel::failure,
                joyeer::semantic::diagnosticName(diagnostic.id),
                diagnostic.message);
    }
    if (!resolution.succeeded()) {
        return;
    }

    const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
    sourcefile->typeCheckedModel = checking.model;
    for (const auto& diagnostic : checking.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                sourcefile,
                diagnostic.span,
                ErrorLevel::failure,
                joyeer::typing::diagnosticName(diagnostic.id),
                    diagnostic.message,
                    diagnostic.help,
                diagnostic.fixIt,
                diagnostic.notes);
    }
    if (!checking.succeeded()) {
        return;
    }

    const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    for (const auto& diagnostic : analysis.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                sourcefile,
                diagnostic.span,
                diagnostic.severity == joyeer::analysis::Severity::warning
                    ? ErrorLevel::report
                    : ErrorLevel::failure,
                joyeer::analysis::diagnosticName(diagnostic.id),
                diagnostic.message,
                diagnostic.help);
    }
    if (!analysis.succeeded()) {
        return;
    }

    const auto lowering = joyeer::lowering::Lowerer().lower(
            checking.model,
            sourcefile->getLocation(),
            sourceInfoFor(sourcefile));
    sourcefile->joyeerIR = lowering.module;
    for (const auto& diagnostic : lowering.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                sourcefile,
                diagnostic.span,
                ErrorLevel::failure,
                joyeer::lowering::diagnosticName(diagnostic.id),
                diagnostic.message);
    }
    if (!lowering.succeeded()) {
        return;
    }

    const auto llvm = joyeer::llvmbackend::Emitter().emit(
            *lowering.module,
            joyeer::llvmbackend::EmitOptions {
                options.debugInfo.emitLineTables,
                options.debugInfo.format,
                options.optimizationLevel,
                options.debugInfo.emitVariables,
            });
    sourcefile->llvmIR = llvm.text;
    sourcefile->llvmHasEntryPoint = llvm.hasEntryPoint;
    for (const auto& diagnostic : llvm.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                sourcefile,
                diagnostic.span,
                ErrorLevel::failure,
                joyeer::llvmbackend::diagnosticName(diagnostic.id),
                diagnostic.message);
    }
    if (!llvm.succeeded()) {
        return;
    }
    if (options.outputMode == joyeer::OutputMode::llvmIR) {
        std::ofstream output(options.outputFile, std::ios::binary);
        output << llvm.text;
        if (!output.good()) {
            diagnostics->reportDiagnostic(
                        ErrorLevel::failure,
                        "driver.output-file-error",
                        "cannot write LLVM IR output: " +
                        options.outputFile.string());
        }
    }
}
