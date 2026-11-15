// Builds an executable from a Joyeer program whose array library is supplied as a source file, through the
// collections-prelude test seam of decision 0026. It exists so the cost
// of a library array (decision 0026) can be measured before the library array exists in the product. Not a
// test and not part of the product; see tests/perf/README.md.
//
//   array-addressor-probe <program.joyeer> <library.joyeer> <output> [--O0|--O1|--O2|--O3] [--lines] [--full]
//                         [--emit-llvm <file>]
#include "compiler/codegen/linker.h"
#include "compiler/codegen/llvm.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/ir/irlowering.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

joyeer::ir::SourceInfo info(const SourceFile::Ptr& file, const std::string& name) {
    return joyeer::ir::SourceInfo { name, ".", file->content.size(), file->lineStarts };
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: array-addressor-probe <program> <library> <output> [--O0..--O3] [--lines] [--full]\n";
        return 2;
    }
    try {
        const std::filesystem::path programPath = argv[1];
        const std::filesystem::path libraryPath = argv[2];
        const std::filesystem::path output = argv[3];
        auto level = joyeer::OptimizationLevel::O2;
        joyeer::DebugInfoOptions debug;
        std::filesystem::path emitLlvm;
        for (int index = 4; index < argc; ++index) {
            const std::string flag = argv[index];
            if (flag == "--O0") level = joyeer::OptimizationLevel::O0;
            else if (flag == "--O1") level = joyeer::OptimizationLevel::O1;
            else if (flag == "--O2") level = joyeer::OptimizationLevel::O2;
            else if (flag == "--O3") level = joyeer::OptimizationLevel::O3;
            else if (flag == "--lines") debug.emitLineTables = true;
            else if (flag == "--full") { debug.emitLineTables = true; debug.emitVariables = true; }
            else if (flag == "--emit-llvm" && index + 1 < argc) emitLlvm = argv[++index];
            else throw std::runtime_error("unknown option " + flag);
        }

        Diagnostics lexerDiagnostics;
        LexParser lexer(&lexerDiagnostics);

        const auto program = std::make_shared<SourceFile>(readFile(programPath));
        lexer.parse(program);
        const auto programParse = joyeer::parser::Parser(program->tokens).parse();
        if (!programParse.succeeded()) throw std::runtime_error(joyeer::parser::dump(programParse.diagnostics));

        const auto library = std::make_shared<SourceFile>(readFile(libraryPath));
        lexer.parse(library);
        for (auto& token : library->tokens) token->span.sourceId = 1;
        const auto libraryParse = joyeer::parser::Parser(library->tokens, true, true).parse();
        if (!libraryParse.succeeded()) throw std::runtime_error(joyeer::parser::dump(libraryParse.diagnostics));
        if (!lexerDiagnostics.errors.empty()) throw std::runtime_error(lexerDiagnostics.errors.front().message);

        joyeer::semantic::ModuleInput libraryModule { "$collections-prelude", { libraryParse.root }, true, false, true,
                                                      joyeer::semantic::ReplacedPrelude::collections };
        const auto resolution = joyeer::semantic::NameResolver().resolve({
                { "app", { programParse.root } }, libraryModule });
        if (!resolution.succeeded()) throw std::runtime_error(joyeer::semantic::dump(resolution.diagnostics));
        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        if (!checking.succeeded()) throw std::runtime_error(joyeer::typing::dump(checking.diagnostics));
        const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
        if (!analysis.succeeded()) throw std::runtime_error(joyeer::analysis::dump(analysis.diagnostics));

        std::vector<joyeer::ir::SourceInfo> sources { info(program, programPath.filename().string()),
                                                      info(library, libraryPath.filename().string()) };
        for (const auto& prelude : checking.model->semanticModel()->preludeSources()) {
            sources.push_back(info(prelude, "prelude.joyeer"));
        }
        const auto lowering = joyeer::lowering::Lowerer().lower(
                checking.model, programPath.string(), info(program, programPath.filename().string()), sources);
        if (!lowering.succeeded()) throw std::runtime_error(joyeer::lowering::dump(lowering.diagnostics));

        const auto emitted = joyeer::llvmbackend::Emitter().emit(
                *lowering.module,
                joyeer::llvmbackend::EmitOptions { debug.emitLineTables, debug.format, level, debug.emitVariables,
                                                   !emitLlvm.empty() });
        if (!emitted.succeeded()) throw std::runtime_error(joyeer::llvmbackend::dump(emitted.diagnostics));
        if (!emitLlvm.empty()) std::ofstream(emitLlvm, std::ios::binary) << emitted.text;

        joyeer::native::LinkOptions options;
        options.runtimeLibrary = std::filesystem::u8path(JOYEER_PROBE_RUNTIME_LIBRARY);
        options.outputFile = output;
        options.optimizationLevel = level;
        options.debugInfo = debug;
        options.sourceFile = programPath;
        options.module = emitted.module;
        const auto linked = joyeer::native::Linker().link(emitted.text, emitted.hasEntryPoint, options);
        if (!linked.succeeded()) throw std::runtime_error(joyeer::native::dump(linked.diagnostics));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "array-addressor-probe: " << error.what() << "\n";
        return 1;
    }
}
