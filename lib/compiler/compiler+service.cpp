#include "joyeer/compiler/compiler+service.h"

#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/semanticanalysis.h"
#include "joyeer/backend/llvm.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <utility>

namespace {

std::filesystem::path normalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (!error) return result;
    error.clear();
    result = std::filesystem::absolute(path, error);
    return (error ? path : result).lexically_normal();
}

bool pathsAlias(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) return true;
#if defined(_WIN32)
    const auto key = [](const std::filesystem::path& path) {
        auto text = normalizedPath(path).native();
        std::replace(text.begin(), text.end(), L'/', L'\\');
        std::wstring result;
        size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find(L'\\', start);
            auto component = text.substr(start, end == std::wstring::npos ? text.size() - start : end - start);
            if (!component.empty() && component.back() != L':') {
                while (!component.empty() && (component.back() == L' ' || component.back() == L'.')) {
                    component.pop_back();
                }
            }
            std::transform(component.begin(), component.end(), component.begin(),
                    [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
            if (start != 0) result.push_back(L'\\');
            result += component;
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return result;
    };
    return key(left) == key(right);
#else
    return normalizedPath(left) == normalizedPath(right);
#endif
}

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
    const std::vector<SourceFile::Ptr>& sources,
    SourceSpan span,
    ErrorLevel level,
    std::string code,
    const std::string& message,
    std::optional<std::string> help = std::nullopt,
    std::optional<DiagnosticFixIt> fixIt = std::nullopt,
    std::vector<DiagnosticSourceNote> notes = {}) {
    const auto& sourcefile = sources.at(span.sourceId);
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
    sourceFiles.clear();
    compilationSources.clear();
    lastCompiledSourceFile.reset();
    if (!options.moduleName.empty() || !options.sourceFiles.empty()) {
        if (!inputFile.empty()) {
            diagnostics->reportDiagnostic(ErrorLevel::failure, "module.conflicting-input",
                    "explicit compilation-unit sources and the single-file compile argument are mutually exclusive");
            return;
        }
        compileModules();
        return;
    }
    if (!options.modules.empty()) {
        diagnostics->reportDiagnostic(ErrorLevel::failure, "module.missing-root",
                "dependency source sets require a named root compilation unit");
        return;
    }
    auto sourcefile = findSourceFile(inputFile);
    lastCompiledSourceFile = sourcefile;
    if (!protectInputs({sourcefile->getAbstractPath()})) return;
    auto root = parse(sourcefile);
    if (root == nullptr) return;
    for (const auto& import : root->imports) {
        reportSpannedDiagnostic(diagnostics, compilationSources, import.span, ErrorLevel::failure,
                "module.unknown-import", "unknown module '" + import.name + "'; use --module-name and --module-source");
    }
    CHECK_ERROR_RETURN
    compile({joyeer::semantic::ModuleInput {"", {root}}});
}

bool CompilerService::protectInputs(const std::vector<std::filesystem::path>& inputs) {
    if (options.outputMode == joyeer::OutputMode::validate) return true;
#if defined(_WIN32)
    if (options.outputFile.filename().native().find(L':') != std::wstring::npos) {
        diagnostics->reportDiagnostic(ErrorLevel::failure, "driver.output-file-error",
                "output paths must not use Windows alternate data streams");
        return false;
    }
#endif
    auto pdb = options.outputFile;
    pdb.replace_extension(".pdb");
    for (const auto& input : inputs) {
        if (pathsAlias(input, options.outputFile)) {
            diagnostics->reportDiagnostic(ErrorLevel::failure, "driver.output-file-error",
                    "output path must not overwrite input file '" + input.string() + "'");
            return false;
        }
#if defined(_WIN32)
        if (options.outputMode == joyeer::OutputMode::executable && pathsAlias(input, pdb)) {
            diagnostics->reportDiagnostic(ErrorLevel::failure, "driver.output-file-error",
                    "sibling PDB cleanup path must not overwrite input file '" + input.string() + "'");
            return false;
        }
#endif
    }
    return true;
}

void CompilerService::compileModules() {
    struct Module {
        std::string name;
        std::vector<std::filesystem::path> files;
        joyeer::semantic::ModuleInput syntax;
        enum State { unseen, visiting, done } state = unseen;
    };
    std::vector<Module> graph;
    std::vector<std::filesystem::path> allInputs;
    std::unordered_map<std::string, size_t> names;
    auto sourceSets = options.modules;
    sourceSets.insert(sourceSets.begin(), {options.moduleName, options.sourceFiles});
    for (const auto& sourceSet : sourceSets) {
        if (!joyeer::isModuleName(sourceSet.name)) {
            diagnostics->reportDiagnostic(ErrorLevel::failure, "module.invalid-name",
                    "invalid logical module name '" + sourceSet.name + "'");
            return;
        }
        if (names.contains(sourceSet.name)) {
            diagnostics->reportDiagnostic(ErrorLevel::failure, "module.duplicate-name",
                    "duplicate module source set '" + sourceSet.name + "'");
            return;
        }
        if (sourceSet.files.empty()) {
            diagnostics->reportDiagnostic(ErrorLevel::failure, "module.empty",
                    "module '" + sourceSet.name + "' has no source files");
            return;
        }
        Module module {sourceSet.name, {}, {sourceSet.name, {}}};
        for (const auto& input : sourceSet.files) {
            if (input.empty()) {
                diagnostics->reportDiagnostic(ErrorLevel::failure, "module.read-source",
                        "module '" + sourceSet.name + "' contains an empty source path");
                return;
            }
            std::error_code error;
            const auto path = input.is_relative() ? options.workingDirectory / input : input;
            const auto file = std::filesystem::canonical(path, error);
            if (error) {
                diagnostics->reportDiagnostic(ErrorLevel::failure, "module.read-source",
                        "cannot resolve source file '" + path.string() +
                        "' for module '" + sourceSet.name + "': " + error.message());
                return;
            }
            const bool regular = std::filesystem::is_regular_file(file, error);
            if (error || !regular) {
                diagnostics->reportDiagnostic(ErrorLevel::failure, "module.read-source",
                        "module '" + sourceSet.name + "' requires a regular source file: '" +
                        path.string() + "'" + (error ? ": " + error.message() : ""));
                return;
            }
            if (std::any_of(allInputs.begin(), allInputs.end(),
                    [&](const auto& earlier) { return pathsAlias(earlier, file); })) {
                diagnostics->reportDiagnostic(ErrorLevel::failure, "module.duplicate-source",
                        "source file is supplied more than once: '" + file.string() + "'");
                return;
            }
            allInputs.push_back(file);
            module.files.push_back(file);
        }
        std::sort(module.files.begin(), module.files.end());
        names.emplace(sourceSet.name, graph.size());
        graph.push_back(std::move(module));
    }
    if (!protectInputs(allInputs)) return;

    std::vector<size_t> stack;
    std::vector<joyeer::semantic::ModuleInput> inputs;
    std::function<bool(size_t)> visit = [&](size_t index) {
        auto& module = graph[index];
        if (module.state == Module::done) return true;
        module.state = Module::visiting;
        stack.push_back(index);
        for (const auto& path : module.files) {
            auto file = findSourceFile(path);
            if (lastCompiledSourceFile == nullptr) lastCompiledSourceFile = file;
            auto syntax = parse(file);
            if (syntax == nullptr) return false;
            module.syntax.files.push_back(std::move(syntax));
        }
        // Keep the root first while still discovering only reachable dependencies.
        inputs.push_back(module.syntax);
        for (const auto& file : module.syntax.files) {
            for (const auto& import : file->imports) {
                const auto found = names.find(import.name);
                if (found == names.end()) {
                    reportSpannedDiagnostic(diagnostics, compilationSources, import.span, ErrorLevel::failure,
                            "module.unknown-import", "unknown module '" + import.name + "'; provide --module-source name=file inputs");
                    return false;
                }
                if (graph[found->second].state == Module::visiting) {
                    std::ostringstream cycle;
                    const auto begin = std::find(stack.begin(), stack.end(), found->second);
                    for (auto item = begin; item != stack.end(); ++item) {
                        cycle << (graph[*item].name.empty() ? "<root>" : graph[*item].name) << " -> ";
                    }
                    cycle << import.name;
                    reportSpannedDiagnostic(diagnostics, compilationSources, import.span, ErrorLevel::failure,
                            "module.import-cycle", "module dependency cycle: " + cycle.str());
                    return false;
                }
                if (!visit(found->second)) return false;
            }
        }
        stack.pop_back();
        module.state = Module::done;
        return true;
    };
    if (visit(0)) compile(inputs);
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

    const auto sourcefilePath = normalizedPath(sourcefile);
    if(sourceFiles.find(sourcefilePath) == sourceFiles.end()) {
        auto sf = std::make_shared<SourceFile>(options.workingDirectory, sourcefilePath);
        sourceFiles.insert({sourcefilePath, sf});
    }

    return sourceFiles.find(sourcefilePath)->second;
}


joyeer::syntax::SourceFileSyntax::Ptr CompilerService::parse(const SourceFile::Ptr& sourcefile) {
    if (!sourcefile->loaded()) {
        const auto tooLarge = sourcefile->loadingError() == SourceFile::LoadError::sourceTooLarge;
        diagnostics->reportDiagnostic(
                ErrorLevel::failure,
                tooLarge ? "lexer.source-too-large" : "source-file.read-failed",
                tooLarge
                        ? Diagnostics::errorSourceTooLarge
                        : "cannot read source file '" + sourcefile->getLocation() + "'");
        return nullptr;
    }

    LexParser lexParser(diagnostics);
    lexParser.parse(sourcefile);
    if (diagnostics->hasFailure()) return nullptr;
    if (compilationSources.size() > std::numeric_limits<uint32_t>::max()) {
        diagnostics->reportDiagnostic(ErrorLevel::failure, "module.too-many-sources",
                "module graph exceeds the source-file identity limit");
        return nullptr;
    }
    const auto sourceId = static_cast<uint32_t>(compilationSources.size());
    compilationSources.push_back(sourcefile);
    for (auto& token : sourcefile->tokens) token->span.sourceId = sourceId;

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
                compilationSources,
                diagnostic.span,
                ErrorLevel::failure,
                joyeer::parser::diagnosticName(diagnostic.id),
                diagnostic.message,
                diagnostic.help,
                diagnostic.fixIt);
    }
    if (!result.succeeded()) {
        return nullptr;
    }
    return result.root;
}

void CompilerService::compile(const std::vector<joyeer::semantic::ModuleInput>& modules) {
    const auto& sourcefile = lastCompiledSourceFile;
    const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
    sourcefile->semanticModel = resolution.model;
    for (const auto& diagnostic : resolution.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                compilationSources,
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
                compilationSources,
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
                compilationSources,
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

    std::vector<joyeer::ir::SourceInfo> sources;
    for (const auto& source : compilationSources) sources.push_back(sourceInfoFor(source));
    const auto lowering = joyeer::lowering::Lowerer().lower(
            checking.model,
            sourcefile->getLocation(),
            sourceInfoFor(sourcefile),
            std::move(sources));
    sourcefile->joyeerIR = lowering.module;
    for (const auto& diagnostic : lowering.diagnostics) {
        reportSpannedDiagnostic(
                diagnostics,
                compilationSources,
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
                compilationSources,
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
